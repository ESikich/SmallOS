#include "syscall.h"
#include "terminal.h"
#include "timer.h"
#include "paging.h"
#include "keyboard.h"
#include "scheduler.h"
#include "process.h"
#include "pmm.h"
#include "memory.h"
#include "boot_info.h"
#include "system.h"
#include "klib.h"
#include "../drivers/nic.h"
#include "../drivers/net.h"
#include "../drivers/dhcp.h"
#include "../drivers/arp.h"
#include "../drivers/ipv4.h"
#include "../drivers/tcp.h"
#include "../drivers/ntp.h"
#include "../drivers/mouse.h"
#include "../drivers/usb.h"
#include "uapi_poll.h"
#include "uapi_errno.h"
#include "uapi_dirent.h"
#include "uapi_socket.h"
#include "uapi_epoll.h"
#include "uapi_display.h"
#include "uapi_input.h"
#include "uapi_syscall.h"
#include "../exec/elf_loader.h"
#include "vfs.h"
#include "socket.h"
#include "input.h"
#include "wait.h"
#include "../drivers/display.h"
#include "../drivers/ext2.h"
#include "gdt.h"

#define SYSCALL_MAX_WRITE_LEN 4096u
#define EXEC_NAME_MAX         PROCESS_FD_NAME_MAX
#define EPOLL_MAX_WATCHES     64u
#define POLL_MAX_FDS          PROCESS_FD_LIMIT_HARD
#define INPUT_READ_MAX_EVENTS 64u
#define DIRLIST_BATCH_MAX     64u

static unsigned char s_sys_block_sector[512] __attribute__((aligned(16)));
static volatile int s_sys_block_sector_locked = 0;

typedef struct epoll_watch {
    int used;
    int fd;
    unsigned int events;
    unsigned int data_u32;
} epoll_watch_t;

struct user_itimerspec {
    struct {
        unsigned int tv_sec;
        long tv_nsec;
    } it_interval;
    struct {
        unsigned int tv_sec;
        long tv_nsec;
    } it_value;
};

struct user_timespec {
    unsigned int tv_sec;
    long tv_nsec;
};

typedef struct syscall_iret_frame {
    unsigned int eip;
    unsigned int cs;
    unsigned int eflags;
    unsigned int user_esp;
    unsigned int ss;
} syscall_iret_frame_t;

static int sys_close_impl(int fd);

static int path_is_sep(char c) {
    return c == '/' || c == '\\';
}

static int path_add_component(char comps[][32], int* count, const char* component) {
    if (*count >= 16) {
        return 0;
    }

    int len = 0;
    while (component[len] != '\0') {
        if (len >= 31) {
            return 0;
        }
        len++;
    }

    for (int i = 0; i < len; i++) {
        comps[*count][i] = component[i];
    }
    comps[*count][len] = '\0';
    (*count)++;
    return 1;
}

static int path_build_from(const char* base, const char* path, char* out, unsigned int out_size) {
    char comps[16][32];
    const char* sources[2];
    int source_count = 0;
    int count = 0;

    if (!path || !out || out_size == 0) {
        return 0;
    }

    if (base && base[0] != '\0' && !path_is_sep(path[0])) {
        sources[source_count++] = base;
    }
    sources[source_count++] = path;

    for (int s = 0; s < source_count; s++) {
        const char* cursor = sources[s];
        while (*cursor) {
            while (*cursor && path_is_sep(*cursor)) {
                cursor++;
            }
            if (*cursor == '\0') {
                break;
            }

            char component[32];
            int len = 0;
            while (cursor[len] && !path_is_sep(cursor[len])) {
                if (len >= 31) {
                    return 0;
                }
                component[len] = cursor[len];
                len++;
            }
            component[len] = '\0';
            cursor += len;

            if (k_strcmp(component, ".")) {
                continue;
            }
            if (k_strcmp(component, "..")) {
                if (count > 0) {
                    count--;
                }
                continue;
            }
            if (!path_add_component(comps, &count, component)) {
                return 0;
            }
        }
    }

    if (count == 0) {
        out[0] = '\0';
        return 1;
    }

    unsigned int pos = 0;
    for (int i = 0; i < count; i++) {
        unsigned int len = 0;
        while (comps[i][len] != '\0') {
            len++;
        }

        if (pos + len + (i > 0 ? 1u : 0u) + 1u > out_size) {
            return 0;
        }
        if (i > 0) {
            out[pos++] = '/';
        }
        for (unsigned int j = 0; j < len; j++) {
            out[pos++] = comps[i][j];
        }
    }
    out[pos] = '\0';
    return 1;
}

/* ------------------------------------------------------------------ */
/* Copy-from-user validation                                          */
/* ------------------------------------------------------------------ */

static u32* current_user_pd(void) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return 0;
    return proc->pd;
}

/*
 * user_page_mapped(pd, addr)
 *
 * Return 1 if the 4 KB page containing addr is present and user-accessible
 * in the given page directory.
 */
static int user_page_mapped(u32* pd, unsigned int addr) {
    u32* pd_virt = (u32*)paging_phys_to_kernel_virt((u32)pd);
    u32 pde = pd_virt[addr >> 22];
    if (!(pde & PAGE_PRESENT)) return 0;
    if (!(pde & PAGE_USER))    return 0;

    u32* pt = (u32*)paging_phys_to_kernel_virt(pde & ~0xFFFu);
    u32 pte = pt[(addr >> 12) & 0x3FF];
    if (!(pte & PAGE_PRESENT)) return 0;
    if (!(pte & PAGE_USER))    return 0;
    return 1;
}

/*
 * user_buf_ok(ptr, len)
 *
 * Return 1 only if [ptr, ptr + len) lies entirely in mapped user memory.
 * This validates both address range and page-table presence so kernel code
 * never dereferences an unmapped user page by accident.
 */
static int user_buf_ok(unsigned int ptr, unsigned int len) {
    if (ptr < USER_CODE_BASE)       return 0;
    if (ptr >= USER_STACK_TOP)      return 0;
    if (len == 0)                   return 0;
    if (len > USER_STACK_TOP - ptr) return 0;

    u32* pd = current_user_pd();
    if (!pd) return 0;

    unsigned int start_page = ptr & ~(PAGE_SIZE - 1u);
    unsigned int end_page = (ptr + len - 1u) & ~(PAGE_SIZE - 1u);
    unsigned int page = start_page;

    while (1) {
        if (!user_page_mapped(pd, page)) return 0;
        if (page == end_page) break;
        page += PAGE_SIZE;
    }

    return 1;
}

static int user_count_bytes_ok(unsigned int ptr,
                               unsigned int count,
                               unsigned int elem_size,
                               unsigned int* out_bytes) {
    unsigned int bytes;

    if (elem_size == 0u) return 0;
    if (count > 0xFFFFFFFFu / elem_size) return 0;
    bytes = count * elem_size;
    if (out_bytes) *out_bytes = bytes;
    return user_buf_ok(ptr, bytes);
}

static int copy_from_user(void* dst, const void* src, unsigned int len) {
    if (len == 0u) return 0;
    if (!dst || !src) return -EFAULT;
    if (!user_buf_ok((unsigned int)src, len)) return -EFAULT;
    k_memcpy(dst, src, len);
    return 0;
}

static int copy_to_user(void* dst, const void* src, unsigned int len) {
    if (len == 0u) return 0;
    if (!dst || !src) return -EFAULT;
    if (!user_buf_ok((unsigned int)dst, len)) return -EFAULT;
    k_memcpy(dst, src, len);
    return 0;
}

static int read_user_u32(unsigned int* out, const unsigned int* src) {
    return copy_from_user(out, src, sizeof(*out));
}

static int write_user_u32(unsigned int* dst, unsigned int value) {
    return copy_to_user(dst, &value, sizeof(value));
}

/*
 * copy_user_cstr(dst, dst_size, src)
 *
 * Copy a NUL-terminated string from user space into a kernel buffer.
 * The copy stops at the first '\0'.  Returns the number of bytes copied,
 * including the terminator, or -1 on validation failure or truncation.
 */
static int copy_user_cstr(char* dst, unsigned int dst_size, const char* src) {
    if (!dst || !src || dst_size == 0) return -EFAULT;

    unsigned int ptr = (unsigned int)src;
    if (ptr < USER_CODE_BASE || ptr >= USER_STACK_TOP) return -EFAULT;

    u32* pd = current_user_pd();
    if (!pd) return -EFAULT;

    for (unsigned int i = 0; i < dst_size; i++) {
        unsigned int addr = ptr + i;
        if (addr < USER_CODE_BASE || addr >= USER_STACK_TOP) return -EFAULT;
        if (!user_page_mapped(pd, addr)) return -EFAULT;

        dst[i] = src[i];
        if (dst[i] == '\0') {
            return (int)(i + 1);
        }
    }

    return -ENAMETOOLONG;
}

static int copy_user_path_resolved(char* dst, unsigned int dst_size, const char* src) {
    char raw[PROCESS_FD_NAME_MAX];
    process_t* proc = (process_t*)sched_current();

    int copied = copy_user_cstr(raw, sizeof(raw), src);
    if (copied < 0) return copied;
    if (copied <= 1) return -EINVAL;
    if (!proc) return -EINVAL;
    if (!path_build_from(proc->cwd, raw, dst, dst_size)) return -ENAMETOOLONG;
    return 1;
}

static int path_lookup_errno(const char* path) {
    char prefix[PROCESS_FD_NAME_MAX];
    u32 size = 0;
    int is_dir = 0;

    if (!path || path[0] == '\0') {
        return vfs_is_dir("") ? 0 : -ENOENT;
    }

    for (unsigned int i = 0; path[i] != '\0'; i++) {
        if (path[i] != '/') {
            continue;
        }
        if (i == 0 || i >= sizeof(prefix)) {
            continue;
        }
        k_memcpy(prefix, path, i);
        prefix[i] = '\0';
        if (vfs_stat(prefix, &size, &is_dir) && !is_dir) {
            return -ENOTDIR;
        }
    }

    if (vfs_stat(path, &size, &is_dir) || vfs_is_dir(path)) {
        return 0;
    }
    return -ENOENT;
}

/* ------------------------------------------------------------------ */
/* Syscall implementations                                            */
/* ------------------------------------------------------------------ */

static int sys_write_impl(const char* buf, unsigned int len) {
    process_t* proc;
    fd_entry_t* stdout_ent;

    if (len == 0) return 0;
    if (len > SYSCALL_MAX_WRITE_LEN) return -EFBIG;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    proc = (process_t*)sched_current();
    stdout_ent = proc ? process_fd_get(proc, 1) : 0;
    if (!stdout_ent) {
        terminal_write(buf, len);
        return (int)len;
    }
    return process_fd_write(stdout_ent, buf, len);
}

static int sys_putc_impl(unsigned int ch) {
    char c = (char)ch;
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* stdout_ent = proc ? process_fd_get(proc, 1) : 0;

    if (!stdout_ent) {
        terminal_putc(c);
        return 1;
    }
    return process_fd_write(stdout_ent, &c, 1);
}

