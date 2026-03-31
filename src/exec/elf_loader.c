#include "elf_loader.h"
#include "elf.h"
#include "paging.h"
#include "memory.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "ramdisk.h"
#include "terminal.h"
#include "gdt.h"
#include "keyboard.h"
#include <setjmp.h>

typedef unsigned char u8;

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

static void elf_enter_ring3(unsigned int entry,
                             unsigned int user_esp,
                             int          argc,
                             char**       argv)
{
    char* sp = (char*)user_esp;
    char* user_argv_ptrs[32];
    for (int i = argc - 1; i >= 0; i--) {
        int len = str_len(argv[i]) + 1;
        sp -= len;
        mem_copy((u8*)sp, (const u8*)argv[i], len);
        user_argv_ptrs[i] = sp;
    }
    sp = (char*)((unsigned int)sp & ~3u);
    sp -= (argc + 1) * 4;
    unsigned int* user_argv = (unsigned int*)sp;
    for (int i = 0; i < argc; i++)
        user_argv[i] = (unsigned int)user_argv_ptrs[i];
    user_argv[argc] = 0;
    unsigned int* frame = (unsigned int*)sp;
    frame[-1] = (unsigned int)user_argv;
    frame[-2] = (unsigned int)argc;
    frame[-3] = 0;
    unsigned int final_esp = (unsigned int)frame - 12;
    unsigned int user_cs = SEG_USER_CODE;
    unsigned int user_ds = SEG_USER_DATA;
    __asm__ __volatile__ (
        "mov  %3, %%eax     \n"
        "mov  %%ax, %%ds    \n"
        "mov  %%ax, %%es    \n"
        "mov  %%ax, %%fs    \n"
        "mov  %%ax, %%gs    \n"
        "push %3            \n"
        "push %1            \n"
        "pushf              \n"
        "push %2            \n"
        "push %0            \n"
        "iret               \n"
        :
        : "r"(entry), "r"(final_esp), "r"(user_cs), "r"(user_ds)
        : "eax"
    );
    __builtin_unreachable();
}

int elf_process_running(void) {
    process_t* proc = process_get_current();
    return proc && proc->state == PROCESS_STATE_RUNNING;
}

void elf_process_exit(void) {
    process_t* proc = process_get_current();
    if (!proc) return;

    proc->state = PROCESS_STATE_EXITED;
    keyboard_set_process_mode(0);

    /* Remove from scheduler before freeing resources. */
    sched_dequeue(proc);

    /* Restore kernel CR3 before destroying the process PD. */
    paging_switch(paging_get_kernel_pd());

    /* Restore TSS ESP0 to the static kernel stack (shell context). */
    tss_set_kernel_stack(0x90000);

    /* Save exit context pointer before process_destroy frees the frame. */
    jmp_buf* ctx = &proc->exit_ctx;

    process_destroy(proc);
    process_set_current(0);

    longjmp(*ctx, 1);
}

int elf_run_image(const unsigned char* image, int argc, char** argv) {
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)image;

    if (*(const unsigned int*)eh->e_ident != ELF_MAGIC) {
        terminal_puts("elf: bad magic\n");
        return 0;
    }

    process_t* proc = process_create("elf");
    if (!proc) return 0;

    proc->pd = process_pd_create();

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
            u32 page_virt  = map_start + p * PAGE_SIZE;
            u32 frame_phys = pmm_alloc_frame();
            if (!frame_phys) {
                terminal_puts("elf: out of frames\n");
                process_destroy(proc);
                process_set_current(0);
                return 0;
            }
            mem_zero((u8*)frame_phys, PAGE_SIZE);
            paging_map_page(proc->pd, page_virt, frame_phys,
                            PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        }

        {
            u32 filesz    = ph[i].p_filesz;
            const u8* src = image + ph[i].p_offset;
            u32 copied    = 0;
            while (copied < filesz) {
                u32 va       = seg_start + copied;
                u32 pd_idx   = va >> 22;
                u32 pt_idx   = (va >> 12) & 0x3FF;
                u32 page_off = va & 0xFFFu;
                u32* pt = (u32*)(proc->pd[pd_idx] & ~0xFFFu);
                u8*  dst_frame = (u8*)(pt[pt_idx] & ~0xFFFu);
                u32 chunk = PAGE_SIZE - page_off;
                if (copied + chunk > filesz) chunk = filesz - copied;
                mem_copy(dst_frame + page_off, src + copied, chunk);
                copied += chunk;
            }
        }
    }

    u32 stack_virt       = USER_STACK_TOP - USER_STACK_SIZE;
    u32 stack_frame_phys = pmm_alloc_frame();
    if (!stack_frame_phys) {
        terminal_puts("elf: out of frames (stack)\n");
        process_destroy(proc);
        process_set_current(0);
        return 0;
    }
    mem_zero((u8*)stack_frame_phys, PAGE_SIZE);
    paging_map_page(proc->pd, stack_virt, stack_frame_phys,
                    PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

    proc->kernel_stack_frame = pmm_alloc_frame();
    if (!proc->kernel_stack_frame) {
        terminal_puts("elf: out of frames (kernel stack)\n");
        process_destroy(proc);
        process_set_current(0);
        return 0;
    }
    tss_set_kernel_stack(proc->kernel_stack_frame + PAGE_SIZE);

    process_set_current(proc);
    proc->state = PROCESS_STATE_RUNNING;

    sched_enqueue(proc);

    if (setjmp(proc->exit_ctx) != 0) {
        return 1;
    }

    keyboard_set_process_mode(1);
    paging_switch(proc->pd);
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