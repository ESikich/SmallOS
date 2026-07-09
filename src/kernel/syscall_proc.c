#include "syscall_internal.h"
#include "boot_info.h"
#include "gdt.h"
#include "kalloc.h"
#include "klib.h"
#include "memory.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "timer.h"
#include "uapi_errno.h"
#include "vfs.h"
#include "wait.h"
#include "../exec/elf_loader.h"

#define SYS_PROT_READ         1u
#define SYS_PROT_WRITE        2u
#define SYS_PROT_EXEC         4u
#define SYS_MAP_PRIVATE       0x02u
#define SYS_MAP_FIXED         0x10u
#define SYS_MAP_ANON          0x20u
#define SYS_RLIMIT_CPU        0
#define SYS_RLIMIT_FSIZE      1
#define SYS_RLIMIT_DATA       2
#define SYS_RLIMIT_STACK      3
#define SYS_RLIMIT_CORE       4
#define SYS_RLIMIT_AS         6
#define SYS_RLIMIT_NOFILE     7
#define SYS_RLIM_INFINITY     0xFFFFFFFFu
#define SYS_RUSAGE_SELF       0
#define SYS_RUSAGE_CHILDREN  (-1)

typedef struct syscall_iret_frame {
    unsigned int eip;
    unsigned int cs;
    unsigned int eflags;
    unsigned int user_esp;
    unsigned int ss;
} syscall_iret_frame_t;

void sys_exit_impl(syscall_regs_t* regs) {
    process_t* proc = (process_t*)sched_current();
    if (proc) {
        proc->exit_status = (int)regs->ebx;
    }
    paging_switch(paging_get_kernel_pd());
    sched_exit_current((unsigned int)regs);
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}


/*
 * sys_exec_impl(name, argc, argv)
 *
 * Spawn a named ELF program asynchronously.
 *
 * name and argv are user virtual addresses — fully validated before use.
 * name is copied into a kernel-side buffer before calling elf_run_named()
 * because the loader later switches page directories and must not depend
 * on the caller's user pointer remaining valid.
 *
 * argv validation:
 *   1. The argv array base is checked with user_buf_ok() to ensure the
 *      pointer array itself is mapped in user space.
 *   2. Each argv[i] string is copied into a kernel buffer with
 *      copy_user_cstr(), which validates every page it touches and stops
 *      at the first '\0'.  That means elf_run_named() only sees kernel
 *      memory, never caller-owned pointers.
 *   3. The checks happen here, while the caller's CR3 is still active,
 *      so invalid pointers fail before any ELF work begins.
 *
 * Returns 0 on success or a negative errno if validation fails or the
 * program was not found.
 */
static int sys_exec_spawn_impl(const char* name,
                               int argc,
                               char** argv,
                               int new_process_group) {
    char kname[EXEC_NAME_MAX];
    char kargv_data[PROCESS_ARG_BYTES];
    char* kargv[PROCESS_MAX_ARGS + 1];
    unsigned int used = 0;
    process_t* child;
    process_t* parent;

    int name_rc = copy_user_path_resolved(kname, sizeof(kname), name);
    if (name_rc < 0) return name_rc;
    {
        process_t* proc = (process_t*)sched_current();
        int perm_rc = check_path_permission(proc, kname, SYS_PERM_X, 0);
        if (perm_rc < 0) return perm_rc;
    }

    if (argc < 0 || argc > PROCESS_MAX_ARGS) return -EINVAL;

    /* Validate the argv pointer array itself */
    if (argc > 0 && !user_count_bytes_ok((unsigned int)argv,
                                         (unsigned int)argc,
                                         sizeof(char*),
                                         0)) {
        return -EFAULT;
    }

    for (int i = 0; i < argc; i++) {
        int copied = copy_user_cstr(&kargv_data[used],
                                    PROCESS_ARG_BYTES - used,
                                    argv[i]);
        if (copied < 0) return copied == -ENAMETOOLONG ? -EINVAL : copied;
        kargv[i] = &kargv_data[used];
        used += (unsigned int)copied;
    }
    kargv[argc] = 0;

    child = new_process_group ? elf_run_named_new_group(kname, argc, kargv)
                              : elf_run_named(kname, argc, kargv);
    if (!child) return -ENOENT;
    if (new_process_group) {
        parent = (process_t*)sched_current();
        if (process_fd_pty_set_foreground(parent ? process_fd_get(parent, 0) : 0,
                                          child->pgid) < 0) {
            process_set_foreground(child);
        }
    }
    process_claim_for_wait(child);
    return (int)child->pid;
}

int sys_exec_impl(const char* name, int argc, char** argv) {
    return sys_exec_spawn_impl(name, argc, argv, 0);
}

