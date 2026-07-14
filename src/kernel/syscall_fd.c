#include "syscall_internal.h"
#include "keyboard.h"
#include "input.h"
#include "klib.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "socket.h"
#include "terminal.h"
#include "timer.h"
#include "uapi_errno.h"
#include "wait.h"

#define SYSCALL_MAX_WRITE_LEN 4096u
#define EPOLL_MAX_WATCHES     64u
#define POLL_MAX_FDS          PROCESS_FD_LIMIT_HARD
#define SYS_IOCTL_TIOCGPGRP   0x540Fu
#define SYS_IOCTL_TIOCSPGRP   0x5410u
#define SYS_IOCTL_TIOCGWINSZ  0x5413u
#define SYS_IOCTL_TIOCSWINSZ  0x5414u
#define SYS_IOCTL_TIOCSCTTY   0x540Eu
#define SYS_IOCTL_TIOCNOTTY   0x5422u

typedef struct epoll_watch {
    int used;
    int fd;
    unsigned int events;
    unsigned int data_u32;
} epoll_watch_t;

int sys_write_impl(const char* buf, unsigned int len) {
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

int sys_putc_impl(unsigned int ch) {
    char c = (char)ch;
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* stdout_ent = proc ? process_fd_get(proc, 1) : 0;

    if (!stdout_ent) {
        terminal_putc(c);
        return 1;
    }
    return process_fd_write(stdout_ent, &c, 1);
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
int sys_read_impl(char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    return process_fd_read(process_fd_get(proc, 0), buf, len);
}

int sys_read_raw_impl(char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    return process_fd_read_raw(process_fd_get(proc, 0), buf, len);
}

int sys_pipe2_impl(int* user_fds, unsigned int flags) {
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

int sys_pty_open_impl(int* user_fds, unsigned int master_flags) {
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

int sys_pty_set_size_impl(int fd, unsigned int rows, unsigned int cols) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    return process_fd_pty_set_size(process_fd_get(proc, fd), rows, cols);
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

static void sys_poll_clear_waits(process_t* proc) {
    socket_wait_clear_process(proc);
    wait_queue_remove_proc(proc);
    if (keyboard_get_waiting_process() == (void*)proc) {
        keyboard_set_waiting_process(0);
    }
    input_forget_waiting_process(proc);
}

int sys_input_fd_wait_until_impl(syscall_regs_t* regs, struct pollfd* fds,
                                 unsigned int nfds, unsigned int deadline) {
    process_t* proc = (process_t*)sched_current();
    int has_deadline = deadline != 0u;
    (void)regs;
    if (!proc) return -EINVAL;
    if (nfds > POLL_MAX_FDS) return -EINVAL;
    if (nfds && !user_count_bytes_ok((unsigned int)fds, nfds,
                                     sizeof(struct pollfd), 0)) return -EFAULT;
    for (;;) {
        unsigned int result = 0u;
        unsigned int ready = nfds ? sys_poll_snapshot(proc, fds, nfds) : 0u;
        if (input_available()) result |= SYS_INPUT_FD_WAIT_INPUT;
        if (ready) result |= SYS_INPUT_FD_WAIT_READY;
        if (result) {
            sys_poll_clear_waits(proc);
            return (int)result;
        }
        if (has_deadline && (int)(timer_get_ticks() - deadline) >= 0) {
            sys_poll_clear_waits(proc);
            return SYS_INPUT_FD_WAIT_DEADLINE;
        }
        proc->sleep_until = has_deadline ? deadline : 0u;
        proc->state = has_deadline ? PROCESS_STATE_SLEEPING
                                   : PROCESS_STATE_WAITING;
        input_set_waiting_process(proc);
        if (nfds) {
            int rc = sys_poll_register_fd_waits(proc, fds, nfds);
            if (rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                sys_poll_clear_waits(proc);
                return rc;
            }
        }
        ready = nfds ? sys_poll_snapshot(proc, fds, nfds) : 0u;
        if (input_available() || ready) {
            proc->state = PROCESS_STATE_RUNNING;
            sys_poll_clear_waits(proc);
            return (input_available() ? SYS_INPUT_FD_WAIT_INPUT : 0) |
                   (ready ? SYS_INPUT_FD_WAIT_READY : 0);
        }
        sys_wait_until_current_running(proc);
        sys_poll_clear_waits(proc);
    }
}

int sys_poll_impl(syscall_regs_t* regs, struct pollfd* fds,
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
            sys_poll_clear_waits(proc);
            return (int)ready;
        }

        if (!infinite_wait && (int)(timer_get_ticks() - deadline) >= 0) {
            sys_poll_clear_waits(proc);
            return 0;
        }

        proc->sleep_until = infinite_wait ? 0u : deadline;
        proc->state = infinite_wait ? PROCESS_STATE_WAITING
                                    : PROCESS_STATE_SLEEPING;
        {
            int wait_rc = sys_poll_register_fd_waits(proc, fds, nfds);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                sys_poll_clear_waits(proc);
                return wait_rc;
            }
        }
        ready = sys_poll_snapshot(proc, fds, nfds);
        if (ready != 0u) {
            proc->state = PROCESS_STATE_RUNNING;
            sys_poll_clear_waits(proc);
            return (int)ready;
        }

        (void)regs;
        sys_wait_until_current_running(proc);

        sys_poll_clear_waits(proc);
    }
}

int sys_fcntl_impl(int fd, int cmd, unsigned int arg) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;

    if (cmd == SYS_FCNTL_DUPFD) {
        return process_fd_dup(proc, fd, (int)arg, 0);
    }
    if (cmd == SYS_FCNTL_DUPFD_CLOEXEC) {
        return process_fd_dup(proc, fd, (int)arg, SYS_FD_FLAG_CLOEXEC);
    }
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

int sys_dup_impl(int oldfd) {
    process_t* proc = (process_t*)sched_current();
    return process_fd_dup(proc, oldfd, 0, 0);
}

int sys_dup2_impl(int oldfd, int newfd) {
    process_t* proc = (process_t*)sched_current();
    return process_fd_dup2(proc, oldfd, newfd, 0, 0);
}

int sys_dup3_impl(int oldfd, int newfd, unsigned int flags) {
    process_t* proc = (process_t*)sched_current();
    unsigned int fd_flags = 0u;

    if ((flags & ~SYS_FD_FLAG_CLOEXEC) != 0u) return -EINVAL;
    if ((flags & SYS_FD_FLAG_CLOEXEC) != 0u) fd_flags = SYS_FD_FLAG_CLOEXEC;
    return process_fd_dup2(proc, oldfd, newfd, fd_flags, 1);
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

int sys_epoll_create_impl(int flags) {
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

int sys_epoll_ctl_impl(int epfd, int op, int fd,
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
            sys_poll_clear_waits(proc);
            return rc;
        }
    }

    return 0;
}

int sys_epoll_wait_impl(syscall_regs_t* regs,
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
            sys_poll_clear_waits(proc);
            return (int)ready;
        }

        if (!infinite_wait && (int)(timer_get_ticks() - timeout_deadline) >= 0) {
            sys_poll_clear_waits(proc);
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
                sys_poll_clear_waits(proc);
                return wait_rc;
            }
        }
        ready = watches ? epoll_snapshot(proc, watches, events,
                                         (unsigned int)maxevents)
                        : 0u;
        if (ready != 0u) {
            proc->state = PROCESS_STATE_RUNNING;
            sys_poll_clear_waits(proc);
            return (int)ready;
        }

        (void)regs;
        sys_wait_until_current_running(proc);

        sys_poll_clear_waits(proc);
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

