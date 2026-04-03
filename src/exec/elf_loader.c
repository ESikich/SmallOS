#include "elf_loader.h"
#include "../kernel/elf.h"
#include "../kernel/paging.h"
#include "../kernel/memory.h"
#include "../kernel/pmm.h"
#include "../kernel/process.h"
#include "../kernel/scheduler.h"
#include "../drivers/fat16.h"
#include "../drivers/terminal.h"
#include "../kernel/gdt.h"

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
    for (int i = 0; i < argc; i++) {
        user_argv[i] = (unsigned int)user_argv_ptrs[i];
    }
    user_argv[argc] = 0;

    unsigned int* frame = (unsigned int*)sp;
    frame[-1] = (unsigned int)user_argv;
    frame[-2] = (unsigned int)argc;
    frame[-3] = 0;   /* fake return address */

    unsigned int final_esp = (unsigned int)frame - 12;
    unsigned int user_cs   = SEG_USER_CODE;
    unsigned int user_ds   = SEG_USER_DATA;

    __asm__ __volatile__ (
        "mov  %3, %%eax      \n"
        "mov  %%ax, %%ds     \n"
        "mov  %%ax, %%es     \n"
        "mov  %%ax, %%fs     \n"
        "mov  %%ax, %%gs     \n"
        "push %3             \n" /* SS */
        "push %1             \n" /* ESP */
        "pushf               \n" /* EFLAGS */
        "orl  $0x200, (%%esp)\n" /* set IF */
        "push %2             \n" /* CS */
        "push %0             \n" /* EIP */
        "iret                \n"
        :
        : "r"(entry), "r"(final_esp), "r"(user_cs), "r"(user_ds)
        : "eax"
    );

    __builtin_unreachable();
}

static void elf_user_task_bootstrap(void) {
    process_t* proc = sched_current();

    if (!proc || proc->user_entry == 0) {
        terminal_puts("elf: user task bootstrap failed\n");
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }

    tss_set_kernel_stack(proc->kernel_stack_frame + PAGE_SIZE);
    paging_switch(proc->pd);

    elf_enter_ring3(proc->user_entry,
                    USER_STACK_TOP,
                    proc->user_argc,
                    proc->user_argv);
}

static int elf_seed_sched_context(process_t* proc,
                                  unsigned int entry,
                                  int argc,
                                  char** argv)
{
    unsigned int used = 0;

    if (!proc) return 0;
    if (argc < 0 || argc > PROCESS_MAX_ARGS) {
        terminal_puts("elf: too many args for bootstrap\n");
        return 0;
    }

    for (int i = 0; i < argc; i++) {
        int len = str_len(argv[i]) + 1;
        if (used + (unsigned int)len > PROCESS_ARG_BYTES) {
            terminal_puts("elf: args too large for bootstrap\n");
            return 0;
        }

        proc->user_argv[i] = &proc->user_arg_data[used];
        mem_copy((u8*)proc->user_argv[i], (const u8*)argv[i], (unsigned int)len);
        used += (unsigned int)len;
    }

    proc->user_argc  = argc;
    proc->user_entry = entry;
    if (argc < PROCESS_MAX_ARGS) {
        proc->user_argv[argc] = 0;
    }

    {
        unsigned int* stack_top = (unsigned int*)(proc->kernel_stack_frame + PAGE_SIZE);
        stack_top--;
        *stack_top = (unsigned int)elf_user_task_bootstrap;
        proc->sched_esp = (unsigned int)stack_top;
    }

    return 1;
}

int elf_process_running(void) {
    process_t* proc = process_get_current();
    return proc && proc->state == PROCESS_STATE_RUNNING;
}

process_t* elf_run_image(const unsigned char* image, int argc, char** argv) {
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)image;

    if (*(const unsigned int*)eh->e_ident != ELF_MAGIC) {
        terminal_puts("elf: bad magic\n");
        return 0;
    }

    process_t* proc = process_create("elf");
    if (!proc) return 0;

    proc->pd = process_pd_create();
    if (!proc->pd) {
        process_destroy(proc);
        return 0;
    }

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
                u32* pt      = (u32*)(proc->pd[pd_idx] & ~0xFFFu);
                u8*  dst     = (u8*)(pt[pt_idx] & ~0xFFFu);

                u32 chunk = PAGE_SIZE - page_off;
                if (copied + chunk > filesz) {
                    chunk = filesz - copied;
                }

                mem_copy(dst + page_off, src + copied, chunk);
                copied += chunk;
            }
        }
    }

    {
        u32 stack_virt       = USER_STACK_TOP - USER_STACK_SIZE;
        u32 stack_frame_phys = pmm_alloc_frame();
        if (!stack_frame_phys) {
            terminal_puts("elf: out of frames (stack)\n");
            process_destroy(proc);
            return 0;
        }

        mem_zero((u8*)stack_frame_phys, PAGE_SIZE);
        paging_map_page(proc->pd, stack_virt, stack_frame_phys,
                        PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
    }

    proc->kernel_stack_frame = pmm_alloc_frame();
    if (!proc->kernel_stack_frame) {
        terminal_puts("elf: out of frames (kernel stack)\n");
        process_destroy(proc);
        return 0;
    }

    tss_set_kernel_stack(proc->kernel_stack_frame + PAGE_SIZE);

    if (!elf_seed_sched_context(proc, eh->e_entry, argc, argv)) {
        process_destroy(proc);
        return 0;
    }

    proc->state = PROCESS_STATE_RUNNING;

    if (!sched_enqueue(proc)) {
        process_destroy(proc);
        return 0;
    }

    return proc;
}

process_t* elf_run_named(const char* name, int argc, char** argv) {
    u32 size = 0;
    const u8* data = fat16_load(name, &size);
    if (!data) {
        terminal_puts("elf: not found: ");
        terminal_puts(name);
        terminal_putc('\n');
        return 0;
    }

    return elf_run_image(data, argc, argv);
}