int sys_exec_fg_impl(const char* name, int argc, char** argv) {
    return sys_exec_spawn_impl(name, argc, argv, 1);
}

static int sys_copy_user_strv(char** argv,
                              int max_entries,
                              unsigned int data_bytes,
                              int* out_argc,
                              char* kargv_data,
                              char** kargv) {
    unsigned int used = 0;
    int argc = 0;

    if (!out_argc || !kargv_data || !kargv) return -EINVAL;
    if (!argv) {
        *out_argc = 0;
        kargv[0] = 0;
        return 0;
    }

    while (argc < max_entries) {
        char* user_arg = 0;
        int rc;

        if (!user_buf_ok((unsigned int)&argv[argc], sizeof(char*))) {
            return -EFAULT;
        }
        rc = copy_from_user(&user_arg, &argv[argc], sizeof(user_arg));
        if (rc < 0) return rc;
        if (!user_arg) break;

        rc = copy_user_cstr(&kargv_data[used],
                            data_bytes - used,
                            user_arg);
        if (rc < 0) return rc == -ENAMETOOLONG ? -EINVAL : rc;
        kargv[argc] = &kargv_data[used];
        used += (unsigned int)rc;
        argc++;
    }

    if (argc == max_entries) {
        char* extra = 0;
        if (!user_buf_ok((unsigned int)&argv[argc], sizeof(char*))) {
            return -EFAULT;
        }
        if (copy_from_user(&extra, &argv[argc], sizeof(extra)) < 0) {
            return -EFAULT;
        }
        if (extra) return -EINVAL;
    }

    kargv[argc] = 0;
    *out_argc = argc;
    return 0;
}

static int sys_copy_argv(char** argv,
                         int* out_argc,
                         char* kargv_data,
                         char** kargv) {
    return sys_copy_user_strv(argv,
                              PROCESS_MAX_ARGS,
                              PROCESS_ARG_BYTES,
                              out_argc,
                              kargv_data,
                              kargv);
}

static int sys_copy_envp(process_t* proc,
                         char** envp,
                         int* out_envc,
                         char* kenv_data,
                         char** kenvp) {
    unsigned int used = 0;
    int envc;

    if (envp) {
        return sys_copy_user_strv(envp,
                                  PROCESS_MAX_ENVS,
                                  PROCESS_ENV_BYTES,
                                  out_envc,
                                  kenv_data,
                                  kenvp);
    }

    if (!proc || !out_envc || !kenv_data || !kenvp) return -EINVAL;
    envc = proc->user_envc;
    if (envc < 0 || envc > PROCESS_MAX_ENVS) return -EINVAL;

    for (int i = 0; i < envc; i++) {
        int len = proc->user_envp[i] ? k_strlen(proc->user_envp[i]) + 1 : 0;
        if (len <= 0 || used + (unsigned int)len > PROCESS_ENV_BYTES) {
            return -EINVAL;
        }
        kenvp[i] = &kenv_data[used];
        k_memcpy(kenvp[i], proc->user_envp[i], (k_size_t)len);
        used += (unsigned int)len;
    }

    kenvp[envc] = 0;
    *out_envc = envc;
    return 0;
}


int sys_fork_impl(syscall_regs_t* regs) {
    process_t* proc = (process_t*)sched_current();
    process_t* child;
    unsigned int top;

    if (!proc || !proc->kernel_stack_frame) return -EINVAL;
    top = (unsigned int)paging_phys_to_kernel_virt(proc->kernel_stack_frame) +
          proc->kernel_stack_frames * PAGE_SIZE;
    child = process_fork_from_syscall((unsigned int)regs, top);
    if (!child) return -ENOMEM;
    process_claim_for_wait(child);
    return (int)child->pid;
}