static void sys_exit_impl(syscall_regs_t* regs) {
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

static unsigned int sys_get_ticks_impl(void) {
    return timer_get_ticks();
}

static void sys_wait_until_current_running(process_t* proc) {
    __asm__ volatile ("sti");
    while (proc && proc->state != PROCESS_STATE_RUNNING) {
        __asm__ volatile ("hlt");
    }
    __asm__ volatile ("cli");
}

static int sys_yield_impl(void) {
    __asm__ volatile ("sti; hlt; cli");
    return 0;
}

/*
 * sys_sleep_impl(regs, ticks)
 *
 * Block the current process until at least ticks timer ticks have elapsed.
 * The task marks itself SLEEPING, stores a wake deadline, then parks with
 * interrupts enabled.  When the timer reaches the deadline the scheduler
 * wakes the task and this function continues.
 */
static int sys_sleep_impl(syscall_regs_t* regs, unsigned int ticks) {
    if (ticks == 0) return 0;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    proc->sleep_until = timer_get_ticks() + ticks;
    proc->state = PROCESS_STATE_SLEEPING;

    /*
     * Park with interrupts enabled.  The next timer IRQ can switch to another
     * runnable task, and this syscall frame resumes once the sleeper is woken.
     */
    (void)regs;
    sys_wait_until_current_running(proc);
    return 0;
}

/*
 * sys_read_impl — true blocking keyboard read.
 *
 * When the keyboard buffer is empty, the calling process is parked:
 *
 *   1. proc->state is set to PROCESS_STATE_WAITING so that
 *      sched_find_next_runnable_from() skips this task on every timer tick.
 *   2. keyboard_set_waiting_process(proc) registers the waiter so that
 *      process_key_consumer() (IRQ1 context) can wake it.
 *   3. sti; hlt — re-enables interrupts and suspends the CPU.
 *
 * While halted the timer IRQ fires normally.  sched_tick() sees that this
 * task is WAITING, skips it, and switches to another runnable task.  When
 * a keypress arrives, process_key_consumer() sets proc->state back to
 * PROCESS_STATE_RUNNING and clears the waiter slot.  On the next timer
 * tick sched_tick() selects this task again; execution resumes after the
 * hlt instruction, re-checks keyboard_buf_available(), finds the character,
 * and continues normally.
 *
 * The outer while loop re-checks the buffer on every wakeup, which
 * correctly handles spurious wakeups (none expected today, but the
 * guard is cheap and correct).
 *
 * IF management:
 *   The syscall gate is an interrupt gate so the CPU clears IF on entry.
 *   We re-enable with sti before the first hlt so IRQ1 can fire.  After
 *   all characters have been collected we restore cli before returning,
 *   matching the expected IF=0 postcondition of the syscall gate.
 */
static int sys_read_impl(char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    return process_fd_read(process_fd_get(proc, 0), buf, len);
}

static int sys_read_raw_impl(char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    return process_fd_read_raw(process_fd_get(proc, 0), buf, len);
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

static int sys_exec_impl(const char* name, int argc, char** argv) {
    return sys_exec_spawn_impl(name, argc, argv, 0);
}

static int sys_exec_fg_impl(const char* name, int argc, char** argv) {
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

static int sys_pipe2_impl(int* user_fds, unsigned int flags) {
    process_t* proc = (process_t*)sched_current();
    int fds[2];
    int rc;

    if (!proc) return -EINVAL;
    if (!user_buf_ok((unsigned int)user_fds, sizeof(fds))) return -EFAULT;
    rc = process_fd_pipe(proc, fds, flags);
    if (rc < 0) return rc;
    if (copy_to_user(user_fds, fds, sizeof(fds)) < 0) {
        sys_close_impl(fds[0]);
        sys_close_impl(fds[1]);
        return -EFAULT;
    }
    return 0;
}

static int sys_pty_open_impl(int* user_fds, unsigned int master_flags) {
    process_t* proc = (process_t*)sched_current();
    int fds[2];
    int rc;

    if (!proc) return -EINVAL;
    if (!user_buf_ok((unsigned int)user_fds, sizeof(fds))) return -EFAULT;
    rc = process_fd_pty(proc, fds, master_flags);
    if (rc < 0) return rc;
    if (copy_to_user(user_fds, fds, sizeof(fds)) < 0) {
        sys_close_impl(fds[0]);
        sys_close_impl(fds[1]);
        return -EFAULT;
    }
    return 0;
}

static int sys_pty_set_size_impl(int fd, unsigned int rows, unsigned int cols) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    return process_fd_pty_set_size(process_fd_get(proc, fd), rows, cols);
}

static int sys_fork_impl(syscall_regs_t* regs) {
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

static int sys_execve_impl(syscall_regs_t* regs, const char* name, char** argv, char** envp) {
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
    if (status >= 128 && status < 256) {
        return status - 128;
    }
    return (status & 0xFF) << 8;
}

static int sys_getpid_impl(void) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    return (int)proc->pid;
}

static int sys_waitpid_impl(int pid, int* user_status, int options) {
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

static int sys_waitpid_fg_impl(int pid, int* user_status) {
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

static int sys_kill_impl(syscall_regs_t* regs, int pid, int signum) {
    if (signum <= 0 || signum >= 32) return -EINVAL;
    return process_kill_pid(pid, 128 + signum, (unsigned int)regs);
}

/*
 * sys_writefile_impl(name, buf, len)
 *
 * Create or overwrite a root-directory ext2 file in one shot.  This is
 * the historical output primitive for user-space tools.
 */
static int sys_writefile_impl(const char* name, const void* buf, unsigned int len) {
    char kname[EXEC_NAME_MAX];
    int name_rc = copy_user_cstr(kname, sizeof(kname), name);
    if (name_rc < 0) return name_rc;
    if (name_rc <= 1) return -EINVAL;
    if (len > 0 && !user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    return vfs_write_root(kname, (const u8*)buf, len) ? 0 : -EIO;
}

/*
 * sys_writefile_path_impl(path, buf, len)
 *
 * Create or overwrite an ext2 file at an arbitrary path.  This is the
 * preferred output primitive for compilers because it can emit directly
 * into nested directories.
 */
static int sys_writefile_path_impl(const char* path, const void* buf, unsigned int len) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    if (len > 0 && !user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    return vfs_write_path(kpath, (const u8*)buf, len) ? 0 : -EIO;
}

/*
 * heap_page_table_empty(pt)
 *
 * Return 1 if the page table has no present entries.
 */
static int heap_page_table_empty(u32* pt) {
    for (unsigned int i = 0; i < 1024u; i++) {
        if (pt[i] & PAGE_PRESENT) {
            return 0;
        }
    }
    return 1;
}

/*
 * heap_unmap_page(pd, virt)
 *
 * Unmap the page containing virt from the given user page directory.
 * If the page table becomes empty, free it as well.
 */
static void heap_unmap_page(u32* pd, unsigned int virt) {
    unsigned int pd_index = virt >> 22;
    unsigned int pt_index = (virt >> 12) & 0x3FFu;

    u32* pd_virt = (u32*)paging_phys_to_kernel_virt((u32)pd);
    u32 pde = pd_virt[pd_index];
    if (!(pde & PAGE_PRESENT)) {
        return;
    }

    u32 pt_phys = pde & ~0xFFFu;
    u32* pt = (u32*)paging_phys_to_kernel_virt(pt_phys);
    u32 pte = pt[pt_index];
    if (!(pte & PAGE_PRESENT)) {
        return;
    }

    pmm_free_frame(pte & ~0xFFFu);
    pt[pt_index] = 0;
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");

    if (heap_page_table_empty(pt)) {
        pd_virt[pd_index] = 0;
        pmm_free_frame(pt_phys);
    }
}

/*
 * sys_brk_impl(new_brk)
 *
 * Query or adjust the calling process heap break.
 *
 * Passing 0 returns the current break.  Growing the break maps new user
 * pages on demand.  Shrinking the break unmaps whole pages above the new
 * limit and returns the updated value.
 */
static unsigned int sys_brk_impl(unsigned int new_brk) {
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

    u32* pd = proc->pd;

    if (new_brk > cur_brk) {
        unsigned int map_start = PAGE_ALIGN(cur_brk);
        unsigned int map_end = PAGE_ALIGN(new_brk);
        unsigned int mapped = map_start;

        for (unsigned int addr = map_start; addr < map_end; addr += PAGE_SIZE) {
            u32 frame = pmm_alloc_frame();
            if (!frame) {
                for (unsigned int undo = map_start; undo < mapped; undo += PAGE_SIZE) {
                    heap_unmap_page(pd, undo);
                }
                return cur_brk;
            }
            k_memset(paging_phys_to_kernel_virt(frame), 0, PAGE_SIZE);
            paging_map_page(pd, addr, frame, PAGE_WRITE | PAGE_USER);
            mapped = addr + PAGE_SIZE;
        }

        proc->heap_brk = new_brk;
        return new_brk;
    }

    {
        unsigned int unmap_start = PAGE_ALIGN(new_brk);
        unsigned int unmap_end = PAGE_ALIGN(cur_brk);

        for (unsigned int addr = unmap_start; addr < unmap_end; addr += PAGE_SIZE) {
            heap_unmap_page(pd, addr);
        }

        proc->heap_brk = new_brk;
        return new_brk;
    }
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
static int sys_open_impl(const char* name) {
    char kname[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kname, sizeof(kname), name);
    if (path_rc < 0) return path_rc;

    u32 file_size = 0;
    int is_dir = 0;
    if (!vfs_stat(kname, &file_size, &is_dir)) return path_lookup_errno(kname);

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    int fd = process_fd_open_file(proc, kname, file_size, 0);
    fd_entry_t* ent = process_fd_get(proc, fd);
    if (ent) vfs_file_set_is_dir(ent, is_dir);
    return fd;
}

/*
 * sys_close_impl(fd)
 *
 * Mark the fd slot as free.  Returns 0 on success or a negative errno
 * on bad fd/current process state.
 */
static int sys_close_impl(int fd) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;

    process_fd_close(ent);
    return 0;
}

static int sys_open_write_impl(const char* name) {
    char kname[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kname, sizeof(kname), name);
    if (path_rc < 0) return path_rc;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    int fd = process_fd_open_file(proc, kname, 0, 1);
    if (fd < 0) return fd;
    if (!vfs_write_path(kname, 0, 0)) {
        sys_close_impl(fd);
        return -EIO;
    }
    return fd;
}

static int sys_open_mode_impl(const char* name, unsigned int mode) {
    char kname[PROCESS_FD_NAME_MAX];
    unsigned int supported = SYS_OPEN_MODE_READ | SYS_OPEN_MODE_WRITE |
                             SYS_OPEN_MODE_CREATE | SYS_OPEN_MODE_TRUNC |
                             SYS_OPEN_MODE_APPEND | SYS_OPEN_MODE_EXCL;
    int readable = (mode & SYS_OPEN_MODE_READ) != 0;
    int writable = (mode & SYS_OPEN_MODE_WRITE) != 0;
    int create = (mode & SYS_OPEN_MODE_CREATE) != 0;
    int trunc = (mode & SYS_OPEN_MODE_TRUNC) != 0;
    int append = (mode & SYS_OPEN_MODE_APPEND) != 0;
    int excl = (mode & SYS_OPEN_MODE_EXCL) != 0;
    u32 file_size = 0;
    int is_dir = 0;
    int exists;
    int fd;

    if ((mode & ~supported) != 0) return -EINVAL;
    if (!readable && !writable) return -EINVAL;
    if ((trunc || append || create) && !writable) return -EINVAL;
    int path_rc = copy_user_path_resolved(kname, sizeof(kname), name);
    if (path_rc < 0) return path_rc;

    exists = vfs_stat(kname, &file_size, &is_dir);
    if (exists && create && excl) return -EEXIST;
    if (exists && is_dir && writable) return -EISDIR;
    if (!exists && !create) return path_lookup_errno(kname);
    if (!exists || trunc) file_size = 0;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    fd = process_fd_open_file_mode(proc, kname, file_size, readable, writable);
    if (fd < 0) return fd;

    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) {
        sys_close_impl(fd);
        return -EBADF;
    }
    vfs_file_set_is_dir(ent, (exists && is_dir) ? 1 : 0);

    if (!exists || trunc) {
        if (!vfs_write_path(kname, 0, 0)) {
            sys_close_impl(fd);
            return -EIO;
        }
    }

    if (append) {
        if (process_fd_seek(ent, 0, 2) < 0) {
            sys_close_impl(fd);
            return -EINVAL;
        }
    }

    return fd;
}

static int copy_user_sockaddr_in(struct sockaddr_in* dst,
                                 const struct sockaddr* src,
                                 unsigned int len) {
    if (!dst || !src) {
        return -EFAULT;
    }
    if (len < sizeof(struct sockaddr_in)) {
        return -EINVAL;
    }
    if (!user_buf_ok((unsigned int)src, sizeof(struct sockaddr_in))) {
        return -EFAULT;
    }

    if (copy_from_user(dst, src, sizeof(struct sockaddr_in)) < 0) {
        return -EFAULT;
    }
    if (dst->sin_family != AF_INET) {
        return -EINVAL;
    }
    return 0;
}

static int socket_fd_is_socket(fd_entry_t* ent) {
    return ent && ent->valid && ent->kind == PROCESS_HANDLE_KIND_SOCKET && ent->socket;
}

static unsigned short swap_u16(unsigned short value) {
    return (unsigned short)(((value & 0x00FFu) << 8) | ((value & 0xFF00u) >> 8));
}

static unsigned int swap_u32(unsigned int value) {
    return ((value & 0x000000FFu) << 24)
         | ((value & 0x0000FF00u) << 8)
         | ((value & 0x00FF0000u) >> 8)
         | ((value & 0xFF000000u) >> 24);
}

static int sys_socket_impl(int domain, int type, int protocol) {
    process_t* proc = (process_t*)sched_current();
    int fd;
    int sock_type = type & SOCK_TYPE_MASK;
    unsigned int fd_flags = 0u;

    if (!proc) return -EINVAL;
    if (domain != AF_INET) return -EINVAL;
    if (sock_type != SOCK_STREAM) return -EINVAL;
    if ((type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC)) != 0) return -EINVAL;
    if (protocol != 0 && protocol != IPPROTO_TCP) return -EINVAL;

    fd = process_fd_open_socket(proc, "socket");
    if (fd < 0) return fd;
    if ((type & SOCK_NONBLOCK) != 0) {
        fd_entry_t* ent = process_fd_get(proc, fd);
        fd_flags |= SYS_FD_FLAG_NONBLOCK;
        (void)process_fd_set_flags(ent, fd_flags);
    }
    if ((type & SOCK_CLOEXEC) != 0) {
        fd_entry_t* ent = process_fd_get(proc, fd);
        (void)process_fd_set_fd_flags(ent, SYS_FD_FLAG_CLOEXEC);
    }
    return fd;
}

static int sys_bind_impl(int fd, const struct sockaddr* addr, unsigned int addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_state(ent->socket) != SOCKET_STATE_OPEN) return -EINVAL;
    int sa_rc = copy_user_sockaddr_in(&sa, addr, addrlen);
    if (sa_rc < 0) return sa_rc;

    sa_rc = socket_bind_tcp(ent->socket, swap_u16(sa.sin_port));
    if (sa_rc < 0) return sa_rc;
    ent->socket_port = socket_local_port(ent->socket);
    ent->socket_state = PROCESS_SOCKET_STATE_BOUND;
    return 0;
}

static int sys_listen_impl(int fd, int backlog) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    (void)backlog;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_state(ent->socket) != SOCKET_STATE_BOUND) return -EINVAL;
    if (socket_local_port(ent->socket) == 0u) return -EINVAL;

    int listen_rc = socket_listen_tcp(ent->socket, backlog);
    if (listen_rc < 0) return listen_rc;
    ent->socket_port = socket_local_port(ent->socket);
    ent->socket_state = PROCESS_SOCKET_STATE_LISTENER;
    return 0;
}

