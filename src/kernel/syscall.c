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
#include "uapi_dirent.h"
#include "uapi_socket.h"
#include "../exec/elf_loader.h"
#include "fat16.h"

#define SYSCALL_ERR_INVALID   ((unsigned int)-1)
#define SYSCALL_MAX_WRITE_LEN 4096u
#define EXEC_NAME_MAX         PROCESS_FD_NAME_MAX
#define SCHED_RESUME_RETADDR_OFFSET 8u  /* push esp + call return address */

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
    u32 pde = pd[addr >> 22];
    if (!(pde & PAGE_PRESENT)) return 0;
    if (!(pde & PAGE_USER))    return 0;

    u32* pt = (u32*)(pde & ~0xFFFu);
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
    if (!dst || !src || dst_size == 0) return -1;

    unsigned int ptr = (unsigned int)src;
    if (ptr < USER_CODE_BASE || ptr >= USER_STACK_TOP) return -1;

    u32* pd = current_user_pd();
    if (!pd) return -1;

    for (unsigned int i = 0; i < dst_size; i++) {
        unsigned int addr = ptr + i;
        if (addr < USER_CODE_BASE || addr >= USER_STACK_TOP) return -1;
        if (!user_page_mapped(pd, addr)) return -1;

        dst[i] = src[i];
        if (dst[i] == '\0') {
            return (int)(i + 1);
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* Syscall implementations                                            */
/* ------------------------------------------------------------------ */

static int sys_write_impl(const char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (len > SYSCALL_MAX_WRITE_LEN) return -1;
    if (!user_buf_ok((unsigned int)buf, len)) return -1;

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

static int sys_yield_impl(unsigned int esp) {
    sched_yield_now(esp - SCHED_RESUME_RETADDR_OFFSET);
    return 0;
}

/*
 * sys_sleep_impl(regs, ticks)
 *
 * Block the current process until at least ticks timer ticks have elapsed.
 * The task marks itself SLEEPING, stores a wake deadline, then yields to
 * the scheduler.  When the timer reaches the deadline the scheduler wakes
 * the task and this function continues.
 */
static int sys_sleep_impl(syscall_regs_t* regs, unsigned int ticks) {
    if (ticks == 0) return 0;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -1;

    proc->sleep_until = timer_get_ticks() + ticks;
    proc->state = PROCESS_STATE_SLEEPING;

    /*
     * Yield immediately so other runnable tasks can execute while this
     * one sleeps.  If there is no other runnable task, sched_yield_now()
     * simply returns and the local hlt loop keeps the CPU idle.
     */
    __asm__ volatile ("sti");
    sched_yield_now((unsigned int)regs - SCHED_RESUME_RETADDR_OFFSET);

    while (proc->state != PROCESS_STATE_RUNNING) {
        __asm__ volatile ("hlt");
    }

    __asm__ volatile ("cli");
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
    if (!user_buf_ok((unsigned int)buf, len)) return -1;

    process_t* proc = (process_t*)sched_current();

    unsigned int n = 0;

    __asm__ volatile ("sti");

    while (n < len) {
        /* Park until at least one character is available. */
        while (!keyboard_buf_available()) {
            if (proc) {
                proc->state = PROCESS_STATE_WAITING;
                keyboard_set_waiting_process(proc);
            }
            __asm__ volatile ("hlt");
            /*
             * Execution resumes here after the timer IRQ switches back to
             * this task.  proc->state has been set to RUNNING by
             * process_key_consumer() before the scheduler selected us again.
             * Re-check the buffer — if it is still empty we park again.
             */
        }

        char c = keyboard_buf_pop();
        terminal_putc(c);
        buf[n++] = c;
        if (c == '\n') break;
    }

    __asm__ volatile ("cli");
    return (int)n;
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
 * Returns 0 on success, -1 if any validation fails or the program was
 * not found.
 */
static int sys_exec_impl(const char* name, int argc, char** argv) {
    char kname[EXEC_NAME_MAX];
    char kargv_data[PROCESS_ARG_BYTES];
    char* kargv[PROCESS_MAX_ARGS + 1];
    unsigned int used = 0;

    if (copy_user_cstr(kname, sizeof(kname), name) <= 1) return -1;

    if (argc < 0 || argc > PROCESS_MAX_ARGS) return -1;

    /* Validate the argv pointer array itself */
    if (argc > 0 && !user_buf_ok((unsigned int)argv,
                                  (unsigned int)argc * sizeof(char*))) {
        return -1;
    }

    for (int i = 0; i < argc; i++) {
        int copied = copy_user_cstr(&kargv_data[used],
                                    PROCESS_ARG_BYTES - used,
                                    argv[i]);
        if (copied < 0) return -1;
        kargv[i] = &kargv_data[used];
        used += (unsigned int)copied;
    }
    kargv[argc] = 0;

    return elf_run_named(kname, argc, kargv) ? 0 : -1;
}

/*
 * sys_writefile_impl(name, buf, len)
 *
 * Create or overwrite a root-directory FAT16 file in one shot.  This is
 * the historical output primitive for user-space tools.
 */
static int sys_writefile_impl(const char* name, const void* buf, unsigned int len) {
    char kname[EXEC_NAME_MAX];
    if (copy_user_cstr(kname, sizeof(kname), name) <= 1) return -1;
    if (len > 0 && !user_buf_ok((unsigned int)buf, len)) return -1;

    return fat16_write(kname, (const u8*)buf, len) ? 0 : -1;
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
    if (copy_user_cstr(kpath, sizeof(kpath), path) <= 1) return -1;
    if (len > 0 && !user_buf_ok((unsigned int)buf, len)) return -1;

    return fat16_write_path(kpath, (const u8*)buf, len) ? 0 : -1;
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

    u32 pde = pd[pd_index];
    if (!(pde & PAGE_PRESENT)) {
        return;
    }

    u32* pt = (u32*)(pde & ~0xFFFu);
    u32 pte = pt[pt_index];
    if (!(pte & PAGE_PRESENT)) {
        return;
    }

    pmm_free_frame(pte & ~0xFFFu);
    pt[pt_index] = 0;
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");

    if (heap_page_table_empty(pt)) {
        pd[pd_index] = 0;
        pmm_free_frame((u32)pt);
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
            k_memset((void*)frame, 0, PAGE_SIZE);
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
 * Validate the filename, confirm the file exists on the FAT16 partition
 * via fat16_stat(), allocate the lowest free fd slot (>= PROCESS_FD_FIRST)
 * in the current process's fd table, and record name, size, and offset=0.
 *
 * Returns the fd (>= 3) on success, -1 on any failure.
 */
static int sys_open_impl(const char* name) {
    char kname[PROCESS_FD_NAME_MAX];
    if (copy_user_cstr(kname, sizeof(kname), name) <= 1) return -1;

    /* Check the file exists and get its size */
    u32 file_size = 0;
    if (!fat16_stat(kname, &file_size)) return -1;

    /* Allocate an fd slot in the current process */
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -1;

    return process_fd_open_file(proc, kname, file_size, 0);
}

/*
 * sys_close_impl(fd)
 *
 * Mark the fd slot as free.  Returns 0 on success, -1 on bad fd.
 */
static int sys_close_impl(int fd) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -1;
    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -1;

    process_fd_close(ent);
    return 0;
}

static int sys_open_write_impl(const char* name) {
    char kname[PROCESS_FD_NAME_MAX];
    if (copy_user_cstr(kname, sizeof(kname), name) <= 1) return -1;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -1;

    return process_fd_open_file(proc, kname, 0, 1);
}

static int copy_user_sockaddr_in(struct sockaddr_in* dst,
                                 const struct sockaddr* src,
                                 unsigned int len) {
    if (!dst || !src) {
        return -1;
    }
    if (len < sizeof(struct sockaddr_in)) {
        return -1;
    }
    if (!user_buf_ok((unsigned int)src, sizeof(struct sockaddr_in))) {
        return -1;
    }

    k_memcpy(dst, src, sizeof(struct sockaddr_in));
    if (dst->sin_family != AF_INET) {
        return -1;
    }
    return 0;
}

static int socket_fd_is_socket(fd_entry_t* ent) {
    return ent && ent->valid && ent->kind == PROCESS_HANDLE_KIND_SOCKET;
}

static unsigned short swap_u16(unsigned short value) {
    return (unsigned short)(((value & 0x00FFu) << 8) | ((value & 0xFF00u) >> 8));
}

static int sys_socket_impl(int domain, int type, int protocol) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -1;
    if (domain != AF_INET) return -1;
    if (type != SOCK_STREAM) return -1;
    if (protocol != 0 && protocol != IPPROTO_TCP) return -1;

    return process_fd_open_socket(proc, "socket");
}

static int sys_bind_impl(int fd, const struct sockaddr* addr, unsigned int addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;

    if (!proc) return -1;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -1;
    if (ent->socket_state != PROCESS_SOCKET_STATE_OPEN) return -1;
    if (copy_user_sockaddr_in(&sa, addr, addrlen) < 0) return -1;

    ent->socket_port = swap_u16(sa.sin_port);
    ent->socket_state = PROCESS_SOCKET_STATE_BOUND;
    return 0;
}

static int sys_listen_impl(int fd, int backlog) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -1;
    (void)backlog;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -1;
    if (ent->socket_state != PROCESS_SOCKET_STATE_BOUND) return -1;
    if (ent->socket_port == 0u) return -1;

    tcp_socket_use_port(ent->socket_port);
    if (tcp_socket_bind(ent->socket_port) < 0) return -1;
    if (tcp_socket_listen() < 0) return -1;
    ent->socket_state = PROCESS_SOCKET_STATE_LISTENER;
    return 0;
}

static int sys_accept_impl(syscall_regs_t* regs,
                           int fd,
                           struct sockaddr* addr,
                           unsigned int* addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    unsigned int peer_ip;
    unsigned int peer_port;
    int new_fd;

    if (!proc) return -1;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -1;
    if (ent->socket_state != PROCESS_SOCKET_STATE_LISTENER) return -1;
    tcp_socket_use_port(ent->socket_port);

    __asm__ __volatile__("sti");
    while (!tcp_socket_accept_ready()) {
        proc->state = PROCESS_STATE_WAITING;
        tcp_socket_set_waiter(proc);
        __asm__ __volatile__("hlt");
    }
    __asm__ __volatile__("cli");

    new_fd = process_fd_open_socket(proc, "socket");
    if (new_fd < 0) return -1;
    terminal_puts("sys_accept allocated fd=");
    terminal_put_uint((unsigned int)new_fd);
    terminal_putc('\n');

    {
        fd_entry_t* new_ent = process_fd_get(proc, new_fd);
        if (!socket_fd_is_socket(new_ent)) {
            process_fd_close(new_ent);
            return -1;
        }
        new_ent->socket_state = PROCESS_SOCKET_STATE_CONNECTED;
        new_ent->socket_port = ent->socket_port;
    }
    tcp_socket_mark_accepted();

    peer_ip = tcp_socket_peer_ip();
    peer_port = tcp_socket_peer_port();
    if (addr && addrlen) {
        struct sockaddr_in sa;
        if (!user_buf_ok((unsigned int)addrlen, sizeof(unsigned int))) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -1;
        }
        if (*addrlen < sizeof(struct sockaddr_in)) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -1;
        }
        if (!user_buf_ok((unsigned int)addr, sizeof(struct sockaddr_in))) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -1;
        }
        k_memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = swap_u16((unsigned short)peer_port);
        sa.sin_addr.s_addr = peer_ip;
        k_memcpy(addr, &sa, sizeof(sa));
        *addrlen = sizeof(sa);
    }

    tcp_socket_wake_waiter();
    terminal_puts("sys_accept final fd=");
    terminal_put_uint((unsigned int)new_fd);
    terminal_putc('\n');
    terminal_puts("sys_accept returning fd=");
    terminal_put_uint((unsigned int)new_fd);
    terminal_putc('\n');
    return new_fd;
}