int sys_execve_impl(syscall_regs_t* regs, const char* name, char** argv, char** envp) {
    char kname[EXEC_NAME_MAX];
    char kargv_data[PROCESS_ARG_BYTES];
    char kenv_data[PROCESS_ENV_BYTES];
    char* kargv[PROCESS_MAX_ARGS + 1];
    char* kenvp[PROCESS_MAX_ENVS + 1];
    int argc;
    int envc;
    int rc;
    process_t* proc = (process_t*)sched_current();
    unsigned int entry = 0;
    unsigned int user_esp = 0;
    syscall_iret_frame_t* iret;

    if (!proc) return -EINVAL;
    rc = copy_user_path_resolved(kname, sizeof(kname), name);
    if (rc < 0) return rc;
    rc = check_path_permission(proc, kname, SYS_PERM_X, 0);
    if (rc < 0) return rc;
    rc = sys_copy_argv(argv, &argc, kargv_data, kargv);
    if (rc < 0) return rc;
    rc = sys_copy_envp(proc, envp, &envc, kenv_data, kenvp);
    if (rc < 0) return rc;

    if (!elf_exec_named_into(proc, kname, argc, kargv, envc, kenvp, &entry, &user_esp)) {
        return -ENOENT;
    }

    process_close_cloexec_fds(proc);
    regs->gs = SEG_USER_DATA;
    regs->fs = SEG_USER_DATA;
    regs->es = SEG_USER_DATA;
    regs->ds = SEG_USER_DATA;

    iret = (syscall_iret_frame_t*)((unsigned char*)regs + sizeof(*regs));
    iret->eip = entry;
    iret->cs = SEG_USER_CODE;
    iret->user_esp = user_esp;
    iret->ss = SEG_USER_DATA;
    iret->eflags |= 0x200u;
    return 0;
}

static int wait_status_to_user(int status) {
    if ((status & 0x10000) != 0) {
        return ((status & 0xFF) << 8) | 0x7F;
    }
    if (status >= 128 && status < 256) {
        return status - 128;
    }
    return (status & 0xFF) << 8;
}

int sys_getpid_impl(void) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    return (int)proc->pid;
}

int sys_setsid_impl(void) {
    process_t* proc = (process_t*)sched_current();
    return process_setsid(proc);
}

int sys_getsid_impl(int pid) {
    process_t* proc = (process_t*)sched_current();
    return process_getsid(proc, pid);
}

int sys_setpgid_impl(int pid, int pgid) {
    process_t* proc = (process_t*)sched_current();
    return process_setpgid(proc, pid, pgid);
}

int sys_getpgid_impl(int pid) {
    process_t* proc = (process_t*)sched_current();
    return process_getpgid(proc, pid);
}

int sys_waitpid_impl(int pid, int* user_status, int options) {
    process_t* proc = (process_t*)sched_current();
    int out_pid = 0;
    int raw_status = 0;
    int rc;

    if (!proc) return -EINVAL;
    if (user_status && !user_buf_ok((unsigned int)user_status, sizeof(int))) {
        return -EFAULT;
    }

    rc = process_wait_pid(proc, pid, options, &out_pid, &raw_status);
    if (rc < 0) return rc;

    if (out_pid != 0 && user_status) {
        int encoded = wait_status_to_user(raw_status);
        if (copy_to_user(user_status, &encoded, sizeof(encoded)) < 0) {
            return -EFAULT;
        }
    }

    return out_pid;
}

int sys_waitpid_fg_impl(int pid, int* user_status) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* stdin_ent;
    process_t* child;
    int raw_status;
    int out_pid = 0;
    int wait_rc;

    if (!proc) return -EINVAL;
    if (pid <= 0) return -EINVAL;
    if (user_status && !user_buf_ok((unsigned int)user_status, sizeof(int))) {
        return -EFAULT;
    }

    child = process_find_by_pid((u32)pid);
    if (!child || child->parent_pid != proc->pid) {
        return -ECHILD;
    }

    child->pgid = child->pid;
    stdin_ent = process_fd_get(proc, 0);
    if (process_fd_pty_set_foreground(stdin_ent, child->pgid) == 0) {
        process_claim_for_wait(child);
        wait_rc = process_wait_pid(proc, pid, 0, &out_pid, &raw_status);
        (void)process_fd_pty_set_foreground(stdin_ent, proc->pgid);
        if (wait_rc < 0) return wait_rc;
    } else {
        raw_status = process_wait_restore_foreground(child, proc);
    }
    if (user_status) {
        int encoded = wait_status_to_user(raw_status);
        if (copy_to_user(user_status, &encoded, sizeof(encoded)) < 0) {
            return -EFAULT;
        }
    }

    return pid;
}

int sys_kill_impl(syscall_regs_t* regs, int pid, int signum) {
    process_t* proc;
    u32 pgid;

    if (signum < 0 || signum >= 32) return -EINVAL;
    proc = (process_t*)sched_current();
    if (signum == 0) {
        if (pid > 0) return process_find_by_pid((u32)pid) ? 0 : -ESRCH;
        pgid = (pid == 0) ? (proc ? proc->pgid : 0u) : (u32)(-pid);
        return process_find_by_pgid(pgid) ? 0 : -ESRCH;
    }

    if (pid <= 0) {
        pgid = (pid == 0) ? (proc ? proc->pgid : 0u) : (u32)(-pid);
        if (pgid == 0u) return -EINVAL;
        if (signum == 18) return process_group_continue(pgid);
        if (signum == 19 || signum == 20 || signum == 21 || signum == 22) {
            int stopped = process_group_stop(pgid, signum, proc, 0);
            if (stopped && proc && proc->state == PROCESS_STATE_STOPPED) {
                sched_yield_now((unsigned int)regs);
            }
            return stopped ? 0 : -ESRCH;
        }
        return process_group_kill(pgid, 128 + signum) ? 0 : -ESRCH;
    }

    if (signum == 18) return process_continue_pid(pid);
    if (signum == 19 || signum == 20 || signum == 21 || signum == 22) {
        return process_stop_pid(pid, signum, (unsigned int)regs);
    }
    return process_kill_pid(pid, 128 + signum, (unsigned int)regs);
}