static int sys_accept_impl(syscall_regs_t* regs,
                           int fd,
                           struct sockaddr* addr,
                           unsigned int* addrlen,
                           unsigned int flags) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    fd_entry_t* new_ent;
    unsigned int peer_ip;
    unsigned int peer_port;
    int new_fd;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_state(ent->socket) != SOCKET_STATE_LISTENING) return -EINVAL;
    if ((flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) != 0u) return -EINVAL;

    if (!socket_accept_ready(ent->socket) &&
        ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u ||
         (flags & SOCK_NONBLOCK) != 0u)) {
        return -EAGAIN;
    }

    while (!socket_accept_ready(ent->socket)) {
        int wait_rc;

        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_accept_ready(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);

    new_fd = process_fd_open_socket(proc, "socket");
    if (new_fd < 0) return new_fd;
    new_ent = process_fd_get(proc, new_fd);
    if (!socket_fd_is_socket(new_ent)) {
        process_fd_close(new_ent);
        return -EBADF;
    }
    if (socket_accept_tcp(ent->socket, new_ent->socket) < 0) {
        process_fd_close(new_ent);
        return -EAGAIN;
    }
    new_ent->socket_state = PROCESS_SOCKET_STATE_CONNECTED;
    new_ent->socket_port = socket_local_port(new_ent->socket);
    new_ent->socket_conn = socket_conn_id(new_ent->socket);
    if ((flags & SOCK_NONBLOCK) != 0u) {
        new_ent->flags |= SYS_FD_FLAG_NONBLOCK;
    }
    if ((flags & SOCK_CLOEXEC) != 0u) {
        new_ent->fd_flags |= SYS_FD_FLAG_CLOEXEC;
    }

    peer_ip = socket_peer_ip(new_ent->socket);
    peer_port = socket_peer_port(new_ent->socket);
    if (addr && addrlen) {
        struct sockaddr_in sa;
        unsigned int user_addrlen = 0;
        if (read_user_u32(&user_addrlen, addrlen) < 0) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -EFAULT;
        }
        if (user_addrlen < sizeof(struct sockaddr_in)) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -EINVAL;
        }
        if (!user_buf_ok((unsigned int)addr, sizeof(struct sockaddr_in))) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -EFAULT;
        }
        k_memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = swap_u16((unsigned short)peer_port);
        sa.sin_addr.s_addr = swap_u32(peer_ip);
        if (copy_to_user(addr, &sa, sizeof(sa)) < 0 ||
            write_user_u32(addrlen, sizeof(sa)) < 0) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -EFAULT;
        }
    }

    return new_fd;
}

static int sys_connect_impl(int fd, const struct sockaddr* addr, unsigned int addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;
    unsigned int remote_ip;
    unsigned int remote_port;
    int rc;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    rc = copy_user_sockaddr_in(&sa, addr, addrlen);
    if (rc < 0) return rc;

    remote_ip = swap_u32(sa.sin_addr.s_addr);
    remote_port = swap_u16(sa.sin_port);
    rc = socket_connect_tcp(ent->socket, remote_ip, remote_port);
    if (rc == -EALREADY && (ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
        return -EALREADY;
    }
    if (rc < 0) return rc;

    ent->socket_state = socket_tcp_connection_established(ent->socket)
                      ? PROCESS_SOCKET_STATE_CONNECTED
                      : PROCESS_SOCKET_STATE_CONNECTING;
    ent->socket_port = socket_local_port(ent->socket);
    ent->socket_conn = socket_conn_id(ent->socket);

    if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
        if (!socket_tcp_connection_established(ent->socket)) {
            return -EINPROGRESS;
        }
        ent->socket_state = PROCESS_SOCKET_STATE_CONNECTED;
        return 0;
    }

    while (!socket_tcp_connection_established(ent->socket)) {
        int wait_rc;

        if (!socket_tcp_connect_pending(ent->socket)) {
            socket_wait_clear_process(proc);
            return -ECONNREFUSED;
        }
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLOUT);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_tcp_connection_established(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);
    ent->socket_state = PROCESS_SOCKET_STATE_CONNECTED;
    return 0;
}

static int sys_send_impl(int fd, const void* buf, unsigned int len) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    if (len == 0u) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    return process_fd_write(ent, (const char*)buf, len);
}

static int sys_recv_impl(syscall_regs_t* regs, int fd, void* buf, unsigned int len) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    int rc;

    if (!proc) return -EINVAL;
    if (len == 0u) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_state(ent->socket) != SOCKET_STATE_CONNECTED &&
        socket_state(ent->socket) != SOCKET_STATE_CONNECTING) return -EINVAL;

    if (!socket_tcp_recv_ready(ent->socket) &&
        (ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
        return -EAGAIN;
    }

    while (!socket_tcp_recv_ready(ent->socket)) {
        int wait_rc;

        if (!socket_tcp_connection_established(ent->socket)) {
            if (!socket_tcp_connect_pending(ent->socket)) {
                return 0;
            }
            proc->state = PROCESS_STATE_WAITING;
            wait_rc = socket_wait(ent->socket, proc, POLLOUT);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                socket_wait_clear_process(proc);
                return wait_rc;
            }
            (void)regs;
            sys_wait_until_current_running(proc);
            socket_wait_clear_process(proc);
            continue;
        }
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_tcp_recv_ready(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        (void)regs;
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);

    rc = socket_tcp_recv(ent->socket, buf, len);
    return rc < 0 ? -ECONNRESET : rc;
}

static short sys_poll_revents_for_fd(process_t* proc, struct pollfd* pfd) {
    fd_entry_t* ent = process_fd_get(proc, pfd->fd);
    return process_fd_poll(ent, pfd->events);
}

static unsigned int sys_poll_snapshot(process_t* proc, struct pollfd* fds,
                                      unsigned int nfds) {
    unsigned int ready = 0u;

    for (unsigned int i = 0; i < nfds; i++) {
        short revents = sys_poll_revents_for_fd(proc, &fds[i]);
        fds[i].revents = revents;
        if (revents) {
            ready++;
        }
    }

    return ready;
}

static int sys_poll_register_fd_waits(process_t* proc,
                                      struct pollfd* fds,
                                      unsigned int nfds) {
    if (!proc || !fds) return -EINVAL;

    for (unsigned int i = 0; i < nfds; i++) {
        fd_entry_t* ent = process_fd_get(proc, fds[i].fd);
        int rc;

        if (!ent) continue;

        rc = process_fd_wait(ent, proc, fds[i].events);
        if (rc < 0) {
            socket_wait_clear_process(proc);
            return rc;
        }
    }

    return 0;
}

static unsigned int sys_poll_timeout_ticks(int timeout_ms) {
    if (timeout_ms <= 0) {
        return 0u;
    }

    return timer_ms_to_ticks_round_up((unsigned int)timeout_ms);
}

static int sys_poll_impl(syscall_regs_t* regs, struct pollfd* fds,
                         unsigned int nfds, int timeout) {
    process_t* proc = (process_t*)sched_current();
    unsigned int timeout_ticks;
    unsigned int deadline;
    int infinite_wait;

    if (!proc) return -EINVAL;
    if (nfds == 0u) return 0;
    if (nfds > POLL_MAX_FDS) return -EINVAL;
    if (!user_count_bytes_ok((unsigned int)fds, nfds, sizeof(struct pollfd), 0)) {
        return -EFAULT;
    }

    infinite_wait = (timeout < 0);
    timeout_ticks = infinite_wait ? 0u : sys_poll_timeout_ticks(timeout);
    deadline = infinite_wait ? 0u : (timer_get_ticks() + timeout_ticks);

    for (;;) {
        unsigned int ready = sys_poll_snapshot(proc, fds, nfds);
        if (ready != 0u) {
            socket_wait_clear_process(proc);
            wait_queue_remove_proc(proc);
            return (int)ready;
        }

        if (!infinite_wait && (int)(timer_get_ticks() - deadline) >= 0) {
            socket_wait_clear_process(proc);
            wait_queue_remove_proc(proc);
            return 0;
        }

        proc->sleep_until = infinite_wait ? 0u : deadline;
        proc->state = infinite_wait ? PROCESS_STATE_WAITING
                                    : PROCESS_STATE_SLEEPING;
        {
            int wait_rc = sys_poll_register_fd_waits(proc, fds, nfds);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                socket_wait_clear_process(proc);
                wait_queue_remove_proc(proc);
                return wait_rc;
            }
        }
        ready = sys_poll_snapshot(proc, fds, nfds);
        if (ready != 0u) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            wait_queue_remove_proc(proc);
            return (int)ready;
        }

        (void)regs;
        sys_wait_until_current_running(proc);

        socket_wait_clear_process(proc);
        wait_queue_remove_proc(proc);
    }
}

