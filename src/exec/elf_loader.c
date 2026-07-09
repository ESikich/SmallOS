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

typedef struct elf_map_info {
    unsigned int entry;
    unsigned int phdr;
    unsigned int phent;
    unsigned int phnum;
    unsigned int load_base;
    unsigned int low;
    unsigned int high;
} elf_map_info_t;

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
        if (!out) {
            if (!process_vm_fault_in_page(proc, cur, 1)) return 0;
            out = elf_user_ptr(proc, cur);
            if (!out) return 0;
        }
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
                                int auxc,
                                const unsigned int* auxv,
                                unsigned int* out_esp) {
    unsigned int sp = USER_STACK_TOP;
    unsigned int user_argv_ptrs[PROCESS_MAX_ARGS + 1];
    unsigned int user_envp_ptrs[PROCESS_MAX_ENVS + 1];
    unsigned int user_argv = 0;
    unsigned int user_envp = 0;
    unsigned int user_auxv = 0;

    if (!proc || !out_esp) return 0;
    if (argc < 0 || argc > PROCESS_MAX_ARGS) return 0;
    if (envc < 0 || envc > PROCESS_MAX_ENVS) return 0;
    if (auxc < 0 || auxc > PROCESS_AUXV_MAX) return 0;

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

    sp -= (unsigned int)(auxc * 2 + 2) * 4u;
    user_auxv = sp;
    if (auxc > 0 && auxv) {
        if (!elf_copy_to_user(proc, sp, auxv, (unsigned int)auxc * 2u * 4u)) {
            return 0;
        }
    }
    {
        unsigned int null_pair[2] = { AT_NULL, 0 };
        if (!elf_copy_to_user(proc,
                              sp + (unsigned int)auxc * 2u * 4u,
                              null_pair,
                              sizeof(null_pair))) {
            return 0;
        }
    }

    {
        unsigned int frame[5];
        frame[0] = 0;
        frame[1] = (unsigned int)argc;
        frame[2] = user_argv;
        frame[3] = user_envp;
        frame[4] = user_auxv;
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
                            char**       envp,
                            int          auxc,
                            const unsigned int* auxv)
{
    char* sp = (char*)user_esp;
    char* user_argv_ptrs[PROCESS_MAX_ARGS + 1];
    char* user_envp_ptrs[PROCESS_MAX_ENVS + 1];
    unsigned int* user_auxv;

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

    sp -= (auxc * 2 + 2) * 4;
    user_auxv = (unsigned int*)sp;
    for (int i = 0; i < auxc * 2; i++) {
        user_auxv[i] = auxv ? auxv[i] : 0;
    }
    user_auxv[auxc * 2] = AT_NULL;
    user_auxv[auxc * 2 + 1] = 0;

    unsigned int* frame = (unsigned int*)sp;
    frame[-1] = (unsigned int)user_auxv;
    frame[-2] = (unsigned int)user_envp;
    frame[-3] = (unsigned int)user_argv;
    frame[-4] = (unsigned int)argc;
    frame[-5] = 0;   /* fake return address */

    unsigned int final_esp = (unsigned int)frame - 20;
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

static void elf_enter_ring3_raw(unsigned int entry, unsigned int user_esp) {
    unsigned int user_cs = SEG_USER_CODE;
    unsigned int user_ds = SEG_USER_DATA;

    __asm__ __volatile__ (
        "mov  %3, %%eax      \n"
        "mov  %%ax, %%ds     \n"
        "mov  %%ax, %%es     \n"
        "mov  %%ax, %%fs     \n"
        "mov  %%ax, %%gs     \n"
        "push %3             \n"
        "push %1             \n"
        "pushf               \n"
        "orl  $0x200, (%%esp)\n"
        "push %2             \n"
        "push %0             \n"
        "iret                \n"
        :
        : "r"(entry), "r"(user_esp), "r"(user_cs), "r"(user_ds)
        : "eax"
    );

    __builtin_unreachable();
}

/*
 * First scheduled entry point for a user task.
 *
 * This is where TSS.ESP0 is updated for the process-owned kernel stack.
 * Do not move that tss_set_kernel_stack() call earlier into elf_run_image():
 * async launch paths such as background launches or SYS_EXEC may build a new process
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

    elf_enter_ring3_raw(proc->user_entry, proc->user_stack_esp);
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

static int elf_valid_header(const Elf32_Ehdr* eh) {
    if (!eh) return 0;
    if (*(const unsigned int*)eh->e_ident != ELF_MAGIC) return 0;
    if (eh->e_machine != 3u) return 0;
    if (eh->e_phentsize != sizeof(Elf32_Phdr)) return 0;
    if (eh->e_phnum == 0) return 0;
    return eh->e_type == ET_EXEC || eh->e_type == ET_DYN;
}

static unsigned int elf_vaddr_for_file_offset(const unsigned char* image,
                                              unsigned int load_bias,
                                              unsigned int off) {
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)image;
    const Elf32_Phdr* ph = (const Elf32_Phdr*)(image + eh->e_phoff);

    for (unsigned short i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (off >= ph[i].p_offset && off < ph[i].p_offset + ph[i].p_filesz) {
            return load_bias + ph[i].p_vaddr + (off - ph[i].p_offset);
        }
    }
    return load_bias + off;
}

static int elf_copy_interp_path(const unsigned char* image, char* out, unsigned int out_size) {
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)image;
    const Elf32_Phdr* ph = (const Elf32_Phdr*)(image + eh->e_phoff);
    unsigned int read_pos;
    unsigned int write_pos;

    if (!out || out_size == 0u) return 0;
    out[0] = '\0';
    for (unsigned short i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_INTERP) continue;
        if (ph[i].p_filesz == 0 || ph[i].p_filesz >= out_size) return 0;
        k_memcpy(out, image + ph[i].p_offset, ph[i].p_filesz);
        out[ph[i].p_filesz] = '\0';
        read_pos = 0;
        write_pos = 0;
        while (out[read_pos] == '/' || out[read_pos] == '\\') {
            read_pos++;
        }
        while (out[read_pos] != '\0') {
            out[write_pos++] = out[read_pos++];
        }
        out[write_pos] = '\0';
        return 1;
    }
    return 0;
}

static unsigned int elf_main_load_base(const Elf32_Ehdr* eh, int has_interp) {
    if (!eh || eh->e_type != ET_DYN) return 0;
    return has_interp ? USER_PIE_BASE : 0;
}

static int elf_validate_main_layout(const Elf32_Ehdr* eh,
                                    const elf_map_info_t* main_info,
                                    int has_interp) {
    if (!eh || !main_info) return 0;
    if (eh->e_type == ET_DYN && !has_interp) {
        terminal_puts("elf: unsupported ET_DYN without interpreter\n");
        return 0;
    }
    if (has_interp && main_info->low < USER_INTERP_BASE && main_info->high > USER_MMAP_BASE) {
        terminal_puts("elf: executable overlaps dynamic mmap arena\n");
        return 0;
    }
    if (has_interp && main_info->high > USER_HEAP_BASE) {
        terminal_puts("elf: executable overlaps user heap\n");
        return 0;
    }
    return 1;
}

static int elf_validate_interp_layout(const elf_map_info_t* main_info,
                                      const elf_map_info_t* interp_info) {
    if (!main_info || !interp_info) return 0;
    if (interp_info->low < USER_INTERP_BASE || interp_info->high > USER_HEAP_BASE) {
        terminal_puts("elf: interpreter outside reserved range\n");
        return 0;
    }
    if (main_info->high > interp_info->low && main_info->low < interp_info->high) {
        terminal_puts("elf: executable overlaps interpreter\n");
        return 0;
    }
    return 1;
}

static unsigned int elf_segment_prot(u32 flags) {
    unsigned int prot = 0;
    if (flags & PF_R) prot |= PROCESS_VM_PROT_READ;
    if (flags & PF_W) prot |= PROCESS_VM_PROT_WRITE;
    if (flags & PF_X) prot |= PROCESS_VM_PROT_EXEC;
    if (prot == 0u) prot = PROCESS_VM_PROT_READ;
    return prot;
}

static int elf_map_image_into_process(process_t* proc,
                                      const unsigned char* image,
                                      const char* image_path,
                                      unsigned int dyn_base,
                                      elf_map_info_t* out) {
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)image;
    const Elf32_Phdr* ph;
    unsigned int load_bias = 0;
    unsigned int low = 0xFFFFFFFFu;
    unsigned int high = 0;

    if (!proc || !proc->pd || !image || !out) return 0;
    if (!elf_valid_header(eh)) return 0;

    if (eh->e_type == ET_DYN) {
        load_bias = dyn_base;
    }

    ph = (const Elf32_Phdr*)(image + eh->e_phoff);
    for (unsigned short i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;

        u32 seg_start = load_bias + ph[i].p_vaddr;
        u32 seg_end   = seg_start + ph[i].p_memsz;
        u32 map_start = seg_start & ~(PAGE_SIZE - 1);
        u32 map_end   = (seg_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        if (map_start < USER_CODE_BASE || map_end >= USER_STACK_TOP) return 0;
        if (map_start < low) low = map_start;
        if (map_end > high) high = map_end;

        {
            unsigned int prot = elf_segment_prot(ph[i].p_flags);
            unsigned int max_prot = prot;
            if (eh->e_type == ET_DYN) {
                prot |= PROCESS_VM_PROT_WRITE;
                max_prot |= PROCESS_VM_PROT_WRITE | PROCESS_VM_PROT_READ;
            }
            if (prot & PROCESS_VM_PROT_WRITE) {
                max_prot |= PROCESS_VM_PROT_READ;
            }
            if (!process_vm_add(proc,
                                map_start,
                                map_end,
                                prot,
                                max_prot,
                                PROCESS_VM_KIND_ELF,
                                0,
                                image_path,
                                seg_start,
                                seg_start + ph[i].p_filesz,
                                ph[i].p_offset,
                                ph[i].p_filesz)) {
                return 0;
            }
        }

        if (!image_path) {
            for (u32 page = map_start; page < map_end; page += PAGE_SIZE) {
                u32* pd = (u32*)paging_phys_to_kernel_virt((u32)proc->pd);
                u32* pt = 0;
                u32 pd_idx = page >> 22;
                u32 pt_idx = (page >> 12) & 0x3FFu;
                if (pd[pd_idx] & PAGE_PRESENT) {
                    pt = (u32*)paging_phys_to_kernel_virt(pd[pd_idx] & ~0xFFFu);
                }
                if (!pt || !(pt[pt_idx] & PAGE_PRESENT)) {
                    u32 frame = pmm_alloc_frame();
                    if (!frame) {
                        terminal_puts("elf: out of frames\n");
                        return 0;
                    }
                    k_memset(paging_phys_to_kernel_virt(frame), 0, PAGE_SIZE);
                    paging_map_page(proc->pd, page, frame, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
                } else {
                    pt[pt_idx] |= PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
                }
            }

            if (!elf_copy_to_user(proc,
                                  seg_start,
                                  image + ph[i].p_offset,
                                  ph[i].p_filesz)) {
                return 0;
            }
        }
    }

    if (low == 0xFFFFFFFFu) return 0;
    out->entry = eh->e_entry + load_bias;
    out->phdr = elf_vaddr_for_file_offset(image, load_bias, eh->e_phoff);
    out->phent = eh->e_phentsize;
    out->phnum = eh->e_phnum;
    out->load_base = load_bias;
    out->low = low;
    out->high = high;
    return 1;
}

static process_t* elf_run_image_with_group(const unsigned char* image,
                                           const char* image_path,
                                           int argc,
                                           char** argv,
                                           int new_process_group,
                                           int suspended) {
    const Elf32_Ehdr* eh = (const Elf32_Ehdr*)image;
    process_t* parent;
    elf_map_info_t main_info;
    elf_map_info_t interp_info;
    unsigned int entry;
    unsigned int auxv[PROCESS_AUXV_MAX * 2];
    int auxc = 0;
    char interp_path[PROCESS_FD_NAME_MAX];
    int has_interp;
    unsigned int main_base;

    if (!elf_valid_header(eh)) {
        terminal_puts("elf: bad magic\n");
        return 0;
    }
    has_interp = elf_copy_interp_path(image, interp_path, sizeof(interp_path));
    if (eh->e_type == ET_DYN && !has_interp) {
        terminal_puts("elf: unsupported ET_DYN without interpreter\n");
        return 0;
    }
    main_base = elf_main_load_base(eh, has_interp);

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

    if (!elf_map_image_into_process(proc, image, image_path, main_base, &main_info) ||
        !elf_validate_main_layout(eh, &main_info, has_interp)) {
        process_destroy(proc);
        return 0;
    }
    entry = main_info.entry;

    if (has_interp) {
        u32 interp_size = 0;
        u32 interp_frame = 0;
        u32 interp_frames = 0;
        u8* interp = vfs_load_file_owned(interp_path, &interp_size,
                                         &interp_frame, &interp_frames);
        (void)interp_size;
        if (!interp) {
            terminal_puts("elf: missing interpreter: ");
            terminal_puts(interp_path);
            terminal_putc('\n');
            process_destroy(proc);
            return 0;
        }
        if (!elf_map_image_into_process(proc, interp, interp_path, USER_INTERP_BASE, &interp_info) ||
            !elf_validate_interp_layout(&main_info, &interp_info)) {
            vfs_free_file_owned(interp_frame, interp_frames);
            process_destroy(proc);
            return 0;
        }
        vfs_free_file_owned(interp_frame, interp_frames);
        entry = interp_info.entry;

        auxv[auxc * 2] = AT_PHDR; auxv[auxc * 2 + 1] = main_info.phdr; auxc++;
        auxv[auxc * 2] = AT_PHENT; auxv[auxc * 2 + 1] = main_info.phent; auxc++;
        auxv[auxc * 2] = AT_PHNUM; auxv[auxc * 2 + 1] = main_info.phnum; auxc++;
        auxv[auxc * 2] = AT_ENTRY; auxv[auxc * 2 + 1] = main_info.entry; auxc++;
        auxv[auxc * 2] = AT_BASE; auxv[auxc * 2 + 1] = interp_info.load_base; auxc++;
        auxv[auxc * 2] = AT_PAGESZ; auxv[auxc * 2 + 1] = PAGE_SIZE; auxc++;
    }

    if (!process_vm_add(proc,
                        USER_STACK_TOP - USER_STACK_SIZE,
                        USER_STACK_TOP,
                        PROCESS_VM_PROT_READ | PROCESS_VM_PROT_WRITE,
                        PROCESS_VM_PROT_READ | PROCESS_VM_PROT_WRITE,
                        PROCESS_VM_KIND_STACK,
                        0,
                        0,
                        0,
                        0,
                        0,
                        0)) {
        process_destroy(proc);
        return 0;
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

    (void)process_set_auxv(proc, auxc, auxv);
    if (!elf_setup_user_stack(proc,
                              argc,
                              argv,
                              proc->user_envc,
                              proc->user_envp,
                              proc->user_auxc,
                              proc->user_auxv,
                              &proc->user_stack_esp) ||
        !elf_seed_sched_context(proc, entry, argc, argv)) {
        process_destroy(proc);
        return 0;
    }

    proc->state = suspended ? PROCESS_STATE_WAITING : PROCESS_STATE_RUNNING;

    if (!sched_enqueue(proc)) {
        process_destroy(proc);
        return 0;
    }

    return proc;
}

process_t* elf_run_image(const unsigned char* image, int argc, char** argv) {
    return elf_run_image_with_group(image, 0, argc, argv, 0, 0);
}

int elf_exec_image_into(process_t* proc,
                        const unsigned char* image,
                        const char* image_path,
                        int argc,
                        char** argv,
                        int envc,
                        char** envp,
                        unsigned int* out_entry,
                        unsigned int* out_user_esp) {
    const Elf32_Ehdr* eh;
    u32* old_pd;
    u32* new_pd;
    elf_map_info_t main_info;
    elf_map_info_t interp_info;
    unsigned int entry;
    unsigned int auxv[PROCESS_AUXV_MAX * 2];
    int auxc = 0;
    char interp_path[PROCESS_FD_NAME_MAX];
    int has_interp;
    unsigned int main_base;

    if (!proc || !image || !out_entry || !out_user_esp) return 0;
    eh = (const Elf32_Ehdr*)image;
    if (!elf_valid_header(eh)) {
        return 0;
    }
    if (argc < 0 || argc > PROCESS_MAX_ARGS) return 0;
    if (envc < 0 || envc > PROCESS_MAX_ENVS) return 0;
    has_interp = elf_copy_interp_path(image, interp_path, sizeof(interp_path));
    if (eh->e_type == ET_DYN && !has_interp) return 0;
    main_base = elf_main_load_base(eh, has_interp);

    old_pd = proc->pd;
    new_pd = process_pd_create();
    if (!new_pd) return 0;
    proc->pd = new_pd;

    process_vm_clear(proc);

    if (!elf_map_image_into_process(proc, image, image_path, main_base, &main_info) ||
        !elf_validate_main_layout(eh, &main_info, has_interp)) {
        process_pd_destroy(new_pd);
        proc->pd = old_pd;
        process_vm_clear(proc);
        return 0;
    }
    entry = main_info.entry;

    if (has_interp) {
        u32 interp_size = 0;
        u32 interp_frame = 0;
        u32 interp_frames = 0;
        u8* interp = vfs_load_file_owned(interp_path, &interp_size,
                                         &interp_frame, &interp_frames);
        (void)interp_size;
        if (!interp ||
            !elf_map_image_into_process(proc, interp, interp_path, USER_INTERP_BASE, &interp_info) ||
            !elf_validate_interp_layout(&main_info, &interp_info)) {
            if (interp) vfs_free_file_owned(interp_frame, interp_frames);
            process_pd_destroy(new_pd);
            proc->pd = old_pd;
            process_vm_clear(proc);
            return 0;
        }
        vfs_free_file_owned(interp_frame, interp_frames);
        entry = interp_info.entry;
        auxv[auxc * 2] = AT_PHDR; auxv[auxc * 2 + 1] = main_info.phdr; auxc++;
        auxv[auxc * 2] = AT_PHENT; auxv[auxc * 2 + 1] = main_info.phent; auxc++;
        auxv[auxc * 2] = AT_PHNUM; auxv[auxc * 2 + 1] = main_info.phnum; auxc++;
        auxv[auxc * 2] = AT_ENTRY; auxv[auxc * 2 + 1] = main_info.entry; auxc++;
        auxv[auxc * 2] = AT_BASE; auxv[auxc * 2 + 1] = interp_info.load_base; auxc++;
        auxv[auxc * 2] = AT_PAGESZ; auxv[auxc * 2 + 1] = PAGE_SIZE; auxc++;
    }

    if (!process_vm_add(proc,
                        USER_STACK_TOP - USER_STACK_SIZE,
                        USER_STACK_TOP,
                        PROCESS_VM_PROT_READ | PROCESS_VM_PROT_WRITE,
                        PROCESS_VM_PROT_READ | PROCESS_VM_PROT_WRITE,
                        PROCESS_VM_KIND_STACK,
                        0,
                        0,
                        0,
                        0,
                        0,
                        0)) {
        process_pd_destroy(new_pd);
        proc->pd = old_pd;
        process_vm_clear(proc);
        return 0;
    }

    if (process_set_args(proc, argc, argv) < 0 ||
        process_set_env(proc, envc, envp) < 0 ||
        process_set_auxv(proc, auxc, auxv) < 0 ||
        !elf_setup_user_stack(proc, argc, argv, envc, envp,
                              proc->user_auxc, proc->user_auxv,
                              out_user_esp)) {
        process_pd_destroy(new_pd);
        proc->pd = old_pd;
        process_vm_clear(proc);
        return 0;
    }

    proc->user_entry = entry;
    proc->user_stack_esp = *out_user_esp;
    proc->heap_base = USER_HEAP_BASE;
    proc->heap_brk = USER_HEAP_BASE;
    proc->mmap_base = USER_MMAP_BASE;
    proc->mmap_next = USER_MMAP_BASE;
    *out_entry = entry;

    paging_switch(new_pd);
    if (old_pd) process_pd_destroy(old_pd);
    return 1;
}

static void elf_set_process_name_from_path(process_t* proc, const char* path) {
    const char* base;
    int len;

    if (!proc || !path) return;

    base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') base = p + 1;
    }

    k_strncpy(proc->name, base, sizeof(proc->name));
    len = k_strlen(proc->name);
    if (len > 4 && k_strcmp(proc->name + len - 4, ".elf")) {
        proc->name[len - 4] = '\0';
    }
}

static process_t* elf_run_named_with_group(const char* name,
                                           int argc,
                                           char** argv,
                                           int new_process_group,
                                           int suspended) {
    u32 size = 0;
    u32 image_frame = 0;
    u32 image_frames = 0;
    u8* data = 0;
    char alt_name[40];
    const char* loaded_name = name;

    if (name) {
        int has_dot = 0;
        for (const char* p = name; *p; p++) {
            if (*p == '.') {
                has_dot = 1;
                break;
            }
        }

        data = vfs_load_file_owned(name, &size,
                                   &image_frame, &image_frames);
        loaded_name = name;

        if (!data && !has_dot) {
            u32 len = (u32)k_strlen(name);
            if (len + 4u < sizeof(alt_name)) {
                k_memcpy(alt_name, name, len);
                k_memcpy(alt_name + len, ".elf", 5);
                data = vfs_load_file_owned(alt_name, &size,
                                           &image_frame, &image_frames);
                if (data) {
                    name = alt_name;
                    loaded_name = alt_name;
                }
            }
        }
    }

    if (!data) {
        terminal_puts("elf: not found: ");
        terminal_puts(name);
        terminal_putc('\n');
        return 0;
    }

    {
        process_t* proc = elf_run_image_with_group(data, loaded_name, argc, argv,
                                                   new_process_group,
                                                   suspended);
        vfs_free_file_owned(image_frame, image_frames);
        elf_set_process_name_from_path(proc, loaded_name);
        return proc;
    }
}

process_t* elf_run_named(const char* name, int argc, char** argv) {
    return elf_run_named_with_group(name, argc, argv, 0, 0);
}

process_t* elf_run_named_new_group(const char* name, int argc, char** argv) {
    return elf_run_named_with_group(name, argc, argv, 1, 0);
}

process_t* elf_run_named_suspended(const char* name, int argc, char** argv) {
    return elf_run_named_with_group(name, argc, argv, 0, 1);
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
    u32 image_frame = 0;
    u32 image_frames = 0;
    u8* data = 0;
    char alt_name[40];
    const char* loaded_name = name;

    if (name) {
        int has_dot = 0;
        for (const char* p = name; *p; p++) {
            if (*p == '.') {
                has_dot = 1;
                break;
            }
        }

        data = vfs_load_file_owned(name, &size,
                                   &image_frame, &image_frames);
        loaded_name = name;

        if (!data && !has_dot) {
            u32 len = (u32)k_strlen(name);
            if (len + 4u < sizeof(alt_name)) {
                k_memcpy(alt_name, name, len);
                k_memcpy(alt_name + len, ".elf", 5);
                data = vfs_load_file_owned(alt_name, &size,
                                           &image_frame, &image_frames);
                if (data) loaded_name = alt_name;
            }
        }
    }

    (void)size;
    if (!data) return 0;
    if (!elf_exec_image_into(proc, data, loaded_name, argc, argv, envc, envp, out_entry, out_user_esp)) {
        vfs_free_file_owned(image_frame, image_frames);
        return 0;
    }
    vfs_free_file_owned(image_frame, image_frames);
    elf_set_process_name_from_path(proc, loaded_name);
    return 1;
}