/*
 * sys_writefile_impl(name, buf, len)
 *
 * Create or overwrite a root-directory ext2 file in one shot.  This is
 * the historical output primitive for user-space tools.
 */

/*
 * sys_brk_impl(new_brk)
 *
 * Query or adjust the calling process heap break.
 *
 * Passing 0 returns the current break.  Growing the break maps new user
 * pages on demand.  Shrinking the break unmaps whole pages above the new
 * limit and returns the updated value.
 */
unsigned int sys_brk_impl(unsigned int new_brk) {
    process_t* proc = (process_t*)sched_current();
    if (!proc || !proc->pd) {
        return (unsigned int)-1;
    }

    unsigned int cur_brk = proc->heap_brk;
    if (new_brk == 0) {
        return cur_brk;
    }

    if (new_brk < proc->heap_base || new_brk >= USER_STACK_TOP - USER_STACK_SIZE) {
        return cur_brk;
    }

    if (new_brk == cur_brk) {
        return new_brk;
    }

    if (new_brk > cur_brk) {
        unsigned int map_start = PAGE_ALIGN(cur_brk);
        unsigned int map_end = PAGE_ALIGN(new_brk);
        if (map_start < map_end &&
            !process_vm_add(proc,
                            map_start,
                            map_end,
                            PROCESS_VM_PROT_READ | PROCESS_VM_PROT_WRITE,
                            PROCESS_VM_PROT_READ | PROCESS_VM_PROT_WRITE | PROCESS_VM_PROT_EXEC,
                            PROCESS_VM_KIND_HEAP,
                            0,
                            0,
                            0,
                            0,
                            0,
                            0)) {
            return cur_brk;
        }

        proc->heap_brk = new_brk;
        return new_brk;
    }

    {
        unsigned int unmap_start = PAGE_ALIGN(new_brk);
        unsigned int unmap_end = PAGE_ALIGN(cur_brk);

        if (unmap_start < unmap_end &&
            !process_vm_remove_range(proc, unmap_start, unmap_end)) {
            return cur_brk;
        }

        proc->heap_brk = new_brk;
        return new_brk;
    }
}

static int user_mapping_range_ok(unsigned int start, unsigned int length) {
    unsigned int end;

    if (length == 0u) return 0;
    if ((start & (PAGE_SIZE - 1u)) != 0u) return 0;
    if (length > USER_HEAP_BASE - USER_MMAP_BASE) return 0;
    end = start + PAGE_ALIGN(length);
    if (end < start) return 0;
    if (start < USER_MMAP_BASE) return 0;
    if (end > USER_INTERP_BASE) return 0;
    return 1;
}

static int user_vm_range_ok(unsigned int start, unsigned int length) {
    unsigned int end;

    if (length == 0u) return 0;
    if ((start & (PAGE_SIZE - 1u)) != 0u) return 0;
    end = start + PAGE_ALIGN(length);
    if (end < start) return 0;
    if (start < USER_CODE_BASE) return 0;
    if (end > USER_STACK_TOP) return 0;
    return 1;
}

static int sys_mmap_pick_start(process_t* proc,
                               unsigned int addr,
                               unsigned int size,
                               unsigned int flags,
                               unsigned int* out_start) {
    unsigned int start;

    if (!proc || !out_start) return -EINVAL;
    if (flags & SYS_MAP_FIXED) {
        start = addr & ~(PAGE_SIZE - 1u);
        if (start != addr) return -EINVAL;
    } else {
        start = PAGE_ALIGN(proc->mmap_next ? proc->mmap_next : USER_MMAP_BASE);
        while (start + size <= USER_INTERP_BASE) {
            if (process_vm_range_free(proc, start, start + size)) break;
            start += size;
        }
    }

    if (!user_mapping_range_ok(start, size)) return -ENOMEM;
    if (!process_vm_range_free(proc, start, start + size)) return -EEXIST;

    *out_start = start;
    return 0;
}