static int sys_fcntl_impl(int fd, int cmd, unsigned int arg) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;

    if (cmd == SYS_FCNTL_GETFD) {
        return (int)process_fd_get_fd_flags(ent);
    }
    if (cmd == SYS_FCNTL_SETFD) {
        return process_fd_set_fd_flags(ent, arg);
    }
    if (cmd == SYS_FCNTL_GETFL) {
        return (int)process_fd_get_flags(ent);
    }
    if (cmd == SYS_FCNTL_SETFL) {
        return process_fd_set_flags(ent, arg);
    }

    return -EINVAL;
}

static epoll_watch_t* epoll_watches(fd_entry_t* ent, int create) {
    if (!ent || ent->kind != PROCESS_HANDLE_KIND_EPOLL) return 0;
    if (!ent->aux_frame && create) {
        ent->aux_frame = pmm_alloc_frame();
        if (!ent->aux_frame) return 0;
        k_memset(paging_phys_to_kernel_virt(ent->aux_frame), 0, PAGE_SIZE);
    }
    if (!ent->aux_frame) return 0;
    return (epoll_watch_t*)paging_phys_to_kernel_virt(ent->aux_frame);
}

static int epoll_find_watch(epoll_watch_t* watches, int fd) {
    if (!watches) return -1;
    for (unsigned int i = 0; i < EPOLL_MAX_WATCHES; i++) {
        if (watches[i].used && watches[i].fd == fd) {
            return (int)i;
        }
    }
    return -1;
}

static int sys_epoll_create_impl(int flags) {
    process_t* proc = (process_t*)sched_current();
    int fd;
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    if ((flags & ~EPOLL_CLOEXEC) != 0) return -EINVAL;

    fd = process_fd_open_special(proc, PROCESS_HANDLE_KIND_EPOLL, "epoll");
    if (fd < 0) return fd;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if ((flags & EPOLL_CLOEXEC) != 0) {
        (void)process_fd_set_fd_flags(ent, SYS_FD_FLAG_CLOEXEC);
    }
    return fd;
}

static int sys_epoll_ctl_impl(int epfd, int op, int fd,
                              struct epoll_event* user_event) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* epent;
    fd_entry_t* target;
    epoll_watch_t* watches;
    struct epoll_event event;
    int idx;

    if (!proc) return -EINVAL;
    epent = process_fd_get(proc, epfd);
    if (!epent || epent->kind != PROCESS_HANDLE_KIND_EPOLL) return -EBADF;
    target = process_fd_get(proc, fd);
    if (!target) return -EBADF;
    if (fd == epfd) return -EINVAL;

    if (op != EPOLL_CTL_DEL) {
        if (!user_event ||
            !user_buf_ok((unsigned int)user_event, sizeof(*user_event))) {
            return -EFAULT;
        }
        if (copy_from_user(&event, user_event, sizeof(event)) < 0) {
            return -EFAULT;
        }
    } else {
        k_memset(&event, 0, sizeof(event));
    }

    watches = epoll_watches(epent, op != EPOLL_CTL_DEL);
    if (!watches) return -ENOMEM;
    idx = epoll_find_watch(watches, fd);

    if (op == EPOLL_CTL_ADD) {
        if (idx >= 0) return -EEXIST;
        for (unsigned int i = 0; i < EPOLL_MAX_WATCHES; i++) {
            if (!watches[i].used) {
                watches[i].used = 1;
                watches[i].fd = fd;
                watches[i].events = event.events;
                watches[i].data_u32 = event.data.u32;
                return 0;
            }
        }
        return -ENFILE;
    }

    if (op == EPOLL_CTL_MOD) {
        if (idx < 0) return -ENOENT;
        watches[idx].events = event.events;
        watches[idx].data_u32 = event.data.u32;
        return 0;
    }

    if (op == EPOLL_CTL_DEL) {
        if (idx < 0) return -ENOENT;
        k_memset(&watches[idx], 0, sizeof(watches[idx]));
        return 0;
    }

    return -EINVAL;
}

static unsigned int epoll_snapshot(process_t* proc,
                                   epoll_watch_t* watches,
                                   struct epoll_event* events,
                                   unsigned int maxevents) {
    unsigned int ready = 0u;

    if (!proc || !watches || !events) return 0u;

    for (unsigned int i = 0; i < EPOLL_MAX_WATCHES && ready < maxevents; i++) {
        if (!watches[i].used) continue;

        fd_entry_t* ent = process_fd_get(proc, watches[i].fd);
        short revents;

        if (!ent) {
            revents = EPOLLERR;
        } else {
            revents = process_fd_poll(ent, (short)(watches[i].events & 0xFFFFu));
        }

        if (revents) {
            k_memset(&events[ready], 0, sizeof(events[ready]));
            events[ready].events = (unsigned int)revents;
            events[ready].data.u32 = watches[i].data_u32;
            ready++;
        }
    }

    return ready;
}

static int epoll_register_fd_waits(process_t* proc, epoll_watch_t* watches) {
    if (!proc) return -EINVAL;
    if (!watches) return 0;

    for (unsigned int i = 0; i < EPOLL_MAX_WATCHES; i++) {
        fd_entry_t* ent;
        int rc;

        if (!watches[i].used) continue;

        ent = process_fd_get(proc, watches[i].fd);
        if (!ent) continue;

        rc = process_fd_wait(ent,
                             proc,
                             (short)(watches[i].events & 0xFFFFu));
        if (rc < 0) {
            socket_wait_clear_process(proc);
            return rc;
        }
    }

    return 0;
}

static int sys_epoll_wait_impl(syscall_regs_t* regs,
                               int epfd,
                               struct epoll_event* events,
                               int maxevents,
                               int timeout) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* epent;
    epoll_watch_t* watches;
    unsigned int timeout_ticks;
    unsigned int timeout_deadline;
    int infinite_wait;

    if (!proc) return -EINVAL;
    if (maxevents <= 0) return -EINVAL;
    if (!events) return -EFAULT;
    if (maxevents > (int)EPOLL_MAX_WATCHES) return -EINVAL;
    if (!user_count_bytes_ok((unsigned int)events,
                             (unsigned int)maxevents,
                             sizeof(struct epoll_event),
                             0)) {
        return -EFAULT;
    }

    epent = process_fd_get(proc, epfd);
    if (!epent || epent->kind != PROCESS_HANDLE_KIND_EPOLL) return -EBADF;
    watches = epoll_watches(epent, 0);
    if (!watches) {
        if (timeout == 0) return 0;
    }

    infinite_wait = timeout < 0;
    timeout_ticks = infinite_wait ? 0u : sys_poll_timeout_ticks(timeout);
    timeout_deadline = infinite_wait ? 0u : timer_get_ticks() + timeout_ticks;

    for (;;) {
        unsigned int ready = watches ? epoll_snapshot(proc, watches, events,
                                                      (unsigned int)maxevents)
                                     : 0u;
        if (ready != 0u) {
            socket_wait_clear_process(proc);
            wait_queue_remove_proc(proc);
            return (int)ready;
        }

        if (!infinite_wait && (int)(timer_get_ticks() - timeout_deadline) >= 0) {
            socket_wait_clear_process(proc);
            wait_queue_remove_proc(proc);
            return 0;
        }

        unsigned int sleep_deadline = 0u;

        if (!infinite_wait) {
            sleep_deadline = timeout_deadline;
        }

        if (sleep_deadline != 0u) {
            proc->sleep_until = sleep_deadline;
            proc->state = PROCESS_STATE_SLEEPING;
        } else {
            proc->sleep_until = 0u;
            proc->state = PROCESS_STATE_WAITING;
        }
        {
            int wait_rc = epoll_register_fd_waits(proc, watches);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                socket_wait_clear_process(proc);
                wait_queue_remove_proc(proc);
                return wait_rc;
            }
        }
        ready = watches ? epoll_snapshot(proc, watches, events,
                                         (unsigned int)maxevents)
                        : 0u;
        if (ready != 0u) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            wait_queue_remove_proc(proc);
            return (int)ready;
        }

        (void)regs;
        sys_wait_until_current_running(proc);

        socket_wait_clear_process(proc);
        wait_queue_remove_proc(proc);
    }
}

static unsigned int timerfd_timespec_to_ticks(unsigned int sec, long nsec) {
    unsigned int hz = timer_get_hz();
    unsigned int ticks;
    unsigned int ns_per_tick;

    if (nsec < 0 || nsec >= (long)SMALLOS_NS_PER_SECOND) {
        return 0xFFFFFFFFu;
    }
    if (hz == 0u) return 0xFFFFFFFFu;
    if (sec > 0xFFFFFFFEu / hz) return 0xFFFFFFFEu;

    ticks = sec * hz;
    if (nsec > 0) {
        ns_per_tick = SMALLOS_NS_PER_SECOND / hz;
        ticks += ((unsigned int)nsec + ns_per_tick - 1u) / ns_per_tick;
    }
    return ticks;
}

static int sys_timerfd_create_impl(int clock_id, int flags) {
    process_t* proc = (process_t*)sched_current();
    int fd;
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) return -EINVAL;
    if ((flags & ~(SYS_FD_FLAG_NONBLOCK | SOCK_CLOEXEC)) != 0) return -EINVAL;

    fd = process_fd_open_special(proc, PROCESS_HANDLE_KIND_TIMERFD, "timerfd");
    if (fd < 0) return fd;
    ent = process_fd_get(proc, fd);
    if (ent) {
        (void)process_fd_set_flags(ent, flags);
        if ((flags & SOCK_CLOEXEC) != 0) {
            (void)process_fd_set_fd_flags(ent, SYS_FD_FLAG_CLOEXEC);
        }
    }
    return fd;
}

static int sys_clock_gettime_impl(int clock_id, struct user_timespec* ts) {
    struct user_timespec out;
    unsigned int ticks;
    unsigned int rem;

    if (!ts) return -EFAULT;
    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) return -EINVAL;

    ticks = timer_get_ticks();
    rem = ticks % timer_get_hz();
    out.tv_sec = (clock_id == CLOCK_REALTIME)
               ? timer_get_realtime_seconds()
               : timer_get_seconds();
    out.tv_nsec = (long)(rem * (SMALLOS_NS_PER_SECOND / timer_get_hz()));
    return copy_to_user(ts, &out, sizeof(out));
}

static int sys_clock_settime_impl(int clock_id, const struct user_timespec* ts) {
    struct user_timespec in;

    if (!ts) return -EFAULT;
    if (clock_id != CLOCK_REALTIME) return -EINVAL;
    if (copy_from_user(&in, ts, sizeof(in)) < 0) return -EFAULT;
    if (in.tv_nsec < 0 || in.tv_nsec >= (long)SMALLOS_NS_PER_SECOND) return -EINVAL;

    timer_set_realtime_seconds(in.tv_sec);
    return 0;
}

static int sys_ntp_sync_impl(unsigned int server_ip, struct user_timespec* out_ts) {
    struct user_timespec out;
    unsigned int unix_time;

    __asm__ __volatile__("sti");
    if (!ntp_sync(server_ip, &unix_time)) {
        return -ETIMEDOUT;
    }

    timer_set_realtime_seconds(unix_time);
    if (out_ts) {
        out.tv_sec = unix_time;
        out.tv_nsec = 0;
        return copy_to_user(out_ts, &out, sizeof(out));
    }
    return 0;
}