static int sys_connect_impl(int fd, const struct sockaddr* addr, unsigned int addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -1;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -1;
    (void)addr;
    (void)addrlen;
    return -1;
}

static int sys_send_impl(int fd, const void* buf, unsigned int len) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -1;
    if (len == 0u) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -1;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -1;
    if (ent->socket_state != PROCESS_SOCKET_STATE_CONNECTED) return -1;
    tcp_socket_use_port(ent->socket_port);
    return tcp_socket_send(buf, len);
}

static int sys_recv_impl(syscall_regs_t* regs, int fd, void* buf, unsigned int len) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -1;
    if (len == 0u) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -1;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -1;
    if (ent->socket_state != PROCESS_SOCKET_STATE_CONNECTED) return -1;
    tcp_socket_use_port(ent->socket_port);

    while (!tcp_socket_recv_ready()) {
        if (!tcp_socket_connection_established()) {
            return 0;
        }
        proc->state = PROCESS_STATE_WAITING;
        tcp_socket_set_waiter(proc);
        __asm__ __volatile__("sti");
        sched_yield_now((unsigned int)regs - SCHED_RESUME_RETADDR_OFFSET);
        while (proc->state != PROCESS_STATE_RUNNING) {
            __asm__ __volatile__("hlt");
        }
        __asm__ __volatile__("cli");
    }

    return tcp_socket_recv(buf, len);
}