static int sys_mmap_anon_impl(process_t* proc,
                              unsigned int start,
                              unsigned int size,
                              unsigned int prot) {
    if (!process_vm_add(proc,
                        start,
                        start + size,
                        prot,
                        PROCESS_VM_PROT_READ | PROCESS_VM_PROT_WRITE | PROCESS_VM_PROT_EXEC,
                        PROCESS_VM_KIND_ANON,
                        0,
                        0,
                        0,
                        0,
                        0,
                        0)) {
        return -ENOMEM;
    }
    return 0;
}

static int sys_mmap_file_impl(process_t* proc,
                              unsigned int start,
                              unsigned int size,
                              unsigned int prot,
                              int fd,
                              unsigned int offset) {
    fd_entry_t* ent;
    u32 file_size = 0;
    int is_dir = 0;
    u32 bytes;

    if ((offset & (PAGE_SIZE - 1u)) != 0u) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (ent->kind != PROCESS_HANDLE_KIND_FILE || !ent->readable || ent->is_dir) {
        return -EBADF;
    }
    if (vfs_file_stat_fd(ent, &file_size, &is_dir) < 0 || is_dir) return -EBADF;
    if (offset >= file_size) return -EINVAL;

    bytes = file_size - offset;
    if (bytes > size) bytes = size;
    unsigned int max_prot = prot;
    if (prot & SYS_PROT_WRITE) {
        max_prot |= SYS_PROT_READ;
    }
    if (!process_vm_add(proc,
                        start,
                        start + size,
                        prot,
                        max_prot,
                        PROCESS_VM_KIND_FILE_PRIVATE,
                        0,
                        ent->name,
                        start,
                        start + bytes,
                        offset,
                        file_size)) {
        return -ENOMEM;
    }
    return 0;
}

int sys_mmap_impl(unsigned int addr,
                         unsigned int length,
                         unsigned int prot,
                         unsigned int flags,
                         int fd,
                         unsigned int offset) {
    process_t* proc = (process_t*)sched_current();
    unsigned int size;
    unsigned int start;
    unsigned int supported_flags = SYS_MAP_PRIVATE | SYS_MAP_FIXED | SYS_MAP_ANON;
    int rc;

    if (!proc || !proc->pd) return -EINVAL;
    if (length == 0u) return -EINVAL;
    size = PAGE_ALIGN(length);
    if (size == 0u || size < length) return -EINVAL;
    if ((flags & ~supported_flags) != 0u) return -EINVAL;
    if ((flags & SYS_MAP_PRIVATE) == 0u) return -EINVAL;

    rc = sys_mmap_pick_start(proc, addr, size, flags, &start);
    if (rc < 0) return rc;

    if (flags & SYS_MAP_ANON) {
        if ((prot & ~(SYS_PROT_READ | SYS_PROT_WRITE | SYS_PROT_EXEC)) != 0u) return -EINVAL;
        rc = sys_mmap_anon_impl(proc, start, size, prot);
    } else {
        if ((prot & SYS_PROT_READ) == 0u) return -EINVAL;
        if ((prot & ~(SYS_PROT_READ | SYS_PROT_WRITE | SYS_PROT_EXEC)) != 0u) return -EINVAL;
        rc = sys_mmap_file_impl(proc, start, size, prot, fd, offset);
    }
    if (rc < 0) return rc;

    if (!(flags & SYS_MAP_FIXED) || start + size > proc->mmap_next) {
        proc->mmap_next = start + size;
    }
    return (int)start;
}

int sys_munmap_impl(unsigned int addr, unsigned int length) {
    process_t* proc = (process_t*)sched_current();
    unsigned int size;

    if (!proc || !proc->pd) return -EINVAL;
    if (length == 0u) return -EINVAL;
    size = PAGE_ALIGN(length);
    if (!user_vm_range_ok(addr, size)) return -EINVAL;

    return process_vm_remove_range_errno(proc, addr, addr + size);
}

int sys_mprotect_impl(unsigned int addr, unsigned int length, unsigned int prot) {
    process_t* proc = (process_t*)sched_current();
    unsigned int size;

    if (!proc || !proc->pd) return -EINVAL;
    if (length == 0u) return -EINVAL;
    size = PAGE_ALIGN(length);
    if (!user_vm_range_ok(addr, size)) return -EINVAL;
    if ((prot & ~(SYS_PROT_READ | SYS_PROT_WRITE | SYS_PROT_EXEC)) != 0u) return -EINVAL;

    return process_vm_protect_errno(proc, addr, addr + size, prot);
}

