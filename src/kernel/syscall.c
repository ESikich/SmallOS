#include "syscall.h"
#include "terminal.h"
#include "timer.h"
#include "paging.h"
#include "keyboard.h"
#include "scheduler.h"
#include "process.h"
#include "pmm.h"
#include "system.h"
#include "klib.h"
#include "../drivers/tcp.h"
#include "uapi_poll.h"
#include "uapi_errno.h"
#include "uapi_dirent.h"
#include "uapi_socket.h"
#include "uapi_epoll.h"
#include "../exec/elf_loader.h"
#include "vfs.h"
#include "socket.h"

#define SYSCALL_MAX_WRITE_LEN 4096u
#define EXEC_NAME_MAX         PROCESS_FD_NAME_MAX
#define EPOLL_MAX_WATCHES     64u

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
    if (len == 0) return 0;
    if (len > SYSCALL_MAX_WRITE_LEN) return -EFBIG;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    for (unsigned int i = 0; i < len; i++) {
        terminal_putc(buf[i]);
    }
    return (int)len;
}

static int sys_putc_impl(unsigned int ch) {
    terminal_putc((char)ch);
    return 1;
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
static int sys_exec_impl(const char* name, int argc, char** argv) {
    char kname[EXEC_NAME_MAX];
    char kargv_data[PROCESS_ARG_BYTES];
    char* kargv[PROCESS_MAX_ARGS + 1];
    unsigned int used = 0;

    int name_rc = copy_user_path_resolved(kname, sizeof(kname), name);
    if (name_rc < 0) return name_rc;

    if (argc < 0 || argc > PROCESS_MAX_ARGS) return -EINVAL;

    /* Validate the argv pointer array itself */
    if (argc > 0 && !user_buf_ok((unsigned int)argv,
                                  (unsigned int)argc * sizeof(char*))) {
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

    return elf_run_named(kname, argc, kargv) ? 0 : -ENOENT;
}

/*
 * sys_writefile_impl(name, buf, len)
 *
 * Create or overwrite a root-directory FAT16 file in one shot.  This is
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
 * Create or overwrite a FAT16 file at an arbitrary path.  This is the
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
    if (ent) ent->is_dir = is_dir ? 1 : 0;
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
                             SYS_OPEN_MODE_APPEND;
    int readable = (mode & SYS_OPEN_MODE_READ) != 0;
    int writable = (mode & SYS_OPEN_MODE_WRITE) != 0;
    int create = (mode & SYS_OPEN_MODE_CREATE) != 0;
    int trunc = (mode & SYS_OPEN_MODE_TRUNC) != 0;
    int append = (mode & SYS_OPEN_MODE_APPEND) != 0;
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
    ent->is_dir = (exists && is_dir) ? 1 : 0;

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

    k_memcpy(dst, src, sizeof(struct sockaddr_in));
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

    peer_ip = socket_peer_ip(new_ent->socket);
    peer_port = socket_peer_port(new_ent->socket);
    if (addr && addrlen) {
        struct sockaddr_in sa;
        if (!user_buf_ok((unsigned int)addrlen, sizeof(unsigned int))) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -EFAULT;
        }
        if (*addrlen < sizeof(struct sockaddr_in)) {
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
        sa.sin_addr.s_addr = peer_ip;
        k_memcpy(addr, &sa, sizeof(sa));
        *addrlen = sizeof(sa);
    }

    return new_fd;
}

static int sys_connect_impl(int fd, const struct sockaddr* addr, unsigned int addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    (void)addr;
    (void)addrlen;
    return -ENOSYS;
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
    if (socket_state(ent->socket) != SOCKET_STATE_CONNECTED) return -EINVAL;

    if (!socket_tcp_recv_ready(ent->socket) &&
        (ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
        return -EAGAIN;
    }

    while (!socket_tcp_recv_ready(ent->socket)) {
        int wait_rc;

        if (!socket_tcp_connection_established(ent->socket)) {
            return 0;
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

static int sys_poll_register_socket_waits(process_t* proc,
                                          struct pollfd* fds,
                                          unsigned int nfds) {
    if (!proc || !fds) return -EINVAL;

    for (unsigned int i = 0; i < nfds; i++) {
        fd_entry_t* ent = process_fd_get(proc, fds[i].fd);
        int rc;

        if (!socket_fd_is_socket(ent)) continue;

        rc = socket_wait(ent->socket, proc, fds[i].events);
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
    if (!user_buf_ok((unsigned int)fds, nfds * sizeof(struct pollfd))) return -EFAULT;

    infinite_wait = (timeout < 0);
    timeout_ticks = infinite_wait ? 0u : sys_poll_timeout_ticks(timeout);
    deadline = infinite_wait ? 0u : (timer_get_ticks() + timeout_ticks);

    for (;;) {
        unsigned int ready = sys_poll_snapshot(proc, fds, nfds);
        if (ready != 0u) {
            socket_wait_clear_process(proc);
            return (int)ready;
        }

        if (!infinite_wait && (int)(timer_get_ticks() - deadline) >= 0) {
            socket_wait_clear_process(proc);
            return 0;
        }

        proc->sleep_until = infinite_wait ? 0u : deadline;
        proc->state = infinite_wait ? PROCESS_STATE_WAITING
                                    : PROCESS_STATE_SLEEPING;
        {
            int wait_rc = sys_poll_register_socket_waits(proc, fds, nfds);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                socket_wait_clear_process(proc);
                return wait_rc;
            }
        }
        ready = sys_poll_snapshot(proc, fds, nfds);
        if (ready != 0u) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return (int)ready;
        }

        (void)regs;
        sys_wait_until_current_running(proc);

        socket_wait_clear_process(proc);
    }
}

static int sys_fcntl_impl(int fd, int cmd, unsigned int arg) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;

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
        k_memcpy(&event, user_event, sizeof(event));
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

static int epoll_timer_deadline_elapsed(unsigned int deadline) {
    return deadline != 0u && (int)(timer_get_ticks() - deadline) >= 0;
}

static unsigned int epoll_next_timer_deadline(process_t* proc,
                                              epoll_watch_t* watches) {
    unsigned int best = 0u;

    if (!proc || !watches) return 0u;

    for (unsigned int i = 0; i < EPOLL_MAX_WATCHES; i++) {
        if (!watches[i].used) continue;
        fd_entry_t* ent = process_fd_get(proc, watches[i].fd);
        if (!ent || ent->kind != PROCESS_HANDLE_KIND_TIMERFD) continue;
        if (ent->timer_deadline == 0u) continue;
        if (epoll_timer_deadline_elapsed(ent->timer_deadline)) {
            return timer_get_ticks();
        }
        if (best == 0u || (int)(ent->timer_deadline - best) < 0) {
            best = ent->timer_deadline;
        }
    }

    return best;
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

static int epoll_register_socket_waits(process_t* proc, epoll_watch_t* watches) {
    if (!proc) return -EINVAL;
    if (!watches) return 0;

    for (unsigned int i = 0; i < EPOLL_MAX_WATCHES; i++) {
        fd_entry_t* ent;
        int rc;

        if (!watches[i].used) continue;

        ent = process_fd_get(proc, watches[i].fd);
        if (!socket_fd_is_socket(ent)) continue;

        rc = socket_wait(ent->socket,
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
    if (!events ||
        !user_buf_ok((unsigned int)events,
                     (unsigned int)maxevents * sizeof(struct epoll_event))) {
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
            return (int)ready;
        }

        if (!infinite_wait && (int)(timer_get_ticks() - timeout_deadline) >= 0) {
            socket_wait_clear_process(proc);
            return 0;
        }

        unsigned int timer_deadline = watches ? epoll_next_timer_deadline(proc, watches) : 0u;
        unsigned int sleep_deadline = 0u;

        if (!infinite_wait) {
            sleep_deadline = timeout_deadline;
        }
        if (timer_deadline != 0u &&
            (sleep_deadline == 0u || (int)(timer_deadline - sleep_deadline) < 0)) {
            sleep_deadline = timer_deadline;
        }

        if (sleep_deadline != 0u) {
            proc->sleep_until = sleep_deadline;
            proc->state = PROCESS_STATE_SLEEPING;
        } else {
            proc->sleep_until = 0u;
            proc->state = PROCESS_STATE_WAITING;
        }
        {
            int wait_rc = epoll_register_socket_waits(proc, watches);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                socket_wait_clear_process(proc);
                return wait_rc;
            }
        }
        ready = watches ? epoll_snapshot(proc, watches, events,
                                         (unsigned int)maxevents)
                        : 0u;
        if (ready != 0u) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return (int)ready;
        }

        (void)regs;
        sys_wait_until_current_running(proc);

        socket_wait_clear_process(proc);
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
    }
    return fd;
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
        k_memset(old_value, 0, sizeof(*old_value));
    }

    k_memcpy(&spec, new_value, sizeof(spec));
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
    (void)process_fd_set_flags(ent, flags);
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
    if (!user_buf_ok((unsigned int)addrlen, sizeof(unsigned int))) return -EFAULT;
    if (*addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    if (!user_buf_ok((unsigned int)addr, sizeof(struct sockaddr_in))) return -EFAULT;

    k_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = swap_u16((unsigned short)socket_peer_port(ent->socket));
    sa.sin_addr.s_addr = socket_peer_ip(ent->socket);
    k_memcpy(addr, &sa, sizeof(sa));
    *addrlen = sizeof(sa);
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
    k_memset(out, 0, sizeof(*out));
    k_memcpy(out->d_name, name, k_strlen(name) + 1u);
    out->d_size = size;
    out->d_is_dir = is_dir;
    return 1;
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
    if (!user_buf_ok((unsigned int)addrlen, sizeof(unsigned int))) return -EFAULT;
    if (*addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    if (!user_buf_ok((unsigned int)addr, sizeof(struct sockaddr_in))) return -EFAULT;

    k_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = swap_u16((unsigned short)socket_local_port(ent->socket));
    sa.sin_addr.s_addr = 0x0100007Fu;
    k_memcpy(addr, &sa, sizeof(sa));
    *addrlen = sizeof(sa);
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
        if (!user_buf_ok((unsigned int)out_size, sizeof(unsigned int))) return -EFAULT;
        *out_size = size;
    }
    if (out_is_dir) {
        if (!user_buf_ok((unsigned int)out_is_dir, sizeof(int))) return -EFAULT;
        *out_is_dir = is_dir;
    }
    return 0;
}

static int sys_fstat_impl(int fd, unsigned int* out_size, int* out_is_dir) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;

    if (out_size) {
        if (!user_buf_ok((unsigned int)out_size, sizeof(unsigned int))) return -EFAULT;
        *out_size = ent->is_dir ? 0u : ent->size;
    }
    if (out_is_dir) {
        if (!user_buf_ok((unsigned int)out_is_dir, sizeof(int))) return -EFAULT;
        *out_is_dir = ent->is_dir ? 1 : 0;
    }
    return 0;
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

        default:
            regs->eax = (unsigned int)-ENOSYS;
            break;
    }
}