static short sys_poll_revents_for_fd(process_t* proc, struct pollfd* pfd) {
    fd_entry_t* ent = process_fd_get(proc, pfd->fd);
    short revents = 0;

    if (!socket_fd_is_socket(ent)) {
        return POLLERR;
    }
    tcp_socket_use_port(ent->socket_port);

    if (ent->socket_state == PROCESS_SOCKET_STATE_LISTENER) {
        if (tcp_socket_accept_ready() && (pfd->events & POLLIN)) {
            revents |= POLLIN;
        }
    } else if (ent->socket_state == PROCESS_SOCKET_STATE_BOUND) {
        /* Bound-but-not-listening sockets are not yet ready. */
    } else if (ent->socket_state == PROCESS_SOCKET_STATE_CONNECTED) {
        if ((pfd->events & POLLIN) && tcp_socket_recv_ready()) {
            revents |= POLLIN;
        }
        if ((pfd->events & POLLOUT) && tcp_socket_connection_established()) {
            revents |= POLLOUT;
        }
    }

    return revents;
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

static unsigned int sys_poll_timeout_ticks(int timeout_ms) {
    if (timeout_ms <= 0) {
        return 0u;
    }

    /* timer_init() programs 100 Hz in kernel_main(). */
    return (unsigned int)((timeout_ms + 9) / 10);
}

static int sys_poll_impl(syscall_regs_t* regs, struct pollfd* fds,
                         unsigned int nfds, int timeout) {
    process_t* proc = (process_t*)sched_current();
    unsigned int timeout_ticks;
    unsigned int deadline;
    int infinite_wait;

    if (!proc) return -1;
    if (nfds == 0u) return 0;
    if (!user_buf_ok((unsigned int)fds, nfds * sizeof(struct pollfd))) return -1;

    infinite_wait = (timeout < 0);
    timeout_ticks = infinite_wait ? 0u : sys_poll_timeout_ticks(timeout);
    deadline = infinite_wait ? 0u : (timer_get_ticks() + timeout_ticks);

    for (;;) {
        unsigned int ready = sys_poll_snapshot(proc, fds, nfds);
        if (ready != 0u) {
            tcp_socket_set_waiter(0);
            return (int)ready;
        }

        if (!infinite_wait && (int)(timer_get_ticks() - deadline) >= 0) {
            tcp_socket_set_waiter(0);
            return 0;
        }

        proc->sleep_until = infinite_wait ? 0u : deadline;
        proc->state = infinite_wait ? PROCESS_STATE_WAITING
                                    : PROCESS_STATE_SLEEPING;
        tcp_socket_set_waiter(proc);

        __asm__ __volatile__("sti");
        sched_yield_now((unsigned int)regs - SCHED_RESUME_RETADDR_OFFSET);
        while (proc->state != PROCESS_STATE_RUNNING) {
            __asm__ __volatile__("hlt");
        }
        __asm__ __volatile__("cli");

        tcp_socket_set_waiter(0);
    }
}

static int sys_mkdir_impl(const char* path, unsigned int mode) {
    char kpath[PROCESS_FD_NAME_MAX];
    (void)mode;
    if (copy_user_cstr(kpath, sizeof(kpath), path) <= 1) return -1;
    return fat16_mkdir(kpath) ? 0 : -1;
}

static int sys_rmdir_impl(const char* path) {
    char kpath[PROCESS_FD_NAME_MAX];
    if (copy_user_cstr(kpath, sizeof(kpath), path) <= 1) return -1;
    return fat16_rmdir(kpath) ? 0 : -1;
}

static int sys_dirlist_impl(const char* path, unsigned int index, uapi_dirent_t* out) {
    char kpath[PROCESS_FD_NAME_MAX];
    char name[UAPI_DIRENT_NAME_MAX];
    unsigned int size = 0;
    int is_dir = 0;
    if (copy_user_cstr(kpath, sizeof(kpath), path) <= 1) return -1;
    if (!out) return -1;
    if (!user_buf_ok((unsigned int)out, sizeof(*out))) return -1;
    if (!fat16_dirent_at(kpath, index, name, sizeof(name), &size, &is_dir)) {
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

    if (!proc) return -1;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -1;
    (void)level;
    (void)optname;
    return 0;
}

static int sys_getsockname_impl(int fd, struct sockaddr* addr, unsigned int* addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;

    if (!proc) return -1;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -1;
    if (!addr || !addrlen) return -1;
    if (!user_buf_ok((unsigned int)addrlen, sizeof(unsigned int))) return -1;
    if (*addrlen < sizeof(struct sockaddr_in)) return -1;
    if (!user_buf_ok((unsigned int)addr, sizeof(struct sockaddr_in))) return -1;

    k_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = swap_u16(ent->socket_port);
    sa.sin_addr.s_addr = 0x0100007Fu;
    k_memcpy(addr, &sa, sizeof(sa));
    *addrlen = sizeof(sa);
    return 0;
}

static int sys_writefd_impl(int fd, const char* buf, unsigned int len) {
    if (fd < PROCESS_FD_FIRST || fd >= PROCESS_FD_MAX) return -1;
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -1;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -1;

    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -1;
    if (socket_fd_is_socket(ent)) {
        tcp_socket_use_port(ent->socket_port);
        if (ent->socket_state != PROCESS_SOCKET_STATE_CONNECTED) return -1;
        return tcp_socket_send(buf, len);
    }
    if (!ent->writable) return -1;
    return process_fd_write_file(ent, buf, len);
}

static int sys_lseek_impl(int fd, int offset, int whence) {
    if (fd < PROCESS_FD_FIRST || fd >= PROCESS_FD_MAX) return -1;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -1;

    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -1;
    return process_fd_seek(ent, offset, whence);
}

static int sys_unlink_impl(const char* path) {
    char kpath[PROCESS_FD_NAME_MAX];
    if (copy_user_cstr(kpath, sizeof(kpath), path) <= 1) return -1;
    return fat16_rm(kpath) ? 0 : -1;
}

static int sys_rename_impl(const char* src, const char* dst) {
    char ksrc[PROCESS_FD_NAME_MAX];
    char kdst[PROCESS_FD_NAME_MAX];
    if (copy_user_cstr(ksrc, sizeof(ksrc), src) <= 1) return -1;
    if (copy_user_cstr(kdst, sizeof(kdst), dst) <= 1) return -1;
    return fat16_move(ksrc, kdst) ? 0 : -1;
}

static int sys_stat_impl(const char* path, unsigned int* out_size, int* out_is_dir) {
    char kpath[PROCESS_FD_NAME_MAX];
    u32 size = 0;
    if (copy_user_cstr(kpath, sizeof(kpath), path) <= 1) return -1;

    if (fat16_stat(kpath, &size)) {
        if (out_size) {
            if (!user_buf_ok((unsigned int)out_size, sizeof(unsigned int))) return -1;
            *out_size = size;
        }
        if (out_is_dir) {
            if (!user_buf_ok((unsigned int)out_is_dir, sizeof(int))) return -1;
            *out_is_dir = 0;
        }
        return 0;
    }

    if (!fat16_is_dir(kpath)) {
        return -1;
    }

    if (out_size) {
        if (!user_buf_ok((unsigned int)out_size, sizeof(unsigned int))) return -1;
        *out_size = 0;
    }
    if (out_is_dir) {
        if (!user_buf_ok((unsigned int)out_is_dir, sizeof(int))) return -1;
        *out_is_dir = 1;
    }
    return 0;
}

static int sys_fread_impl(int fd, char* buf, unsigned int len) {
    if (fd < PROCESS_FD_FIRST || fd >= PROCESS_FD_MAX) return -1;
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -1;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -1;
    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -1;
    terminal_puts("sys_fread fd=");
    terminal_put_uint((unsigned int)fd);
    terminal_puts(" kind=");
    terminal_put_uint((unsigned int)ent->kind);
    terminal_puts(" state=");
    terminal_put_uint((unsigned int)ent->socket_state);
    terminal_putc('\n');
    if (socket_fd_is_socket(ent)) {
        tcp_socket_use_port(ent->socket_port);
        if (ent->socket_state != PROCESS_SOCKET_STATE_CONNECTED) return -1;
        terminal_puts("sys_fread socket wait\n");
        __asm__ __volatile__("sti");
        while (!tcp_socket_recv_ready()) {
            if (!tcp_socket_connection_established()) {
                terminal_puts("sys_fread socket closed\n");
                __asm__ __volatile__("cli");
                return 0;
            }
            __asm__ __volatile__("hlt");
        }
        terminal_puts("sys_fread socket ready\n");
        __asm__ __volatile__("cli");
        {
            int n = tcp_socket_recv(buf, len);
            terminal_puts("sys_fread socket read=");
            terminal_put_uint((unsigned int)(n < 0 ? 0 : n));
            terminal_putc('\n');
            return n;
        }
    }
    return process_fd_read_file(ent, buf, len);
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

        case SYS_YIELD:
            regs->eax = (unsigned int)sys_yield_impl((unsigned int)regs);
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

        case SYS_CLOSE:
        {
            int fd = (int)regs->ebx;
            process_t* proc = (process_t*)sched_current();
            fd_entry_t* ent = process_fd_get(proc, fd);
            if (ent && ent->writable) {
                if (!process_fd_flush(ent)) {
                    regs->eax = SYSCALL_ERR_INVALID;
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
                                                      (unsigned int*)regs->edx);
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

        default:
            regs->eax = SYSCALL_ERR_INVALID;
            break;
    }
}