static int sys_timerfd_settime_impl(int fd,
                                    int flags,
                                    const struct user_itimerspec* new_value,
                                    struct user_itimerspec* old_value) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct user_itimerspec spec;
    unsigned int first_ticks;
    unsigned int interval_ticks;

    if (!proc) return -EINVAL;
    if (flags != 0) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent || ent->kind != PROCESS_HANDLE_KIND_TIMERFD) return -EBADF;
    if (!new_value ||
        !user_buf_ok((unsigned int)new_value, sizeof(*new_value))) {
        return -EFAULT;
    }
    if (old_value &&
        !user_buf_ok((unsigned int)old_value, sizeof(*old_value))) {
        return -EFAULT;
    }

    if (old_value) {
        struct user_itimerspec zero;
        k_memset(&zero, 0, sizeof(zero));
        if (copy_to_user(old_value, &zero, sizeof(zero)) < 0) {
            return -EFAULT;
        }
    }

    if (copy_from_user(&spec, new_value, sizeof(spec)) < 0) {
        return -EFAULT;
    }
    first_ticks = timerfd_timespec_to_ticks(spec.it_value.tv_sec,
                                            spec.it_value.tv_nsec);
    interval_ticks = timerfd_timespec_to_ticks(spec.it_interval.tv_sec,
                                               spec.it_interval.tv_nsec);
    if (first_ticks == 0xFFFFFFFFu || interval_ticks == 0xFFFFFFFFu) {
        return -EINVAL;
    }

    ent->timer_interval = interval_ticks;
    ent->timer_deadline = first_ticks ? timer_get_ticks() + first_ticks : 0u;
    return 0;
}

static int sys_signalfd_impl(int fd, const void* mask, int flags) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    int out_fd = fd;
    unsigned int kernel_mask = 0u;
    int rc;

    if (!proc) return -EINVAL;
    if (mask && !user_buf_ok((unsigned int)mask, sizeof(unsigned int))) {
        return -EFAULT;
    }
    if ((flags & ~(SYS_FD_FLAG_NONBLOCK | SOCK_CLOEXEC)) != 0) return -EINVAL;

    if (fd < 0) {
        out_fd = process_fd_open_special(proc, PROCESS_HANDLE_KIND_SIGNALFD, "signalfd");
        if (out_fd < 0) return out_fd;
    }

    ent = process_fd_get(proc, out_fd);
    if (!ent || ent->kind != PROCESS_HANDLE_KIND_SIGNALFD) return -EBADF;
    if (mask) {
        if (copy_from_user(&kernel_mask, mask, sizeof(kernel_mask)) < 0) {
            return -EFAULT;
        }
        rc = process_fd_set_signalfd_mask(ent, kernel_mask);
        if (rc < 0) return rc;
    } else if (fd < 0) {
        rc = process_fd_set_signalfd_mask(ent, kernel_mask);
        if (rc < 0) return rc;
    }
    (void)process_fd_set_flags(ent, flags);
    if ((flags & SOCK_CLOEXEC) != 0) {
        (void)process_fd_set_fd_flags(ent, SYS_FD_FLAG_CLOEXEC);
    }
    return out_fd;
}

static int sys_shutdown_impl(int fd, int how) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    return socket_shutdown_tcp(ent->socket, how);
}

static int sys_getpeername_impl(int fd, struct sockaddr* addr, unsigned int* addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_state(ent->socket) != SOCKET_STATE_CONNECTED) return -EINVAL;
    if (!addr || !addrlen) return -EFAULT;
    {
        unsigned int user_addrlen = 0;
        if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
        if (user_addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    }
    if (!user_buf_ok((unsigned int)addr, sizeof(struct sockaddr_in))) return -EFAULT;

    k_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = swap_u16((unsigned short)socket_peer_port(ent->socket));
    sa.sin_addr.s_addr = swap_u32(socket_peer_ip(ent->socket));
    if (copy_to_user(addr, &sa, sizeof(sa)) < 0 ||
        write_user_u32(addrlen, sizeof(sa)) < 0) {
        return -EFAULT;
    }
    return 0;
}

static int sys_mkdir_impl(const char* path, unsigned int mode) {
    char kpath[PROCESS_FD_NAME_MAX];
    (void)mode;
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    return vfs_mkdir(kpath) ? 0 : -EIO;
}

static int sys_rmdir_impl(const char* path) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    return vfs_rmdir(kpath) ? 0 : path_lookup_errno(kpath);
}

static int sys_dirlist_impl(const char* path, unsigned int index, uapi_dirent_t* out) {
    char kpath[PROCESS_FD_NAME_MAX];
    char name[UAPI_DIRENT_NAME_MAX];
    unsigned int size = 0;
    int is_dir = 0;
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    if (!out) return -EFAULT;
    if (!user_buf_ok((unsigned int)out, sizeof(*out))) return -EFAULT;
    if (!vfs_dirent_at(kpath, index, name, sizeof(name), &size, &is_dir)) {
        return 0;
    }
    {
        uapi_dirent_t kout;
        k_memset(&kout, 0, sizeof(kout));
        k_memcpy(kout.d_name, name, k_strlen(name) + 1u);
        kout.d_size = size;
        kout.d_is_dir = is_dir;
        if (copy_to_user(out, &kout, sizeof(kout)) < 0) return -EFAULT;
    }
    return 1;
}

static int sys_dirlist_batch_impl(const char* path,
                                  unsigned int index,
                                  uapi_dirent_t* out,
                                  unsigned int max_count) {
    char kpath[PROCESS_FD_NAME_MAX];
    ext2_dirent_info_t* entries;
    unsigned int entries_frame;
    unsigned int entries_frames;
    unsigned int entries_bytes;
    unsigned int count = 0;
    int path_rc;
    int rc = 0;

    if (max_count == 0) return 0;
    if (max_count > DIRLIST_BATCH_MAX) max_count = DIRLIST_BATCH_MAX;
    if (!out) return -EFAULT;
    if (!user_buf_ok((unsigned int)out, sizeof(*out) * max_count)) return -EFAULT;

    path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;

    entries_bytes = sizeof(*entries) * max_count;
    entries_frames = (entries_bytes + PMM_FRAME_SIZE - 1u) / PMM_FRAME_SIZE;
    entries_frame = pmm_alloc_contiguous_frames(entries_frames);
    if (!entries_frame) return -ENOMEM;
    entries = (ext2_dirent_info_t*)paging_phys_to_kernel_virt(entries_frame);

    if (!vfs_dirents_read(kpath, index, entries, max_count, &count)) {
        pmm_free_contiguous_frames(entries_frame, entries_frames);
        return 0;
    }

    for (unsigned int i = 0; i < count; i++) {
        uapi_dirent_t kout;
        k_memset(&kout, 0, sizeof(kout));
        k_memcpy(kout.d_name, entries[i].name, sizeof(kout.d_name));
        kout.d_size = entries[i].size;
        kout.d_is_dir = entries[i].is_dir;
        if (copy_to_user(&out[i], &kout, sizeof(kout)) < 0) {
            rc = -EFAULT;
            break;
        }
    }

    pmm_free_contiguous_frames(entries_frame, entries_frames);
    return rc < 0 ? rc : (int)count;
}

static int sys_setsockopt_impl(int fd, int level, int optname) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    (void)level;
    (void)optname;
    return 0;
}

static int sys_getsockname_impl(int fd, struct sockaddr* addr, unsigned int* addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (!addr || !addrlen) return -EFAULT;
    {
        unsigned int user_addrlen = 0;
        if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
        if (user_addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    }
    if (!user_buf_ok((unsigned int)addr, sizeof(struct sockaddr_in))) return -EFAULT;

    k_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = swap_u16((unsigned short)socket_local_port(ent->socket));
    sa.sin_addr.s_addr = swap_u32(socket_local_ip(ent->socket));
    if (copy_to_user(addr, &sa, sizeof(sa)) < 0 ||
        write_user_u32(addrlen, sizeof(sa)) < 0) {
        return -EFAULT;
    }
    return 0;
}

static int sys_writefd_impl(int fd, const char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    return process_fd_write(ent, buf, len);
}

static int sys_lseek_impl(int fd, int offset, int whence) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    return process_fd_seek(ent, offset, whence);
}

static int sys_fsync_impl(int fd) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (!ent->writable) return 0;
    return process_fd_flush(ent) ? 0 : -EIO;
}

static int sys_unlink_impl(const char* path) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    if (vfs_is_dir(kpath)) return -EISDIR;
    return vfs_unlink(kpath) ? 0 : path_lookup_errno(kpath);
}

static int sys_rename_impl(const char* src, const char* dst) {
    char ksrc[PROCESS_FD_NAME_MAX];
    char kdst[PROCESS_FD_NAME_MAX];
    int src_rc = copy_user_path_resolved(ksrc, sizeof(ksrc), src);
    if (src_rc < 0) return src_rc;
    int dst_rc = copy_user_path_resolved(kdst, sizeof(kdst), dst);
    if (dst_rc < 0) return dst_rc;
    return vfs_rename(ksrc, kdst) ? 0 : path_lookup_errno(ksrc);
}

static int sys_stat_impl(const char* path, unsigned int* out_size, int* out_is_dir) {
    char kpath[PROCESS_FD_NAME_MAX];
    u32 size = 0;
    int is_dir = 0;
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;

    if (!vfs_stat(kpath, &size, &is_dir)) return path_lookup_errno(kpath);

    if (out_size) {
        if (write_user_u32(out_size, size) < 0) return -EFAULT;
    }
    if (out_is_dir) {
        if (copy_to_user(out_is_dir, &is_dir, sizeof(is_dir)) < 0) return -EFAULT;
    }
    return 0;
}

static int sys_fstat_impl(int fd, unsigned int* out_size, int* out_is_dir) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    u32 size = 0;
    int is_dir = 0;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;

    if (ent->kind == PROCESS_HANDLE_KIND_FILE) {
        int rc = vfs_file_stat_fd(ent, &size, &is_dir);
        if (rc < 0) return rc;
    } else {
        size = ent->is_dir ? 0u : ent->size;
        is_dir = ent->is_dir ? 1 : 0;
    }

    if (out_size) {
        if (write_user_u32(out_size, size) < 0) return -EFAULT;
    }
    if (out_is_dir) {
        if (copy_to_user(out_is_dir, &is_dir, sizeof(is_dir)) < 0) return -EFAULT;
    }
    return 0;
}

