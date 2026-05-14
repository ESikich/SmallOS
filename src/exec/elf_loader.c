#include "elf_loader.h"
#include "../kernel/elf.h"
#include "paging.h"
#include "memory.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "vfs.h"
#include "terminal.h"
#include "gdt.h"
#include "klib.h"

typedef unsigned char u8;

static u8* elf_user_ptr(process_t* proc, unsigned int va) {
    u32* pd;
    u32 pde;
    u32* pt;
    u32 pte;

    if (!proc || !proc->pd) return 0;
    pd = (u32*)paging_phys_to_kernel_virt((u32)proc->pd);
    pde = pd[va >> 22];
    if (!(pde & PAGE_PRESENT)) return 0;
    pt = (u32*)paging_phys_to_kernel_virt(pde & ~0xFFFu);
    pte = pt[(va >> 12) & 0x3FFu];
    if (!(pte & PAGE_PRESENT)) return 0;
    return (u8*)paging_phys_to_kernel_virt(pte & ~0xFFFu) + (va & 0xFFFu);
}

static int elf_copy_to_user(process_t* proc, unsigned int va, const void* src, unsigned int len) {
    const u8* in = (const u8*)src;
    unsigned int copied = 0;

    while (copied < len) {
        unsigned int cur = va + copied;
        unsigned int page_off = cur & 0xFFFu;
        unsigned int chunk = PAGE_SIZE - page_off;
        u8* out;

        if (chunk > len - copied) chunk = len - copied;
        out = elf_user_ptr(proc, cur);
        if (!out) return 0;
        k_memcpy(out, in + copied, chunk);
        copied += chunk;
    }
    return 1;
}

static int elf_setup_user_stack(process_t* proc,
                                int argc,
                                char** argv,
                                int envc,
                                char** envp,
                                unsigned int* out_esp) {
    unsigned int sp = USER_STACK_TOP;
    unsigned int user_argv_ptrs[PROCESS_MAX_ARGS + 1];
    unsigned int user_envp_ptrs[PROCESS_MAX_ENVS + 1];
    unsigned int user_argv = 0;
    unsigned int user_envp = 0;

    if (!proc || !out_esp) return 0;
    if (argc < 0 || argc > PROCESS_MAX_ARGS) return 0;
    if (envc < 0 || envc > PROCESS_MAX_ENVS) return 0;

    for (int i = envc - 1; i >= 0; i--) {
        unsigned int len = (unsigned int)k_strlen(envp[i]) + 1u;
        sp -= len;
        if (!elf_copy_to_user(proc, sp, envp[i], len)) return 0;
        user_envp_ptrs[i] = sp;
    }
    user_envp_ptrs[envc] = 0;

    for (int i = argc - 1; i >= 0; i--) {
        unsigned int len = (unsigned int)k_strlen(argv[i]) + 1u;
        sp -= len;
        if (!elf_copy_to_user(proc, sp, argv[i], len)) return 0;
        user_argv_ptrs[i] = sp;
    }
    user_argv_ptrs[argc] = 0;

    sp &= ~3u;
    sp -= (unsigned int)(envc + 1) * 4u;
    user_envp = sp;
    if (!elf_copy_to_user(proc, sp, user_envp_ptrs, (unsigned int)(envc + 1) * 4u)) {
        return 0;
    }

    sp -= (unsigned int)(argc + 1) * 4u;
    user_argv = sp;
    if (!elf_copy_to_user(proc, sp, user_argv_ptrs, (unsigned int)(argc + 1) * 4u)) {
        return 0;
    }

    {
        unsigned int frame[4];
        frame[0] = 0;
        frame[1] = (unsigned int)argc;
        frame[2] = user_argv;
        frame[3] = user_envp;
        sp -= sizeof(frame);
        if (!elf_copy_to_user(proc, sp, frame, sizeof(frame))) return 0;
    }

    *out_esp = sp;
    return 1;
}