int sys_timerfd_create_impl(int clock_id, int flags) {
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


int sys_timerfd_settime_impl(int fd,
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

int sys_signalfd_impl(int fd, const void* mask, int flags) {
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


int sys_terminal_size_impl(unsigned int* out_rows, unsigned int* out_cols) {
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

int sys_tcgetattr_impl(int fd, sys_termios_t* out) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    sys_termios_t ktio;
    int rc;

    if (!out) return -EFAULT;
    if (!user_buf_ok((unsigned int)out, sizeof(*out))) return -EFAULT;
    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    rc = process_fd_terminal_getattr(ent, &ktio);
    if (rc < 0) return rc;
    if (copy_to_user(out, &ktio, sizeof(ktio)) < 0) return -EFAULT;
    return 0;
}

int sys_tcsetattr_impl(int fd, const sys_termios_t* in) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    sys_termios_t ktio;

    if (!in) return -EFAULT;
    if (!user_buf_ok((unsigned int)in, sizeof(*in))) return -EFAULT;
    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (copy_from_user(&ktio, in, sizeof(ktio)) < 0) return -EFAULT;
    return process_fd_terminal_setattr(ent, &ktio);
}

int sys_tty_ioctl_impl(int fd, unsigned int request, void* arg) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (!process_fd_is_terminal(ent)) return -ENOTTY;

    if (request == SYS_IOCTL_TIOCSCTTY || request == SYS_IOCTL_TIOCNOTTY) {
        return 0;
    }

    if (request == SYS_IOCTL_TIOCGWINSZ) {
        sys_winsize_t ws;
        unsigned int rows = 0;
        unsigned int cols = 0;
        int rc;

        if (!arg || !user_buf_ok((unsigned int)arg, sizeof(ws))) return -EFAULT;
        rc = process_fd_terminal_size(ent, &rows, &cols);
        if (rc < 0) return rc;
        k_memset(&ws, 0, sizeof(ws));
        ws.ws_row = (unsigned short)rows;
        ws.ws_col = (unsigned short)cols;
        if (copy_to_user(arg, &ws, sizeof(ws)) < 0) return -EFAULT;
        return 0;
    }

    if (request == SYS_IOCTL_TIOCSWINSZ) {
        sys_winsize_t ws;

        if (!arg || !user_buf_ok((unsigned int)arg, sizeof(ws))) return -EFAULT;
        if (copy_from_user(&ws, arg, sizeof(ws)) < 0) return -EFAULT;
        return process_fd_pty_set_size(ent, ws.ws_row, ws.ws_col);
    }

    if (request == SYS_IOCTL_TIOCGPGRP) {
        u32 pgid = 0;
        int rc;

        if (!arg || !user_buf_ok((unsigned int)arg, sizeof(int))) return -EFAULT;
        rc = process_fd_terminal_get_pgrp(ent, proc->pgid, &pgid);
        if (rc < 0) return rc;
        if (copy_to_user(arg, &pgid, sizeof(int)) < 0) return -EFAULT;
        return 0;
    }

    if (request == SYS_IOCTL_TIOCSPGRP) {
        u32 pgid = 0;
        process_t* group_proc;
        int rc;

        if (!arg || !user_buf_ok((unsigned int)arg, sizeof(int))) return -EFAULT;
        if (copy_from_user(&pgid, arg, sizeof(int)) < 0) return -EFAULT;
        group_proc = process_find_by_pgid(pgid);
        if (!group_proc) return -ESRCH;
        if (group_proc->sid != proc->sid) return -EPERM;
        rc = process_fd_terminal_set_pgrp(ent, pgid);
        return rc;
    }

    return -ENOTTY;
}