int sys_sigaction_impl(int signum, const sys_sigaction_t* act, sys_sigaction_t* oldact) {
    process_t* proc = (process_t*)sched_current();
    sys_sigaction_t in;
    sys_sigaction_t old;

    if (!proc || signum <= 0 || signum >= 32) return -EINVAL;
    if (oldact) {
        old.handler = proc->signal_actions[signum].handler;
        old.sigaction = proc->signal_actions[signum].sigaction;
        old.restorer = proc->signal_actions[signum].restorer;
        old.mask = proc->signal_actions[signum].mask;
        old.flags = proc->signal_actions[signum].flags;
        if (copy_to_user(oldact, &old, sizeof(old)) < 0) return -EFAULT;
    }
    if (act) {
        if (copy_from_user(&in, act, sizeof(in)) < 0) return -EFAULT;
        proc->signal_actions[signum].handler = in.handler;
        proc->signal_actions[signum].sigaction = in.sigaction;
        proc->signal_actions[signum].restorer = in.restorer;
        proc->signal_actions[signum].mask = in.mask;
        proc->signal_actions[signum].flags = in.flags;
    }
    return 0;
}

int sys_sigprocmask_impl(int how, const unsigned int* set, unsigned int* oldset) {
    process_t* proc = (process_t*)sched_current();
    unsigned int in = 0;
    unsigned int next;
    const unsigned int unmaskable = (1u << 9u) | (1u << 19u);

    if (!proc) return -EINVAL;
    if (oldset && copy_to_user(oldset, &proc->signal_mask, sizeof(proc->signal_mask)) < 0) {
        return -EFAULT;
    }
    if (!set) return 0;
    if (copy_from_user(&in, set, sizeof(in)) < 0) return -EFAULT;
    in &= ~unmaskable;

    if (how == SYS_SIG_BLOCK) {
        next = proc->signal_mask | in;
    } else if (how == SYS_SIG_UNBLOCK) {
        next = proc->signal_mask & ~in;
    } else if (how == SYS_SIG_SETMASK) {
        next = in;
    } else {
        return -EINVAL;
    }
    proc->signal_mask = next & ~unmaskable;
    return 0;
}

int sys_sigreturn_impl(syscall_regs_t* regs, const sys_signal_frame_t* user_frame) {
    process_t* proc = (process_t*)sched_current();
    sys_signal_frame_t frame;
    syscall_iret_frame_t* iret;

    if (!proc || !regs || !user_frame) return -EINVAL;
    if (copy_from_user(&frame, user_frame, sizeof(frame)) < 0) return -EFAULT;

    proc->signal_mask = frame.context.uc_sigmask;
    regs->gs = (unsigned int)frame.context.gregs[SYS_REG_GS];
    regs->fs = (unsigned int)frame.context.gregs[SYS_REG_FS];
    regs->es = (unsigned int)frame.context.gregs[SYS_REG_ES];
    regs->ds = (unsigned int)frame.context.gregs[SYS_REG_DS];
    regs->edi = (unsigned int)frame.context.gregs[SYS_REG_EDI];
    regs->esi = (unsigned int)frame.context.gregs[SYS_REG_ESI];
    regs->ebp = (unsigned int)frame.context.gregs[SYS_REG_EBP];
    regs->esp = (unsigned int)frame.context.gregs[SYS_REG_ESP];
    regs->ebx = (unsigned int)frame.context.gregs[SYS_REG_EBX];
    regs->edx = (unsigned int)frame.context.gregs[SYS_REG_EDX];
    regs->ecx = (unsigned int)frame.context.gregs[SYS_REG_ECX];
    regs->eax = (unsigned int)frame.context.gregs[SYS_REG_EAX];

    iret = (syscall_iret_frame_t*)((unsigned char*)regs + sizeof(*regs));
    iret->eip = (unsigned int)frame.context.gregs[SYS_REG_EIP];
    iret->cs = (unsigned int)frame.context.gregs[SYS_REG_CS];
    iret->eflags = (unsigned int)frame.context.gregs[SYS_REG_EFL];
    iret->user_esp = (unsigned int)frame.context.gregs[SYS_REG_UESP];
    iret->ss = (unsigned int)frame.context.gregs[SYS_REG_SS];
    return 0;
}

/* ------------------------------------------------------------------ */
/* File descriptor syscalls                                           */
/* ------------------------------------------------------------------ */

/*
 * sys_open_impl(name)
 *
 * Validate the filename, confirm the file exists through the VFS layer,
 * allocate the lowest free fd slot (>= PROCESS_FD_FIRST)
 * in the current process's fd table, and record name, size, and offset=0.
 *
 * Returns the fd (>= 3) on success or a negative errno on failure.
 */

unsigned int sys_getuid_impl(void) {
    process_t* proc = (process_t*)sched_current();
    return proc ? proc->uid : 0u;
}