static int sys_stat_full_impl(const char* path, sys_stat_info_t* out) {
    char kpath[PROCESS_FD_NAME_MAX];
    sys_stat_info_t info;
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);

    if (path_rc < 0) return path_rc;
    if (!out) return -EFAULT;
    if (!vfs_stat_info(kpath, &info)) return path_lookup_errno(kpath);
    if (copy_to_user(out, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_fstat_full_impl(int fd, sys_stat_info_t* out) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    sys_stat_info_t info;

    if (!proc) return -EINVAL;
    if (!out) return -EFAULT;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;

    if (ent->kind == PROCESS_HANDLE_KIND_FILE) {
        int rc = vfs_file_stat_info_fd(ent, &info);
        if (rc < 0) return rc;
    } else {
        k_memset(&info, 0, sizeof(info));
        info.mode = 0020000u | 0600u;
        info.nlink = 1u;
        info.blksize = 4096u;
        info.size = ent->size;
    }

    if (copy_to_user(out, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_terminal_size_impl(unsigned int* out_rows, unsigned int* out_cols) {
    process_t* proc;
    fd_entry_t* stdin_ent;
    unsigned int rows = 0;
    unsigned int cols = 0;

    if (!out_rows || !out_cols) return -EFAULT;
    proc = (process_t*)sched_current();
    stdin_ent = proc ? process_fd_get(proc, 0) : 0;
    if (process_fd_terminal_size(stdin_ent, &rows, &cols) < 0) {
        rows = (unsigned int)terminal_rows();
        cols = (unsigned int)terminal_cols();
    }
    if (write_user_u32(out_rows, rows) < 0) return -EFAULT;
    if (write_user_u32(out_cols, cols) < 0) return -EFAULT;
    return 0;
}

static int sys_display_info_impl(sys_display_info_t* out_info) {
    display_info_t info;

    if (!out_info) return -EFAULT;
    if (!user_buf_ok((unsigned int)out_info, sizeof(*out_info))) return -EFAULT;
    if (!display_get_info(&info)) return -EIO;

    {
        sys_display_info_t user_info;
        user_info.width = info.width;
        user_info.height = info.height;
        user_info.pitch = info.pitch;
        user_info.bpp = info.bpp;
        user_info.format = info.format;
        if (copy_to_user(out_info, &user_info, sizeof(user_info)) < 0) return -EFAULT;
    }
    return 0;
}

static int sys_display_acquire_impl(void) {
    process_t* proc = (process_t*)sched_current();
    return display_acquire(proc) ? 0 : -EIO;
}

static int sys_display_release_impl(void) {
    process_t* proc = (process_t*)sched_current();
    display_release(proc);
    return 0;
}

static int sys_display_fill_impl(const sys_display_fill_rect_t* user_req) {
    sys_display_fill_rect_t req;

    if (!user_req) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;
    if (req.w == 0 || req.h == 0) return 0;
    if (!display_fill((process_t*)sched_current(), req.x, req.y, req.w, req.h, req.color)) return -EIO;
    return 0;
}

static int sys_display_blit_impl(const sys_display_blit_rect_t* user_req) {
    sys_display_blit_rect_t req;
    unsigned int bytes;

    if (!user_req) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;
    if (req.w == 0 || req.h == 0) return 0;
    if (!req.pixels) return -EFAULT;
    if (req.h > 0xFFFFFFFFu / req.w) return -EOVERFLOW;
    if (req.w * req.h > 0xFFFFFFFFu / sizeof(unsigned int)) return -EOVERFLOW;
    bytes = req.w * req.h * sizeof(unsigned int);
    if (!user_buf_ok((unsigned int)req.pixels, bytes)) return -EFAULT;
    if (!display_blit((process_t*)sched_current(), req.x, req.y, req.w, req.h, req.pixels)) return -EIO;
    return 0;
}

static int sys_mouse_read_impl(sys_mouse_state_t* out_state) {
    sys_mouse_state_t state;

    if (!out_state) return -EFAULT;
    if (!mouse_read_state(&state)) return -EIO;
    if (copy_to_user(out_state, &state, sizeof(state)) < 0) return -EFAULT;
    return 0;
}

static int sys_mouse_debug_impl(sys_mousedebug_t* out_info) {
    mouse_debug_state_t debug;
    sys_mousedebug_t info;

    if (!out_info) return -EFAULT;
    mouse_debug_snapshot(&debug);
    info.irq_count = debug.irq_count;
    info.byte_count = debug.byte_count;
    info.aux_status_count = debug.aux_status_count;
    info.packet_count = debug.packet_count;
    info.vmware_packet_count = debug.vmware_packet_count;
    info.sync_drop_count = debug.sync_drop_count;
    info.overflow_drop_count = debug.overflow_drop_count;
    info.vmware_enabled = debug.vmware_enabled;
    info.packet_size = debug.packet_size;
    info.device_id = debug.device_id;
    info.ready = debug.ready;
    info.init_step = debug.init_step;
    info.init_fail = debug.init_fail;
    info.config_before = debug.config_before;
    info.config_after = debug.config_after;
    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_usbinfo_impl(sys_usbinfo_t* out_info) {
    usb_debug_state_t debug;
    sys_usbinfo_t info;

    if (!out_info) return -EFAULT;
    usb_debug_snapshot(&debug);
    info.controller_count = debug.controller_count;
    info.uhci_count = debug.uhci_count;
    info.ohci_count = debug.ohci_count;
    info.ehci_count = debug.ehci_count;
    info.xhci_count = debug.xhci_count;
    info.powered_port_count = debug.powered_port_count;
    info.keyboard_active = debug.keyboard_active;
    info.keyboard_port = debug.keyboard_port;
    info.keyboard_endpoint = debug.keyboard_endpoint;
    info.keyboard_packet_size = debug.keyboard_packet_size;
    info.keyboard_interval = debug.keyboard_interval;
    info.keyboard_poll_count = debug.keyboard_poll_count;
    info.keyboard_report_count = debug.keyboard_report_count;
    info.keyboard_fail_count = debug.keyboard_fail_count;
    info.keyboard_last_cc = debug.keyboard_last_cc;
    info.mouse_active = debug.mouse_active;
    info.mouse_port = debug.mouse_port;
    info.mouse_endpoint = debug.mouse_endpoint;
    info.mouse_packet_size = debug.mouse_packet_size;
    info.mouse_interval = debug.mouse_interval;
    info.mouse_poll_count = debug.mouse_poll_count;
    info.mouse_report_count = debug.mouse_report_count;
    info.mouse_fail_count = debug.mouse_fail_count;
    info.mouse_last_cc = debug.mouse_last_cc;
    info.service_active = debug.service_active;
    info.storage_active = debug.storage_active;
    info.storage_port = debug.storage_port;
    info.last_bar = debug.last_bar;
    info.last_ports = debug.last_ports;
    info.last_port_status0 = debug.last_port_status0;
    info.last_port_status1 = debug.last_port_status1;
    info.last_bus = debug.last_bus;
    info.last_slot = debug.last_slot;
    info.last_func = debug.last_func;
    info.last_prog_if = debug.last_prog_if;
    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_usb_diag_op_impl(unsigned int op, unsigned int arg) {
    switch (op) {
        case SYS_USB_DIAG_OP_PORT_SNAPSHOT: {
            sys_usb_port_snapshot_t snapshot;
            if (!arg) return -EFAULT;
            usb_port_snapshot(&snapshot);
            if (copy_to_user((sys_usb_port_snapshot_t*)arg,
                             &snapshot,
                             sizeof(snapshot)) < 0) {
                return -EFAULT;
            }
            return 0;
        }
        case SYS_USB_DIAG_OP_PORTS:
            usb_dump_ports();
            return 0;
        case SYS_USB_DIAG_OP_DIAG:
            usb_diag();
            return 0;
        case SYS_USB_DIAG_OP_PEEK:
            usb_peek_port(arg);
            return 0;
        case SYS_USB_DIAG_OP_POWER:
            return (int)usb_power_ohci_ports();
        default:
            return -EINVAL;
    }
}

static int sys_usb_mouse_op_impl(unsigned int op, unsigned int port) {
    switch (op) {
        case SYS_USB_MOUSE_OP_OPEN:
            return usb_mouse_open_port_quiet(port) ? 1 : 0;
        case SYS_USB_MOUSE_OP_POLL:
            return usb_mouse_poll_once();
        case SYS_USB_MOUSE_OP_CLOSE:
            usb_mouse_close();
            return 0;
        default:
            return -EINVAL;
    }
}

static int sys_input_read_impl(syscall_regs_t* regs,
                               sys_input_event_t* out_events,
                               unsigned int max_events,
                               unsigned int flags) {
    unsigned int bytes;
    unsigned int copied = 0;
    process_t* proc;

    (void)regs;

    if ((flags & ~SYS_INPUT_FLAG_NONBLOCK) != 0u) return -EINVAL;
    if (max_events == 0u) return 0;
    if (max_events > INPUT_READ_MAX_EVENTS) return -EINVAL;
    if (!user_count_bytes_ok((unsigned int)out_events,
                             max_events,
                             sizeof(sys_input_event_t),
                             &bytes)) {
        return -EFAULT;
    }

    proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    while (1) {
        __asm__ volatile ("cli");
        if (input_available()) {
            break;
        }
        if (flags & SYS_INPUT_FLAG_NONBLOCK) {
            return 0;
        }
        proc->state = PROCESS_STATE_WAITING;
        input_set_waiting_process(proc);
        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
    }

    while (copied < max_events) {
        sys_input_event_t ev;
        if (!input_pop_event(&ev)) {
            break;
        }
        if (copy_to_user(&out_events[copied], &ev, sizeof(ev)) < 0) {
            return -EFAULT;
        }
        copied++;
    }

    return (int)copied;
}

static int sys_fsinfo_impl(sys_fsinfo_t* out_info) {
    ext2_fsinfo_t info;
    sys_fsinfo_t user_info;

    if (!out_info) return -EFAULT;
    if (!ext2_fsinfo(&info)) return -EIO;

    user_info.total_bytes = info.total_bytes;
    user_info.used_bytes = info.used_bytes;
    user_info.free_bytes = info.free_bytes;
    user_info.cluster_bytes = info.cluster_bytes;
    user_info.total_clusters = info.total_clusters;
    user_info.free_clusters = info.free_clusters;
    if (copy_to_user(out_info, &user_info, sizeof(user_info)) < 0) return -EFAULT;
    return 0;
}

static int sys_fsmap_impl(sys_fsmap_request_t* user_req) {
    sys_fsmap_request_t req;
    u32 out_clusters = 0;

    if (!user_req) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;

    if (req.max_clusters == 0) {
        req.out_clusters = 0;
        if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
        return 0;
    }
    if (!req.states) return -EFAULT;
    if (!user_buf_ok((unsigned int)req.states, req.max_clusters)) return -EFAULT;
    if (!ext2_fsmap(req.start_cluster, req.max_clusters, req.states, &out_clusters)) return -EIO;

    req.out_clusters = out_clusters;
    if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
    return 0;
}

static int sys_meminfo_impl(sys_meminfo_t* out_info) {
    sys_meminfo_t info;

    if (!out_info) return -EFAULT;

    info.heap_base = memory_get_heap_base();
    info.heap_top = memory_get_heap_top();
    info.pmm_free_frames = pmm_free_count();
    info.pmm_total_frames = pmm_total_count();
    info.e820_valid = boot_info_e820_valid() ? 1u : 0u;
    info.e820_count = info.e820_valid ? boot_info_e820_count() : 0u;

    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static unsigned int process_ram_bytes(process_t* proc) {
    unsigned int frames = 0;

    if (!proc) return 0;

    frames += 1u; /* process_t */
    frames += proc->kernel_stack_frames;
    frames += proc->fd_table_frames;
    frames += process_pd_count_private_frames(proc->pd);

    return frames * PAGE_SIZE;
}

static int sys_procinfo_impl(sys_procinfo_t* out_info) {
    process_t* procs[SYS_PROCINFO_MAX];
    sys_procinfo_t info;
    int count;
    unsigned int total_ticks = timer_get_ticks();

    if (!out_info) return -EFAULT;

    k_memset(&info, 0, sizeof(info));
    count = sched_snapshot_all(procs, SYS_PROCINFO_MAX);
    info.total_count = (unsigned int)count;
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

static int sys_e820_entry_impl(unsigned int index, sys_e820_entry_t* out_entry) {
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

static int sys_netinfo_impl(sys_netinfo_t* out_info) {
    sys_netinfo_t info;
    const u8* mac;
    const net_ipv4_config_t* cfg;
    nic_stats_t nic_stats;
    socket_stats_t socket_stats;
    tcp_stats_t tcp_stats;

    if (!out_info) return -EFAULT;
    k_memset(&info, 0, sizeof(info));

    info.net_link_up = nic_link_up() ? 1u : 0u;
    k_strncpy(info.net_driver, nic_driver_name(), sizeof(info.net_driver));
    mac = nic_mac();
    if (mac) {
        for (unsigned int i = 0; i < 6u; i++) info.mac[i] = mac[i];
    }
    nic_get_stats(&nic_stats);
    info.nic_tx_packets = nic_stats.tx_packets;
    info.nic_rx_packets = nic_stats.rx_packets;
    info.nic_tx_errors = nic_stats.tx_errors;
    info.nic_rx_errors = nic_stats.rx_errors;
    info.nic_status = nic_stats.status;
    info.nic_command = nic_stats.command;
    info.nic_rx_config = nic_stats.rx_config;
    info.nic_tx_config = nic_stats.tx_config;
    info.nic_rx_cursor = nic_stats.rx_cursor;
    info.nic_rx_hw_cursor = nic_stats.rx_hw_cursor;

    cfg = net_ipv4_config();
    if (cfg) {
        info.ipv4_configured = cfg->configured ? 1u : 0u;
        info.ip = cfg->ip;
        info.netmask = cfg->netmask;
        info.gateway = cfg->gateway;
        info.dns = cfg->dns;
        info.dhcp_server = cfg->dhcp_server;
        info.lease_seconds = cfg->lease_seconds;
    }

    socket_get_stats(&socket_stats);
    info.max_sockets = socket_stats.max_sockets;
    info.used_sockets = socket_stats.used_sockets;
    info.tcp_sockets = socket_stats.tcp_sockets;
    info.open_sockets = socket_stats.open_sockets;
    info.bound_sockets = socket_stats.bound_sockets;
    info.listening_sockets = socket_stats.listening_sockets;
    info.connected_sockets = socket_stats.connected_sockets;

    tcp_get_stats(&tcp_stats);
    info.tcp_listeners = tcp_stats.listeners;
    info.tcp_max_listeners = tcp_stats.max_listeners;
    info.tcp_connections = tcp_stats.connections;
    info.tcp_max_connections = tcp_stats.max_connections;
    info.tcp_established_connections = tcp_stats.established_connections;
    info.tcp_accepted_connections = tcp_stats.accepted_connections;
    info.tcp_pending_connections = tcp_stats.pending_connections;
    info.tcp_syn_recv_connections = tcp_stats.syn_recv_connections;
    info.tcp_fin_wait_connections = tcp_stats.fin_wait_connections;
    info.tcp_rx_rings = tcp_stats.rx_rings;
    info.tcp_tx_rings = tcp_stats.tx_rings;
    info.tcp_rx_bytes = tcp_stats.rx_bytes;
    info.tcp_tx_bytes = tcp_stats.tx_bytes;
    info.tcp_rx_buffer_bytes = tcp_stats.rx_buffer_bytes;
    info.tcp_tx_buffer_bytes = tcp_stats.tx_buffer_bytes;
    info.tcp_max_rx_buffer_bytes = tcp_stats.max_rx_buffer_bytes;
    info.tcp_max_tx_buffer_bytes = tcp_stats.max_tx_buffer_bytes;

    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_net_route_for_target(u32 target_ip, u32* out_sender_ip, u32* out_next_hop) {
    u32 sender_ip = net_ipv4_local_ip();
    u32 netmask = net_ipv4_netmask();
    u32 gateway = net_ipv4_gateway();
    u32 next_hop = target_ip;

    if (!net_ipv4_is_configured() || sender_ip == 0u) return -ENETUNREACH;
    if (netmask != 0u && (target_ip & netmask) != (sender_ip & netmask)) {
        if (gateway == 0u) return -ENETUNREACH;
        next_hop = gateway;
    }

    if (out_sender_ip) *out_sender_ip = sender_ip;
    if (out_next_hop) *out_next_hop = next_hop;
    return 0;
}

static int sys_net_op_impl(sys_net_op_request_t* user_req) {
    sys_net_op_request_t req;
    int rc;

    if (!user_req) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;

    switch (req.op) {
    case SYS_NET_OP_SEND_TEST_FRAME:
        return nic_send_test_frame() ? 1 : -EIO;
    case SYS_NET_OP_POLL_ONCE:
        return net_poll_once() ? 1 : 0;
    case SYS_NET_OP_DHCP:
        return dhcp_configure() ? 1 : 0;
    case SYS_NET_OP_CONFIGURE:
        if (req.target_ip == 0u) return -EINVAL;
        net_ipv4_configure(req.target_ip, req.netmask, req.gateway, req.dns,
                           req.dhcp_server, req.lease_seconds);
        return 1;
    case SYS_NET_OP_CLEAR_CONFIG:
        net_ipv4_clear_config();
        return 1;
    case SYS_NET_OP_ARP:
        if (req.target_ip == 0u) req.target_ip = net_ipv4_gateway();
        rc = sys_net_route_for_target(req.target_ip, &req.sender_ip, &req.next_hop_ip);
        if (rc < 0) return rc;
        if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
        if (!arp_resolve(req.sender_ip, req.next_hop_ip, req.mac)) return 0;
        if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
        return 1;
    case SYS_NET_OP_PING:
        if (req.target_ip == 0u) req.target_ip = net_ipv4_gateway();
        rc = sys_net_route_for_target(req.target_ip, &req.sender_ip, &req.next_hop_ip);
        if (rc < 0) return rc;
        if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
        return ipv4_ping_via_gateway(req.sender_ip, req.target_ip, req.next_hop_ip) ? 1 : 0;
    default:
        return -EINVAL;
    }
}

static int sys_block_read_sector_impl(unsigned int lba, void* user_buf) {
    block_device_t* dev = ext2_block_device();
    int rc = 0;

    if (!user_buf) return -EFAULT;
    if (!user_buf_ok((unsigned int)user_buf, sizeof(s_sys_block_sector))) return -EFAULT;
    if (!dev || dev->sector_size != sizeof(s_sys_block_sector)) return -EIO;

    while (!__sync_bool_compare_and_swap(&s_sys_block_sector_locked, 0, 1)) {
        __asm__ __volatile__("" : : : "memory");
    }

    if (!block_read(dev, lba, 1, s_sys_block_sector)) {
        rc = -EIO;
        goto out;
    }
    if (copy_to_user(user_buf, s_sys_block_sector, sizeof(s_sys_block_sector)) < 0) {
        rc = -EFAULT;
        goto out;
    }

out:
    __sync_lock_release(&s_sys_block_sector_locked);
    return rc;
}

static int sys_getcwd_impl(char* buf, unsigned int size) {
    process_t* proc = (process_t*)sched_current();
    unsigned int pos = 0;

    if (!proc) return -EINVAL;
    if (!buf || size == 0) return -EFAULT;
    if (!user_buf_ok((unsigned int)buf, size)) return -EFAULT;

    if (size < 2) return -EINVAL;
    buf[pos++] = '/';
    for (unsigned int i = 0; proc->cwd[i] != '\0'; i++) {
        if (pos + 1 >= size) return -EINVAL;
        buf[pos++] = proc->cwd[i];
    }
    buf[pos] = '\0';
    return 0;
}

static int sys_chdir_impl(const char* path) {
    char kpath[PROCESS_CWD_MAX];
    process_t* proc = (process_t*)sched_current();

    if (!proc) return -EINVAL;
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    if (!vfs_is_dir(kpath)) {
        u32 size = 0;
        int is_dir = 0;
        return vfs_stat(kpath, &size, &is_dir) ? -ENOTDIR : path_lookup_errno(kpath);
    }

    k_memcpy(proc->cwd, kpath, (k_size_t)k_strlen(kpath) + 1u);
    return 0;
}

static int sys_fread_impl(int fd, char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    return process_fd_read(ent, buf, len);
}

void syscall_handler_main(syscall_regs_t* regs) {
    if (regs == 0) return;

    switch (regs->eax) {
        case SYS_WRITE:
            regs->eax = (unsigned int)sys_write_impl(
                            (const char*)regs->ebx, regs->ecx);
            break;

        case SYS_EXIT:
            sys_exit_impl(regs);
            break;

        case SYS_GET_TICKS:
            regs->eax = sys_get_ticks_impl();
            break;

        case SYS_PUTC:
            regs->eax = (unsigned int)sys_putc_impl(regs->ebx);
            break;

        case SYS_READ:
            regs->eax = (unsigned int)sys_read_impl(
                            (char*)regs->ebx,
                            regs->ecx);
            break;

        case SYS_READ_RAW:
            regs->eax = (unsigned int)sys_read_raw_impl(
                            (char*)regs->ebx,
                            regs->ecx);
            break;

        case SYS_YIELD:
            regs->eax = (unsigned int)sys_yield_impl();
            break;

        case SYS_SLEEP:
            regs->eax = (unsigned int)sys_sleep_impl(regs, regs->ebx);
            break;

        case SYS_WRITEFILE:
            regs->eax = (unsigned int)sys_writefile_impl(
                            (const char*)regs->ebx,
                            (const void*)regs->ecx,
                            regs->edx);
            break;

        case SYS_WRITEFILE_PATH:
            regs->eax = (unsigned int)sys_writefile_path_impl(
                            (const char*)regs->ebx,
                            (const void*)regs->ecx,
                            regs->edx);
            break;

        case SYS_BRK:
            regs->eax = sys_brk_impl(regs->ebx);
            break;

        case SYS_HALT:
            system_halt();
            regs->eax = 0;
            break;

        case SYS_REBOOT:
            system_reboot();
            regs->eax = 0;
            break;

        case SYS_EXEC:
            regs->eax = (unsigned int)sys_exec_impl(
                            (const char*)regs->ebx,
                            (int)regs->ecx,
                            (char**)regs->edx);
            break;

        case SYS_EXEC_FG:
            regs->eax = (unsigned int)sys_exec_fg_impl(
                            (const char*)regs->ebx,
                            (int)regs->ecx,
                            (char**)regs->edx);
            break;

        case SYS_GETPID:
            regs->eax = (unsigned int)sys_getpid_impl();
            break;

        case SYS_WAITPID:
            regs->eax = (unsigned int)sys_waitpid_impl(
                            (int)regs->ebx,
                            (int*)regs->ecx,
                            (int)regs->edx);
            break;

        case SYS_WAITPID_FG:
            regs->eax = (unsigned int)sys_waitpid_fg_impl(
                            (int)regs->ebx,
                            (int*)regs->ecx);
            break;

        case SYS_KILL:
            regs->eax = (unsigned int)sys_kill_impl(
                            regs,
                            (int)regs->ebx,
                            (int)regs->ecx);
            break;

        case SYS_OPEN:
            regs->eax = (unsigned int)sys_open_impl(
                            (const char*)regs->ebx);
            break;

        case SYS_OPEN_WRITE:
            regs->eax = (unsigned int)sys_open_write_impl(
                            (const char*)regs->ebx);
            break;

        case SYS_OPEN_MODE:
            regs->eax = (unsigned int)sys_open_mode_impl(
                            (const char*)regs->ebx,
                            (unsigned int)regs->ecx);
            break;

        case SYS_CLOSE:
        {
            int fd = (int)regs->ebx;
            process_t* proc = (process_t*)sched_current();
            fd_entry_t* ent = process_fd_get(proc, fd);
            if (ent && ent->writable) {
                if (!process_fd_flush(ent)) {
                    regs->eax = (unsigned int)-EIO;
                    break;
                }
            }
            regs->eax = (unsigned int)sys_close_impl(fd);
            break;
        }

        case SYS_FREAD:
            regs->eax = (unsigned int)sys_fread_impl(
                            (int)regs->ebx,
                            (char*)regs->ecx,
                            regs->edx);
            break;

        case SYS_WRITEFD:
            regs->eax = (unsigned int)sys_writefd_impl(
                            (int)regs->ebx,
                            (const char*)regs->ecx,
                            regs->edx);
            break;

        case SYS_LSEEK:
            regs->eax = (unsigned int)sys_lseek_impl(
                            (int)regs->ebx,
                            (int)regs->ecx,
                            (int)regs->edx);
            break;

        case SYS_UNLINK:
            regs->eax = (unsigned int)sys_unlink_impl((const char*)regs->ebx);
            break;

        case SYS_RENAME:
            regs->eax = (unsigned int)sys_rename_impl((const char*)regs->ebx,
                                                      (const char*)regs->ecx);
            break;

        case SYS_STAT:
            regs->eax = (unsigned int)sys_stat_impl((const char*)regs->ebx,
                                                    (unsigned int*)regs->ecx,
                                                    (int*)regs->edx);
            break;

        case SYS_SOCKET:
            regs->eax = (unsigned int)sys_socket_impl((int)regs->ebx,
                                                      (int)regs->ecx,
                                                      (int)regs->edx);
            break;

        case SYS_BIND:
            regs->eax = (unsigned int)sys_bind_impl((int)regs->ebx,
                                                    (const struct sockaddr*)regs->ecx,
                                                    regs->edx);
            break;

        case SYS_LISTEN:
            regs->eax = (unsigned int)sys_listen_impl((int)regs->ebx,
                                                      (int)regs->ecx);
            break;

        case SYS_ACCEPT:
        {
            regs->eax = (unsigned int)sys_accept_impl(regs,
                                                      (int)regs->ebx,
                                                      (struct sockaddr*)regs->ecx,
                                                      (unsigned int*)regs->edx,
                                                      0u);
            break;
        }

        case SYS_CONNECT:
            regs->eax = (unsigned int)sys_connect_impl((int)regs->ebx,
                                                       (const struct sockaddr*)regs->ecx,
                                                       regs->edx);
            break;

        case SYS_SEND:
            regs->eax = (unsigned int)sys_send_impl((int)regs->ebx,
                                                    (const void*)regs->ecx,
                                                    regs->edx);
            break;

        case SYS_RECV:
            regs->eax = (unsigned int)sys_recv_impl(regs,
                                                    (int)regs->ebx,
                                                    (void*)regs->ecx,
                                                    regs->edx);
            break;

        case SYS_POLL:
            regs->eax = (unsigned int)sys_poll_impl(regs,
                                                    (struct pollfd*)regs->ebx,
                                                    regs->ecx,
                                                    (int)regs->edx);
            break;

        case SYS_MKDIR:
            regs->eax = (unsigned int)sys_mkdir_impl((const char*)regs->ebx,
                                                     regs->ecx);
            break;

        case SYS_RMDIR:
            regs->eax = (unsigned int)sys_rmdir_impl((const char*)regs->ebx);
            break;

        case SYS_DIRLIST:
            regs->eax = (unsigned int)sys_dirlist_impl((const char*)regs->ebx,
                                                       regs->ecx,
                                                       (uapi_dirent_t*)regs->edx);
            break;

        case SYS_DIRLIST_BATCH:
            regs->eax = (unsigned int)sys_dirlist_batch_impl((const char*)regs->ebx,
                                                             regs->ecx,
                                                             (uapi_dirent_t*)regs->edx,
                                                             regs->esi);
            break;

        case SYS_SETSOCKOPT:
            regs->eax = (unsigned int)sys_setsockopt_impl((int)regs->ebx,
                                                          (int)regs->ecx,
                                                          (int)regs->edx);
            break;

        case SYS_GETSOCKNAME:
            regs->eax = (unsigned int)sys_getsockname_impl((int)regs->ebx,
                                                           (struct sockaddr*)regs->ecx,
                                                           (unsigned int*)regs->edx);
            break;

        case SYS_GETCWD:
            regs->eax = (unsigned int)sys_getcwd_impl((char*)regs->ebx,
                                                      regs->ecx);
            break;

        case SYS_CHDIR:
            regs->eax = (unsigned int)sys_chdir_impl((const char*)regs->ebx);
            break;

        case SYS_FSYNC:
            regs->eax = (unsigned int)sys_fsync_impl((int)regs->ebx);
            break;

        case SYS_FCNTL:
            regs->eax = (unsigned int)sys_fcntl_impl((int)regs->ebx,
                                                     (int)regs->ecx,
                                                     regs->edx);
            break;

        case SYS_PIPE:
            regs->eax = (unsigned int)sys_pipe2_impl((int*)regs->ebx, 0);
            break;

        case SYS_PIPE2:
            regs->eax = (unsigned int)sys_pipe2_impl((int*)regs->ebx, regs->ecx);
            break;

        case SYS_PTY_OPEN:
            regs->eax = (unsigned int)sys_pty_open_impl((int*)regs->ebx, regs->ecx);
            break;

        case SYS_PTY_SET_SIZE:
            regs->eax = (unsigned int)sys_pty_set_size_impl((int)regs->ebx,
                                                            regs->ecx,
                                                            regs->edx);
            break;

        case SYS_STAT_FULL:
            regs->eax = (unsigned int)sys_stat_full_impl((const char*)regs->ebx,
                                                         (sys_stat_info_t*)regs->ecx);
            break;

        case SYS_FSTAT_FULL:
            regs->eax = (unsigned int)sys_fstat_full_impl((int)regs->ebx,
                                                          (sys_stat_info_t*)regs->ecx);
            break;

        case SYS_DUP:
        {
            process_t* proc = (process_t*)sched_current();
            regs->eax = (unsigned int)process_fd_dup(proc, (int)regs->ebx, 0, 0);
            break;
        }

        case SYS_DUP2:
        {
            process_t* proc = (process_t*)sched_current();
            regs->eax = (unsigned int)process_fd_dup2(proc,
                                                      (int)regs->ebx,
                                                      (int)regs->ecx,
                                                      0,
                                                      0);
            break;
        }

        case SYS_DUP3:
        {
            process_t* proc = (process_t*)sched_current();
            unsigned int fd_flags = 0;
            if ((regs->edx & ~SYS_FD_FLAG_CLOEXEC) != 0u) {
                regs->eax = (unsigned int)-EINVAL;
            } else {
                if (regs->edx & SYS_FD_FLAG_CLOEXEC) fd_flags = SYS_FD_FLAG_CLOEXEC;
                regs->eax = (unsigned int)process_fd_dup2(proc,
                                                          (int)regs->ebx,
                                                          (int)regs->ecx,
                                                          fd_flags,
                                                          1);
            }
            break;
        }

        case SYS_FORK:
            regs->eax = (unsigned int)sys_fork_impl(regs);
            break;

        case SYS_EXECVE:
            regs->eax = (unsigned int)sys_execve_impl(regs,
                                                      (const char*)regs->ebx,
                                                      (char**)regs->ecx,
                                                      (char**)regs->edx);
            break;

        case SYS_EPOLL_CREATE:
            regs->eax = (unsigned int)sys_epoll_create_impl((int)regs->ebx);
            break;

        case SYS_EPOLL_CTL:
            regs->eax = (unsigned int)sys_epoll_ctl_impl((int)regs->ebx,
                                                         (int)regs->ecx,
                                                         (int)regs->edx,
                                                         (struct epoll_event*)regs->esi);
            break;

        case SYS_EPOLL_WAIT:
            regs->eax = (unsigned int)sys_epoll_wait_impl(regs,
                                                          (int)regs->ebx,
                                                          (struct epoll_event*)regs->ecx,
                                                          (int)regs->edx,
                                                          (int)regs->esi);
            break;

        case SYS_TIMERFD_CREATE:
            regs->eax = (unsigned int)sys_timerfd_create_impl((int)regs->ebx,
                                                              (int)regs->ecx);
            break;

        case SYS_TIMERFD_SETTIME:
            regs->eax = (unsigned int)sys_timerfd_settime_impl(
                            (int)regs->ebx,
                            (int)regs->ecx,
                            (const struct user_itimerspec*)regs->edx,
                            (struct user_itimerspec*)regs->esi);
            break;

        case SYS_SIGNALFD:
            regs->eax = (unsigned int)sys_signalfd_impl((int)regs->ebx,
                                                        (const void*)regs->ecx,
                                                        (int)regs->edx);
            break;

        case SYS_ACCEPT4:
            regs->eax = (unsigned int)sys_accept_impl(regs,
                                                      (int)regs->ebx,
                                                      (struct sockaddr*)regs->ecx,
                                                      (unsigned int*)regs->edx,
                                                      regs->esi);
            break;

        case SYS_SHUTDOWN:
            regs->eax = (unsigned int)sys_shutdown_impl((int)regs->ebx,
                                                       (int)regs->ecx);
            break;

        case SYS_GETPEERNAME:
            regs->eax = (unsigned int)sys_getpeername_impl((int)regs->ebx,
                                                           (struct sockaddr*)regs->ecx,
                                                           (unsigned int*)regs->edx);
            break;

        case SYS_FSTAT:
            regs->eax = (unsigned int)sys_fstat_impl((int)regs->ebx,
                                                     (unsigned int*)regs->ecx,
                                                     (int*)regs->edx);
            break;

        case SYS_TERMINAL_SIZE:
            regs->eax = (unsigned int)sys_terminal_size_impl(
                            (unsigned int*)regs->ebx,
                            (unsigned int*)regs->ecx);
            break;

        case SYS_DISPLAY_INFO:
            regs->eax = (unsigned int)sys_display_info_impl(
                            (sys_display_info_t*)regs->ebx);
            break;

        case SYS_DISPLAY_FILL:
            regs->eax = (unsigned int)sys_display_fill_impl(
                            (const sys_display_fill_rect_t*)regs->ebx);
            break;

        case SYS_DISPLAY_BLIT:
            regs->eax = (unsigned int)sys_display_blit_impl(
                            (const sys_display_blit_rect_t*)regs->ebx);
            break;

        case SYS_DISPLAY_ACQUIRE:
            regs->eax = (unsigned int)sys_display_acquire_impl();
            break;

        case SYS_DISPLAY_RELEASE:
            regs->eax = (unsigned int)sys_display_release_impl();
            break;

        case SYS_MOUSE_READ:
            regs->eax = (unsigned int)sys_mouse_read_impl(
                            (sys_mouse_state_t*)regs->ebx);
            break;

        case SYS_USB_MOUSE_OP:
            regs->eax = (unsigned int)sys_usb_mouse_op_impl(
                            regs->ebx,
                            regs->ecx);
            break;

        case SYS_USBINFO:
            regs->eax = (unsigned int)sys_usbinfo_impl(
                            (sys_usbinfo_t*)regs->ebx);
            break;

        case SYS_MOUSE_DEBUG:
            regs->eax = (unsigned int)sys_mouse_debug_impl(
                            (sys_mousedebug_t*)regs->ebx);
            break;

        case SYS_USB_DIAG_OP:
            regs->eax = (unsigned int)sys_usb_diag_op_impl(
                            regs->ebx,
                            regs->ecx);
            break;

        case SYS_INPUT_READ:
            regs->eax = (unsigned int)sys_input_read_impl(
                            regs,
                            (sys_input_event_t*)regs->ebx,
                            regs->ecx,
                            regs->edx);
            break;

        case SYS_FSINFO:
            regs->eax = (unsigned int)sys_fsinfo_impl(
                            (sys_fsinfo_t*)regs->ebx);
            break;

        case SYS_FSMAP:
            regs->eax = (unsigned int)sys_fsmap_impl(
                            (sys_fsmap_request_t*)regs->ebx);
            break;

        case SYS_MEMINFO:
            regs->eax = (unsigned int)sys_meminfo_impl(
                            (sys_meminfo_t*)regs->ebx);
            break;

        case SYS_PROCINFO:
            regs->eax = (unsigned int)sys_procinfo_impl(
                            (sys_procinfo_t*)regs->ebx);
            break;

        case SYS_E820_ENTRY:
            regs->eax = (unsigned int)sys_e820_entry_impl(
                            regs->ebx,
                            (sys_e820_entry_t*)regs->ecx);
            break;

        case SYS_NETINFO:
            regs->eax = (unsigned int)sys_netinfo_impl(
                            (sys_netinfo_t*)regs->ebx);
            break;

        case SYS_NET_OP:
            regs->eax = (unsigned int)sys_net_op_impl(
                            (sys_net_op_request_t*)regs->ebx);
            break;

        case SYS_BLOCK_READ_SECTOR:
            regs->eax = (unsigned int)sys_block_read_sector_impl(
                            regs->ebx,
                            (void*)regs->ecx);
            break;

        case SYS_CLOCK_GETTIME:
            regs->eax = (unsigned int)sys_clock_gettime_impl(
                            (int)regs->ebx,
                            (struct user_timespec*)regs->ecx);
            break;

        case SYS_CLOCK_SETTIME:
            regs->eax = (unsigned int)sys_clock_settime_impl(
                            (int)regs->ebx,
                            (const struct user_timespec*)regs->ecx);
            break;

        case SYS_NTP_SYNC:
            regs->eax = (unsigned int)sys_ntp_sync_impl(
                            regs->ebx,
                            (struct user_timespec*)regs->ecx);
            break;

        default:
            regs->eax = (unsigned int)-ENOSYS;
            break;
    }
}