static void elf_enter_ring3(unsigned int entry,
                            unsigned int user_esp,
                            int          argc,
                            char**       argv,
                            int          envc,
                            char**       envp)
{
    char* sp = (char*)user_esp;
    char* user_argv_ptrs[32];
    char* user_envp_ptrs[PROCESS_MAX_ENVS + 1];

    for (int i = envc - 1; i >= 0; i--) {
        int len = k_strlen(envp[i]) + 1;
        sp -= len;
        k_memcpy(sp, envp[i], (k_size_t)len);
        user_envp_ptrs[i] = sp;
    }
    user_envp_ptrs[envc] = 0;

    for (int i = argc - 1; i >= 0; i--) {
        int len = k_strlen(argv[i]) + 1;
        sp -= len;
        k_memcpy(sp, argv[i], (k_size_t)len);
        user_argv_ptrs[i] = sp;
    }

    sp = (char*)((unsigned int)sp & ~3u);

    sp -= (envc + 1) * 4;
    unsigned int* user_envp = (unsigned int*)sp;
    for (int i = 0; i < envc; i++) {
        user_envp[i] = (unsigned int)user_envp_ptrs[i];
    }
    user_envp[envc] = 0;

    sp -= (argc + 1) * 4;
    unsigned int* user_argv = (unsigned int*)sp;
    for (int i = 0; i < argc; i++) {
        user_argv[i] = (unsigned int)user_argv_ptrs[i];
    }
    user_argv[argc] = 0;

    unsigned int* frame = (unsigned int*)sp;
    frame[-1] = (unsigned int)user_envp;
    frame[-2] = (unsigned int)user_argv;
    frame[-3] = (unsigned int)argc;
    frame[-4] = 0;   /* fake return address */

    unsigned int final_esp = (unsigned int)frame - 16;
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

/*
 * First scheduled entry point for a user task.
 *
 * This is where TSS.ESP0 is updated for the process-owned kernel stack.
 * Do not move that tss_set_kernel_stack() call earlier into elf_run_image():
 * async launch paths such as runelf_nowait or SYS_EXEC may build a new process
 * while some other task is still running, and updating ESP0 during setup would
 * clobber the currently running task's ring-3 return stack.
 */
static void elf_user_task_bootstrap(void) {
    process_t* proc = sched_current();

    if (!proc || proc->user_entry == 0) {
        terminal_puts("elf: user task bootstrap failed\n");
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }

    tss_set_kernel_stack((unsigned int)paging_phys_to_kernel_virt(proc->kernel_stack_frame) +
                         proc->kernel_stack_frames * PAGE_SIZE);
    paging_switch(proc->pd);

    elf_enter_ring3(proc->user_entry,
                    USER_STACK_TOP,
                    proc->user_argc,
                    proc->user_argv,
                    proc->user_envc,
                    proc->user_envp);
}

static int elf_seed_sched_context(process_t* proc,
                                  unsigned int entry,
                                  int argc,
                                  char** argv)
{
    if (!proc) return 0;

    int args_rc = process_set_args(proc, argc, argv);
    if (args_rc < 0) {
        terminal_puts("elf: invalid args for bootstrap\n");
        return 0;
    }

    proc->user_entry = entry;

    {
        unsigned int* stack_top =
            (unsigned int*)((u8*)paging_phys_to_kernel_virt(proc->kernel_stack_frame) +
                            proc->kernel_stack_frames * PAGE_SIZE);
        stack_top--;
        *stack_top = (unsigned int)elf_user_task_bootstrap;
        proc->sched_esp = (unsigned int)stack_top;
    }

    return 1;
}

static void elf_inherit_launch_context(process_t* proc, process_t* parent) {
    if (!proc || !parent) return;

    k_memcpy(proc->cwd, parent->cwd, sizeof(proc->cwd));
    (void)process_set_env(proc, parent->user_envc, parent->user_envp);

    for (int fd = 0; fd <= 2; fd++) {
        fd_entry_t* parent_ent = process_fd_get(parent, fd);
        unsigned int fd_flags;

        if (!parent_ent) continue;

        fd_flags = process_fd_get_fd_flags(parent_ent);
        (void)process_fd_dup_from(proc, fd, parent, fd, fd_flags);
    }
}

static process_t* elf_run_image_with_group(const unsigned char* image,
                                           int argc,
                                           char** argv,
                                           int new_process_group) {
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)image;
    process_t* parent;

    if (*(const unsigned int*)eh->e_ident != ELF_MAGIC) {
        terminal_puts("elf: bad magic\n");
        return 0;
    }

    process_t* proc = process_create("elf");
    if (!proc) return 0;
    process_init_user_group(proc);
    if (new_process_group) {
        proc->pgid = proc->pid;
    }
    parent = sched_current();
    elf_inherit_launch_context(proc, parent);

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
            u32 pd_idx = page_virt >> 22;
            u32 pt_idx = (page_virt >> 12) & 0x3FF;
            u32* pt = 0;

            u32* pd = (u32*)paging_phys_to_kernel_virt((u32)proc->pd);

            if (pd[pd_idx] & PAGE_PRESENT) {
                pt = (u32*)paging_phys_to_kernel_virt(pd[pd_idx] & ~0xFFFu);
            }

            if (!pt || !(pt[pt_idx] & PAGE_PRESENT)) {
                u32 frame_phys = pmm_alloc_frame();
                if (!frame_phys) {
                    terminal_puts("elf: out of frames\n");
                    process_destroy(proc);
                    return 0;
                }

                k_memset(paging_phys_to_kernel_virt(frame_phys), 0, PAGE_SIZE);
                paging_map_page(proc->pd, page_virt, frame_phys,
                                PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
            } else {
                pt[pt_idx] |= PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
            }
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
                u32* pd      = (u32*)paging_phys_to_kernel_virt((u32)proc->pd);
                u32* pt      = (u32*)paging_phys_to_kernel_virt(pd[pd_idx] & ~0xFFFu);
                u8*  dst     = (u8*)paging_phys_to_kernel_virt(pt[pt_idx] & ~0xFFFu);

                u32 chunk = PAGE_SIZE - page_off;
                if (copied + chunk > filesz) {
                    chunk = filesz - copied;
                }

                k_memcpy(dst + page_off, src + copied, chunk);
                copied += chunk;
            }
        }
    }

    {
        u32 stack_base = USER_STACK_TOP - USER_STACK_SIZE;
        for (u32 off = 0; off < USER_STACK_SIZE; off += PAGE_SIZE) {
            u32 stack_virt       = stack_base + off;
            u32 stack_frame_phys = pmm_alloc_frame();
            if (!stack_frame_phys) {
                terminal_puts("elf: out of frames (stack)\n");
                process_destroy(proc);
                return 0;
            }

            k_memset(paging_phys_to_kernel_virt(stack_frame_phys), 0, PAGE_SIZE);
            paging_map_page(proc->pd, stack_virt, stack_frame_phys,
                            PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        }
    }

    proc->kernel_stack_frames = PROCESS_KERNEL_STACK_FRAMES;
    proc->kernel_stack_frame = pmm_alloc_contiguous_frames(proc->kernel_stack_frames);
    if (!proc->kernel_stack_frame) {
        terminal_puts("elf: out of frames (kernel stack)\n");
        process_destroy(proc);
        return 0;
    }
    k_memset(paging_phys_to_kernel_virt(proc->kernel_stack_frame),
             0,
             proc->kernel_stack_frames * PAGE_SIZE);

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

process_t* elf_run_image(const unsigned char* image, int argc, char** argv) {
    return elf_run_image_with_group(image, argc, argv, 0);
}

int elf_exec_image_into(process_t* proc,
                        const unsigned char* image,
                        int argc,
                        char** argv,
                        int envc,
                        char** envp,
                        unsigned int* out_entry,
                        unsigned int* out_user_esp) {
    const Elf32_Ehdr* eh;
    const Elf32_Phdr* ph;
    u32* old_pd;
    u32* new_pd;

    if (!proc || !image || !out_entry || !out_user_esp) return 0;
    eh = (const Elf32_Ehdr*)image;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        return 0;
    }
    if (argc < 0 || argc > PROCESS_MAX_ARGS) return 0;
    if (envc < 0 || envc > PROCESS_MAX_ENVS) return 0;

    old_pd = proc->pd;
    new_pd = process_pd_create();
    if (!new_pd) return 0;
    proc->pd = new_pd;

    ph = (const Elf32_Phdr*)(image + eh->e_phoff);
    for (unsigned short i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;

        u32 seg_start = ph[i].p_vaddr;
        u32 seg_end   = ph[i].p_vaddr + ph[i].p_memsz;
        u32 map_start = seg_start & ~(PAGE_SIZE - 1);
        u32 map_end   = (seg_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (u32 page = map_start; page < map_end; page += PAGE_SIZE) {
            u32 frame = pmm_alloc_frame();
            if (!frame) {
                process_pd_destroy(new_pd);
                proc->pd = old_pd;
                return 0;
            }
            k_memset(paging_phys_to_kernel_virt(frame), 0, PAGE_SIZE);
            paging_map_page(new_pd, page, frame, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        }

        if (!elf_copy_to_user(proc, seg_start, image + ph[i].p_offset, ph[i].p_filesz)) {
            process_pd_destroy(new_pd);
            proc->pd = old_pd;
            return 0;
        }
    }

    {
        u32 stack_base = USER_STACK_TOP - USER_STACK_SIZE;
        for (u32 off = 0; off < USER_STACK_SIZE; off += PAGE_SIZE) {
            u32 frame = pmm_alloc_frame();
            if (!frame) {
                process_pd_destroy(new_pd);
                proc->pd = old_pd;
                return 0;
            }
            k_memset(paging_phys_to_kernel_virt(frame), 0, PAGE_SIZE);
            paging_map_page(new_pd, stack_base + off, frame,
                            PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        }
    }

    if (process_set_args(proc, argc, argv) < 0 ||
        process_set_env(proc, envc, envp) < 0 ||
        !elf_setup_user_stack(proc, argc, argv, envc, envp, out_user_esp)) {
        process_pd_destroy(new_pd);
        proc->pd = old_pd;
        return 0;
    }

    proc->user_entry = eh->e_entry;
    proc->heap_base = USER_HEAP_BASE;
    proc->heap_brk = USER_HEAP_BASE;
    *out_entry = eh->e_entry;

    paging_switch(new_pd);
    if (old_pd) process_pd_destroy(old_pd);
    return 1;
}

static process_t* elf_run_named_with_group(const char* name,
                                           int argc,
                                           char** argv,
                                           int new_process_group) {
    u32 size = 0;
    const u8* data = 0;
    char alt_name[40];

    if (name) {
        int has_dot = 0;
        for (const char* p = name; *p; p++) {
            if (*p == '.') {
                has_dot = 1;
                break;
            }
        }

        if (!has_dot) {
            u32 len = (u32)k_strlen(name);
            if (len + 4u < sizeof(alt_name)) {
                k_memcpy(alt_name, name, len);
                k_memcpy(alt_name + len, ".elf", 5);
                data = vfs_load_file(alt_name, &size);
                if (data) {
                    name = alt_name;
                }
            }
        }

        if (!data) {
            data = vfs_load_file(name, &size);
        }
    }

    if (!data) {
        terminal_puts("elf: not found: ");
        terminal_puts(name);
        terminal_putc('\n');
        return 0;
    }

    return elf_run_image_with_group(data, argc, argv, new_process_group);
}

process_t* elf_run_named(const char* name, int argc, char** argv) {
    return elf_run_named_with_group(name, argc, argv, 0);
}

process_t* elf_run_named_new_group(const char* name, int argc, char** argv) {
    return elf_run_named_with_group(name, argc, argv, 1);
}

int elf_exec_named_into(process_t* proc,
                        const char* name,
                        int argc,
                        char** argv,
                        int envc,
                        char** envp,
                        unsigned int* out_entry,
                        unsigned int* out_user_esp) {
    u32 size = 0;
    const u8* data = 0;
    char alt_name[40];

    if (name) {
        int has_dot = 0;
        for (const char* p = name; *p; p++) {
            if (*p == '.') {
                has_dot = 1;
                break;
            }
        }
        if (!has_dot) {
            u32 len = (u32)k_strlen(name);
            if (len + 4u < sizeof(alt_name)) {
                k_memcpy(alt_name, name, len);
                k_memcpy(alt_name + len, ".elf", 5);
                data = vfs_load_file(alt_name, &size);
            }
        }
        if (!data) {
            data = vfs_load_file(name, &size);
        }
    }

    (void)size;
    if (!data) return 0;
    return elf_exec_image_into(proc, data, argc, argv, envc, envp, out_entry, out_user_esp);
}