unsigned int sys_geteuid_impl(void) {
    process_t* proc = (process_t*)sched_current();
    return proc ? proc->euid : 0u;
}

unsigned int sys_getgid_impl(void) {
    process_t* proc = (process_t*)sched_current();
    return proc ? proc->gid : 0u;
}

unsigned int sys_getegid_impl(void) {
    process_t* proc = (process_t*)sched_current();
    return proc ? proc->egid : 0u;
}

int sys_setuid_impl(unsigned int uid) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    if (!process_is_root(proc) && uid != proc->uid && uid != proc->euid) {
        return -EPERM;
    }
    if (process_is_root(proc)) {
        proc->uid = uid;
    }
    proc->euid = uid;
    return 0;
}

int sys_setgid_impl(unsigned int gid) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    if (!process_is_root(proc) && gid != proc->gid && gid != proc->egid) {
        return -EPERM;
    }
    if (process_is_root(proc)) {
        proc->gid = gid;
        proc->supp_gid = gid;
        proc->supp_gid_count = 1u;
    }
    proc->egid = gid;
    return 0;
}

unsigned int sys_umask_impl(unsigned int mask) {
    process_t* proc = (process_t*)sched_current();
    unsigned int old = proc ? proc->umask : 0022u;
    if (proc) proc->umask = mask & 0777u;
    return old;
}


static int resource_limit_value(process_t* proc,
                                int resource,
                                unsigned int* cur,
                                unsigned int* max) {
    if (!proc || !cur || !max) return -EINVAL;

    switch (resource) {
        case SYS_RLIMIT_CPU:
            *cur = SYS_RLIM_INFINITY;
            *max = SYS_RLIM_INFINITY;
            return 0;
        case SYS_RLIMIT_DATA:
            *cur = (USER_STACK_TOP - USER_STACK_SIZE) - USER_HEAP_BASE;
            *max = *cur;
            return 0;
        case SYS_RLIMIT_STACK:
            *cur = USER_STACK_SIZE;
            *max = USER_STACK_SIZE;
            return 0;
        case SYS_RLIMIT_AS:
            *cur = USER_STACK_TOP - USER_CODE_BASE;
            *max = *cur;
            return 0;
        case SYS_RLIMIT_NOFILE:
            *cur = proc->fd_limit;
            *max = PROCESS_FD_LIMIT_HARD;
            return 0;
        default:
            return -EINVAL;
    }
}

int sys_getrlimit_impl(int resource, sys_rlimit_t* out) {
    process_t* proc = (process_t*)sched_current();
    sys_rlimit_t lim;
    int rc;

    if (!out) return -EFAULT;
    if (!user_buf_ok((unsigned int)out, sizeof(*out))) return -EFAULT;
    if (!proc) return -EINVAL;

    rc = resource_limit_value(proc, resource, &lim.rlim_cur, &lim.rlim_max);
    if (rc < 0) return rc;
    if (copy_to_user(out, &lim, sizeof(lim)) < 0) return -EFAULT;
    return 0;
}

int sys_setrlimit_impl(int resource, const sys_rlimit_t* in) {
    process_t* proc = (process_t*)sched_current();
    sys_rlimit_t lim;
    unsigned int cur;
    unsigned int max;
    int rc;

    if (!in) return -EFAULT;
    if (!user_buf_ok((unsigned int)in, sizeof(*in))) return -EFAULT;
    if (!proc) return -EINVAL;
    if (copy_from_user(&lim, in, sizeof(lim)) < 0) return -EFAULT;

    rc = resource_limit_value(proc, resource, &cur, &max);
    if (rc < 0) return rc;
    if (lim.rlim_cur > lim.rlim_max) return -EINVAL;
    if (lim.rlim_max > max || lim.rlim_cur > max) return -EINVAL;

    if (resource == SYS_RLIMIT_NOFILE) {
        if (lim.rlim_cur < proc->fd_capacity) return -EINVAL;
        proc->fd_limit = lim.rlim_cur;
        return 0;
    }

    if (lim.rlim_cur != cur || lim.rlim_max != max) return -EINVAL;
    return 0;
}

