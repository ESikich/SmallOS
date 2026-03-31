#include "elf_loader.h"
#include "elf.h"
#include "paging.h"
#include "memory.h"
#include "pmm.h"
#include "ramdisk.h"
#include "terminal.h"
#include "gdt.h"
#include "keyboard.h"
#include <setjmp.h>

typedef unsigned char u8;

/* ------------------------------------------------------------------ */
/* Process exit mechanism                                             */
/* ------------------------------------------------------------------ */

static jmp_buf  s_exit_ctx;
static u32*     s_current_pd;
static u32      s_kernel_stack_frame;  /* PMM frame used as kernel stack */
static int      s_process_running = 0;

int elf_process_running(void) {
    return s_process_running;
}

void elf_process_exit(void) {
    s_process_running = 0;
    keyboard_set_process_mode(0);

    paging_switch(paging_get_kernel_pd());

    if (s_current_pd) {
        process_pd_destroy(s_current_pd);
        s_current_pd = 0;
    }

    if (s_kernel_stack_frame) {
        pmm_free_frame(s_kernel_stack_frame);
        s_kernel_stack_frame = 0;
    }

    longjmp(s_exit_ctx, 1);
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static void mem_copy(u8* dst, const u8* src, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) dst[i] = src[i];
}

static void mem_zero(u8* dst, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) dst[i] = 0;
}

static int str_len(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/*
 * elf_enter_ring3(entry, user_esp, argc, argv)
 *
 * Copies argv strings into the user stack (ring-3 accessible), builds a
 * cdecl call frame, then irets into ring 3 at the ELF entry point.
 * Does not return.
 *
 * Stack layout built here (addresses descending from USER_STACK_TOP):
 *
 *   [strings]          argv[0..argc-1] contents, null-terminated
 *   [padding]          align to 4 bytes
 *   [user_argv[0..n]]  pointer array (ring-3 virtual addresses)
 *   [argv ptr]         pointer to user_argv  \
 *   [argc value]       argc                   > cdecl call frame
 *   [return addr = 0]  fake return address   /   ← ESP after iret
 */
static void elf_enter_ring3(unsigned int entry,
                             unsigned int user_esp,
                             int          argc,
                             char**       argv)
{
    /* --- copy argv strings onto user stack --- */
    char* sp = (char*)user_esp;

    char* user_argv_ptrs[32];
    for (int i = argc - 1; i >= 0; i--) {
        int len = str_len(argv[i]) + 1;
        sp -= len;
        mem_copy((u8*)sp, (const u8*)argv[i], len);
        user_argv_ptrs[i] = sp;
    }

    /* align down to 4 bytes */
    sp = (char*)((unsigned int)sp & ~3u);

    /* --- write argv pointer array --- */
    sp -= (argc + 1) * 4;
    unsigned int* user_argv = (unsigned int*)sp;
    for (int i = 0; i < argc; i++)
        user_argv[i] = (unsigned int)user_argv_ptrs[i];
    user_argv[argc] = 0;

    /* --- build cdecl call frame --- */
    unsigned int* frame = (unsigned int*)sp;
    frame[-1] = (unsigned int)user_argv;  /* argv */
    frame[-2] = (unsigned int)argc;       /* argc */
    frame[-3] = 0;                        /* return address */
    unsigned int final_esp = (unsigned int)frame - 12;

    unsigned int user_cs = SEG_USER_CODE;
    unsigned int user_ds = SEG_USER_DATA;

    __asm__ __volatile__ (
        "mov  %3, %%eax     \n"
        "mov  %%ax, %%ds    \n"
        "mov  %%ax, %%es    \n"
        "mov  %%ax, %%fs    \n"
        "mov  %%ax, %%gs    \n"

        "push %3            \n"   /* SS  */
        "push %1            \n"   /* ESP */
        "pushf              \n"   /* EFLAGS */
        "push %2            \n"   /* CS  */
        "push %0            \n"   /* EIP */
        "iret               \n"

        :
        : "r"(entry), "r"(final_esp), "r"(user_cs), "r"(user_ds)
        : "eax"
    );

    __builtin_unreachable();
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int elf_run_image(const unsigned char* image, int argc, char** argv) {
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)image;

    if (*(const unsigned int*)eh->e_ident != ELF_MAGIC) {
        terminal_puts("elf: bad magic\n");
        return 0;
    }

    /* 1. Create process page directory. */
    u32* pd = process_pd_create();
    s_current_pd = pd;

    /* 2. Map PT_LOAD segments. */
    const Elf32_Phdr* ph = (const Elf32_Phdr*)(image + eh->e_phoff);

    for (unsigned short i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;

        u32 seg_start = ph[i].p_vaddr;
        u32 seg_end   = ph[i].p_vaddr + ph[i].p_memsz;
        u32 map_start = seg_start & ~(PAGE_SIZE - 1);
        u32 map_end   = (seg_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        u32 pages     = (map_end - map_start) / PAGE_SIZE;

        for (u32 p = 0; p < pages; p++) {
            /*
             * Allocate user ELF frames from the PMM so they can be
             * reclaimed by process_pd_destroy() on exit.
             */
            u32 page_virt  = map_start + p * PAGE_SIZE;
            u32 frame_phys = pmm_alloc_frame();
            if (!frame_phys) {
                terminal_puts("elf: out of frames\n");
                process_pd_destroy(pd);
                s_current_pd = 0;
                return 0;
            }

            /*
             * Frames are identity-mapped in the kernel address space,
             * so we can zero them directly before the process CR3 is active.
             */
            mem_zero((u8*)frame_phys, PAGE_SIZE);
            paging_map_page(pd, page_virt, frame_phys,
                            PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        }

        /*
         * Copy file-backed bytes into the mapped frames.
         * Important: copy starts at p_vaddr, which may be in the middle
         * of the first page.
         */
        {
            u32 filesz    = ph[i].p_filesz;
            const u8* src = image + ph[i].p_offset;
            u32 copied    = 0;

            while (copied < filesz) {
                u32 va       = seg_start + copied;
                u32 pd_idx   = va >> 22;
                u32 pt_idx   = (va >> 12) & 0x3FF;
                u32 page_off = va & 0xFFFu;

                u32* pt = (u32*)(pd[pd_idx] & ~0xFFFu);
                u8*  dst_frame = (u8*)(pt[pt_idx] & ~0xFFFu);

                u32 chunk = PAGE_SIZE - page_off;
                if (copied + chunk > filesz) {
                    chunk = filesz - copied;
                }

                mem_copy(dst_frame + page_off, src + copied, chunk);
                copied += chunk;
            }
        }
    }

    /* 3. Map user stack — also from the PMM. */
    u32 stack_virt       = USER_STACK_TOP - USER_STACK_SIZE;
    u32 stack_frame_phys = pmm_alloc_frame();
    if (!stack_frame_phys) {
        terminal_puts("elf: out of frames (stack)\n");
        process_pd_destroy(pd);
        s_current_pd = 0;
        return 0;
    }
    mem_zero((u8*)stack_frame_phys, PAGE_SIZE);
    paging_map_page(pd, stack_virt, stack_frame_phys,
                    PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

    /* 4. Allocate a dedicated kernel stack frame for this process.
     *
     * When int 0x80 fires from ring 3 the CPU loads SS0/ESP0 from the TSS.
     * We give every process its own 4 KB kernel stack so that the syscall
     * handler has a clean, private stack that does not overlap the setjmp
     * context saved below.  The frame is freed by elf_process_exit().
     */
    s_kernel_stack_frame = pmm_alloc_frame();
    if (!s_kernel_stack_frame) {
        terminal_puts("elf: out of frames (kernel stack)\n");
        process_pd_destroy(pd);
        s_current_pd = 0;
        return 0;
    }
    /* Stack grows down — ESP0 points one byte past the top of the frame. */
    tss_set_kernel_stack(s_kernel_stack_frame + PAGE_SIZE);

    /* 5. Save kernel context for longjmp return from sys_exit. */
    if (setjmp(s_exit_ctx) != 0) {
        return 1;
    }

    /* 6. Switch to process address space. */
    s_process_running = 1;
    keyboard_set_process_mode(1);
    paging_switch(pd);

    /* 7. iret into ring 3 — does not return. */
    /* (TSS ESP0 already set above, before setjmp) */
    elf_enter_ring3(eh->e_entry, USER_STACK_TOP, argc, argv);

    return 1;
}

int elf_run_named(const char* name, int argc, char** argv) {
    const u8* data = 0;
    u32 size = 0;

    if (!ramdisk_find(name, &data, &size)) {
        terminal_puts("elf: not found in ramdisk: ");
        terminal_puts(name);
        terminal_putc('\n');
        return 0;
    }

    return elf_run_image(data, argc, argv);
}