int sys_getrusage_impl(int who, sys_rusage_t* out) {
    process_t* proc = (process_t*)sched_current();
    sys_rusage_t usage;
    unsigned int ticks;

    if (!out) return -EFAULT;
    if (!user_buf_ok((unsigned int)out, sizeof(*out))) return -EFAULT;
    if (!proc) return -EINVAL;
    if (who != SYS_RUSAGE_SELF && who != SYS_RUSAGE_CHILDREN) return -EINVAL;

    k_memset(&usage, 0, sizeof(usage));
    if (who == SYS_RUSAGE_SELF) {
        ticks = proc->cpu_ticks;
        usage.ru_utime.tv_sec = (long)(ticks / 1000u);
        usage.ru_utime.tv_usec = (long)((ticks % 1000u) * 1000u);
        usage.ru_maxrss = (long)(process_ram_bytes(proc) / 1024u);
    } else {
        ticks = proc->child_cpu_ticks;
        usage.ru_utime.tv_sec = (long)(ticks / 1000u);
        usage.ru_utime.tv_usec = (long)((ticks % 1000u) * 1000u);
        usage.ru_nvcsw = (long)proc->child_wait_count;
    }

    if (copy_to_user(out, &usage, sizeof(usage)) < 0) return -EFAULT;
    return 0;
}


int sys_meminfo_impl(sys_meminfo_t* out_info) {
    sys_meminfo_t info;
    process_accounting_t proc_acct;
    kalloc_stats_t ka;

    if (!out_info) return -EFAULT;

    k_memset(&info, 0, sizeof(info));
    process_accounting_snapshot(&proc_acct);
    (void)kalloc_stats(&ka);

    info.heap_base = memory_get_heap_base();
    info.heap_top = memory_get_heap_top();
    info.pmm_free_frames = pmm_free_count();
    info.pmm_total_frames = pmm_total_count();
    info.e820_valid = boot_info_e820_valid() ? 1u : 0u;
    info.e820_count = info.e820_valid ? boot_info_e820_count() : 0u;
    vfs_file_map_cache_stats(&info.ro_file_cache_pages,
                             &info.ro_file_cache_mapped_refs);
    info.pmm_used_frames = pmm_used_count();
    info.pmm_refcounted_frames = pmm_refcounted_count();
    info.pmm_shared_frames = pmm_shared_count();
    info.process_count = proc_acct.process_count;
    info.process_capacity = proc_acct.process_capacity;
    info.process_pages = proc_acct.process_pages;
    info.kernel_stack_pages = proc_acct.kernel_stack_pages;
    info.fd_table_pages = proc_acct.fd_table_pages;
    info.vm_area_pages = proc_acct.vm_area_pages;
    info.kalloc_pages = ka.pages;
    info.kalloc_free_bytes = ka.free_bytes;
    info.kalloc_used_bytes = ka.used_bytes;

    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

unsigned int process_ram_bytes(process_t* proc) {
    unsigned int frames = 0;

    if (!proc) return 0;

    frames += 1u; /* process_t */
    frames += proc->kernel_stack_frames;
    frames += proc->fd_table_frames;
    frames += process_pd_count_private_frames(proc->pd);

    return frames * PAGE_SIZE;
}

int sys_procinfo_impl(sys_procinfo_t* out_info) {
    process_t* procs[SYS_PROCINFO_MAX];
    sys_procinfo_t info;
    int count;
    unsigned int total_ticks = timer_get_ticks();

    if (!out_info) return -EFAULT;

    k_memset(&info, 0, sizeof(info));
    count = sched_snapshot_all(procs, SYS_PROCINFO_MAX);
    info.total_count = (unsigned int)sched_count();
    info.out_count = (unsigned int)count;
    info.total_ticks = total_ticks;

    for (int i = 0; i < count; i++) {
        process_t* proc = procs[i];
        sys_procinfo_entry_t* ent = &info.entries[i];
        unsigned int heap_bytes = 0;

        if (!proc) continue;
        if (proc->heap_brk >= proc->heap_base) {
            heap_bytes = proc->heap_brk - proc->heap_base;
        }

        ent->pid = proc->pid;
        ent->parent_pid = proc->parent_pid;
        ent->pgid = proc->pgid;
        ent->state = (unsigned int)proc->state;
        ent->cpu_ticks = proc->cpu_ticks;
        ent->ram_bytes = process_ram_bytes(proc);
        ent->heap_bytes = heap_bytes;
        k_strncpy(ent->name, proc->name, sizeof(ent->name));
    }

    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}



int sys_e820_entry_impl(unsigned int index, sys_e820_entry_t* out_entry) {
    const boot_info_t* info;
    sys_e820_entry_t entry;

    if (!out_entry) return -EFAULT;
    if (!boot_info_e820_valid()) return 0;

    info = boot_info_get();
    if (index >= info->e820_count) return -EINVAL;

    entry.base = info->e820[index].base;
    entry.length = info->e820[index].length;
    entry.type = info->e820[index].type;
    entry.attr = info->e820[index].attr;
    if (copy_to_user(out_entry, &entry, sizeof(entry)) < 0) return -EFAULT;
    return (int)info->e820_count;
}
