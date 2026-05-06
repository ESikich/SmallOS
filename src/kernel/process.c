#include "process.h"
#include "pmm.h"
#include "paging.h"
#include "klib.h"
#include "terminal.h"
#include "scheduler.h"
#include "keyboard.h"
#include "shell.h"
#include "timer.h"
#include "../drivers/tcp.h"
#include "uapi_poll.h"
#include "uapi_errno.h"
#include "uapi_syscall.h"
#include "vfs.h"
#include "socket.h"

typedef char process_t_must_fit_in_one_frame[(sizeof(process_t) <= 4096u) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static process_t* s_foreground = 0;
static volatile int s_terminal_interrupt_pending = 0;
static volatile process_t* s_detach_requested = 0;

#define PROCESS_TERMINATED_BY_CTRL_C 130

/*
 * process_key_consumer — keyboard consumer active while a user process
 * holds the foreground.  Ctrl+C is handled as a terminal interrupt for
 * the foreground process; ordinary ASCII is buffered for SYS_READ.
 *
 * True-blocking SYS_READ wake-up:
 *   After pushing the character into kb_buf, check whether a process is
 *   parked in PROCESS_STATE_WAITING.  If so, mark it PROCESS_STATE_RUNNING
 *   and clear the waiting slot so the scheduler will pick it up on the
 *   next pass.
 *
 *   This runs entirely in IRQ1 context (IF=0).  It must not allocate or
 *   block; if Ctrl+C interrupts the currently running process, it records
 *   a pending terminal interrupt and lets irq1_handler_main switch away
 *   after keyboard_handle_irq() returns with the saved IRQ frame ESP.
 */
static void process_key_consumer(key_event_t ev) {
    if (ev.ctrl && ev.key == KEY_C) {
        process_t* proc = s_foreground;
        if (!proc) return;

        terminal_puts("^C\n");
        keyboard_buf_clear();
        if (keyboard_get_waiting_process() == (void*)proc) {
            keyboard_set_waiting_process(0);
        }
        socket_wait_clear_process(proc);

        proc->exit_status = PROCESS_TERMINATED_BY_CTRL_C;
        if (proc == sched_current()) {
            s_terminal_interrupt_pending = 1;
        } else {
            sched_kill(proc, 0);
        }
        return;
    }

    if (ev.ctrl && ev.key == KEY_Z) {
        process_t* proc = s_foreground;
        if (!proc) return;

        terminal_puts("^Z\n");
        keyboard_buf_clear();
        s_detach_requested = proc;
        s_foreground = 0;
        shell_register_consumer();
        return;
    }

    if (!ev.ascii) {
        const char* seq = 0;

        switch (ev.key) {
            case KEY_UP:       seq = "\x1b[A"; break;
            case KEY_DOWN:     seq = "\x1b[B"; break;
            case KEY_RIGHT:    seq = "\x1b[C"; break;
            case KEY_LEFT:     seq = "\x1b[D"; break;
            case KEY_ESC:      seq = "\x1b"; break;
            case KEY_HOME:     seq = "\x1b[H"; break;
            case KEY_END:      seq = "\x1b[F"; break;
            case KEY_INSERT:   seq = "\x1b[2~"; break;
            case KEY_DELETE:   seq = "\x1b[3~"; break;
            case KEY_PAGEUP:   seq = "\x1b[5~"; break;
            case KEY_PAGEDOWN: seq = "\x1b[6~"; break;
            case KEY_F1:       seq = "\x1bOP"; break;
            case KEY_F2:       seq = "\x1bOQ"; break;
            case KEY_F3:       seq = "\x1bOR"; break;
            case KEY_F4:       seq = "\x1bOS"; break;
            case KEY_F10:      seq = "\x1b[21~"; break;
            default: break;
        }

        if (!seq) return;
        for (int i = 0; seq[i] != '\0'; i++) {
            keyboard_buf_push_char(seq[i]);
        }

        process_t* waiter = (process_t*)keyboard_get_waiting_process();
        if (waiter && waiter->state == PROCESS_STATE_WAITING) {
            waiter->state = PROCESS_STATE_RUNNING;
            keyboard_set_waiting_process(0);
        }
        return;
    }

    keyboard_buf_push_char(ev.ascii);

    process_t* waiter = (process_t*)keyboard_get_waiting_process();
    if (waiter && waiter->state == PROCESS_STATE_WAITING) {
        waiter->state = PROCESS_STATE_RUNNING;
        keyboard_set_waiting_process(0);
    }
}

static void proc_zero(process_t* p) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned int i = 0; i < sizeof(process_t); i++) b[i] = 0;
}

static void str_copy_n(char* dst, const char* src, unsigned int n) {
    unsigned int i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static unsigned int process_fd_frame_count(unsigned int capacity) {
    unsigned int bytes = capacity * (unsigned int)sizeof(fd_entry_t);
    return PAGE_ALIGN(bytes) / PAGE_SIZE;
}

static int process_fd_table_alloc(unsigned int capacity,
                                  fd_entry_t** out_fds,
                                  u32* out_frame,
                                  u32* out_frames) {
    u32 frames;
    u32 frame;
    fd_entry_t* fds;

    if (!out_fds || !out_frame || !out_frames) return -EINVAL;
    if (capacity == 0u || capacity > PROCESS_FD_LIMIT_HARD) return -EINVAL;

    frames = process_fd_frame_count(capacity);
    frame = pmm_alloc_contiguous_frames(frames);
    if (!frame) return -ENOMEM;

    fds = (fd_entry_t*)paging_phys_to_kernel_virt(frame);
    k_memset(fds, 0, frames * PAGE_SIZE);

    *out_fds = fds;
    *out_frame = frame;
    *out_frames = frames;
    return 0;
}

static void process_fd_table_free(process_t* proc) {
    if (!proc || !proc->fd_table_frame || !proc->fd_table_frames) return;

    pmm_free_contiguous_frames(proc->fd_table_frame, proc->fd_table_frames);
    proc->fds = 0;
    proc->fd_capacity = 0;
    proc->fd_table_frame = 0;
    proc->fd_table_frames = 0;
}

static int process_fd_table_init(process_t* proc, unsigned int limit) {
    fd_entry_t* fds = 0;
    u32 frame = 0;
    u32 frames = 0;
    unsigned int capacity = PROCESS_FD_INITIAL_CAPACITY;
    int rc;

    if (!proc) return -EINVAL;
    if (limit < PROCESS_FD_INITIAL_CAPACITY) {
        limit = PROCESS_FD_INITIAL_CAPACITY;
    }
    if (limit > PROCESS_FD_LIMIT_HARD) {
        limit = PROCESS_FD_LIMIT_HARD;
    }
    if (capacity > limit) {
        capacity = limit;
    }

    rc = process_fd_table_alloc(capacity, &fds, &frame, &frames);
    if (rc < 0) return rc;

    proc->fds = fds;
    proc->fd_capacity = capacity;
    proc->fd_limit = limit;
    proc->fd_table_frame = frame;
    proc->fd_table_frames = frames;
    return 0;
}

static int process_fd_table_grow(process_t* proc, unsigned int min_capacity) {
    fd_entry_t* new_fds = 0;
    u32 new_frame = 0;
    u32 new_frames = 0;
    unsigned int new_capacity;
    int rc;

    if (!proc || !proc->fds) return -EINVAL;
    if (min_capacity <= proc->fd_capacity) return 0;
    if (proc->fd_capacity >= proc->fd_limit) return -ENFILE;
    if (min_capacity > proc->fd_limit) return -ENFILE;

    new_capacity = proc->fd_capacity * 2u;
    if (new_capacity < min_capacity) {
        new_capacity = min_capacity;
    }
    if (new_capacity > proc->fd_limit) {
        new_capacity = proc->fd_limit;
    }

    rc = process_fd_table_alloc(new_capacity, &new_fds, &new_frame, &new_frames);
    if (rc < 0) return rc;

    k_memcpy(new_fds, proc->fds, proc->fd_capacity * (unsigned int)sizeof(fd_entry_t));
    pmm_free_contiguous_frames(proc->fd_table_frame, proc->fd_table_frames);

    proc->fds = new_fds;
    proc->fd_capacity = new_capacity;
    proc->fd_table_frame = new_frame;
    proc->fd_table_frames = new_frames;
    return 0;
}

static int process_fd_alloc_entry(process_t* proc, fd_entry_t** out_ent) {
    int rc;

    if (!proc || !out_ent) return -EINVAL;
    if (!proc->fds || proc->fd_capacity <= PROCESS_FD_FIRST) return -EINVAL;

    for (;;) {
        for (unsigned int fd = PROCESS_FD_FIRST; fd < proc->fd_capacity; fd++) {
            if (!proc->fds[fd].valid) {
                *out_ent = &proc->fds[fd];
                return (int)fd;
            }
        }

        if (proc->fd_capacity >= proc->fd_limit) {
            return -ENFILE;
        }

        rc = process_fd_table_grow(proc, proc->fd_capacity + 1u);
        if (rc < 0) return rc;
    }
}

static int process_handle_console_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_console_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_console_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_console_poll(fd_entry_t* ent, short events);
static void process_handle_console_close(fd_entry_t* ent);

static int process_handle_socket_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_socket_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_socket_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_socket_poll(fd_entry_t* ent, short events);
static void process_handle_socket_close(fd_entry_t* ent);

static int process_handle_special_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_special_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_special_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_special_poll(fd_entry_t* ent, short events);
static void process_handle_special_close(fd_entry_t* ent);

static const process_handle_ops_t s_socket_handle_ops = {
    .read = process_handle_socket_read,
    .write = process_handle_socket_write,
    .seek = process_handle_socket_seek,
    .poll = process_handle_socket_poll,
    .flush = 0,
    .close = process_handle_socket_close,
};

static const process_handle_ops_t s_special_handle_ops = {
    .read = process_handle_special_read,
    .write = process_handle_special_write,
    .seek = process_handle_special_seek,
    .poll = process_handle_special_poll,
    .flush = 0,
    .close = process_handle_special_close,
};

static const process_handle_ops_t s_console_handle_ops = {
    .read = process_handle_console_read,
    .write = process_handle_console_write,
    .seek = process_handle_console_seek,
    .poll = process_handle_console_poll,
    .flush = 0,
    .close = process_handle_console_close,
};

static void process_init_standard_fds(process_t* proc) {
    if (!proc) return;
    if (!proc->fds || proc->fd_capacity < PROCESS_FD_FIRST) return;

    proc->fds[0].valid = 1;
    proc->fds[0].kind = PROCESS_HANDLE_KIND_CONSOLE;
    proc->fds[0].ops = &s_console_handle_ops;
    proc->fds[0].readable = 1;
    proc->fds[0].writable = 0;

    for (int fd = 1; fd <= 2; fd++) {
        proc->fds[fd].valid = 1;
        proc->fds[fd].kind = PROCESS_HANDLE_KIND_CONSOLE;
        proc->fds[fd].ops = &s_console_handle_ops;
        proc->fds[fd].readable = 0;
        proc->fds[fd].writable = 1;
    }
}

fd_entry_t* process_fd_get(process_t* proc, int fd) {
    if (!proc) return 0;
    if (!proc->fds) return 0;
    if (fd < 0 || (unsigned int)fd >= proc->fd_capacity) return 0;
    if (!proc->fds[fd].valid) return 0;
    return &proc->fds[fd];
}

int process_fd_open_file_mode(process_t* proc,
                              const char* name,
                              u32 size,
                              int readable,
                              int writable) {
    fd_entry_t* ent;
    int fd;

    if (!proc || !name) return -EINVAL;

    fd = process_fd_alloc_entry(proc, &ent);
    if (fd < 0) return fd;

    k_memset(ent, 0, sizeof(*ent));
    ent->valid = 1;
    vfs_file_init(ent, name, size, readable, writable);
    return fd;
}

int process_fd_open_file(process_t* proc, const char* name, u32 size, int writable) {
    return process_fd_open_file_mode(proc, name, size, writable ? 0 : 1, writable);
}

int process_fd_open_socket(process_t* proc, const char* name) {
    fd_entry_t* ent;
    socket_t* sock;
    int fd;

    if (!proc) return -EINVAL;

    fd = process_fd_alloc_entry(proc, &ent);
    if (fd < 0) return fd;

    sock = socket_create_tcp();
    if (!sock) return -ENOMEM;

    k_memset(ent, 0, sizeof(*ent));
    ent->valid = 1;
    ent->kind = PROCESS_HANDLE_KIND_SOCKET;
    ent->ops = &s_socket_handle_ops;
    ent->socket = sock;
    ent->socket_state = PROCESS_SOCKET_STATE_OPEN;
    ent->socket_port = 0;
    ent->socket_conn = TCP_SOCKET_CONN_NONE;
    ent->readable = 1;
    ent->writable = 0;
    if (name) {
        k_memcpy(ent->name, name, (k_size_t)k_strlen(name) + 1u);
    }
    return fd;
}

int process_fd_open_special(process_t* proc, int kind, const char* name) {
    fd_entry_t* ent;
    int fd;

    if (!proc) return -EINVAL;
    if (kind != PROCESS_HANDLE_KIND_EPOLL &&
        kind != PROCESS_HANDLE_KIND_TIMERFD &&
        kind != PROCESS_HANDLE_KIND_SIGNALFD) {
        return -EINVAL;
    }

    fd = process_fd_alloc_entry(proc, &ent);
    if (fd < 0) return fd;

    k_memset(ent, 0, sizeof(*ent));
    ent->valid = 1;
    ent->kind = kind;
    ent->ops = &s_special_handle_ops;
    ent->readable = 1;
    ent->writable = 0;
    if (name) {
        k_memcpy(ent->name, name, (k_size_t)k_strlen(name) + 1u);
    }
    return fd;
}

void process_fd_close(fd_entry_t* ent) {
    if (!ent) return;

    if (ent->writable) {
        (void)process_fd_flush(ent);
    }

    if (ent->ops && ent->ops->close) {
        ent->ops->close(ent);
        return;
    }

    k_memset(ent, 0, sizeof(*ent));
}

static int process_handle_socket_read(fd_entry_t* ent, char* buf, unsigned int len) {
    int rc;
    socket_t* sock;
    process_t* proc;

    if (!ent || !ent->valid) return -EBADF;
    sock = ent->socket;
    if (socket_state(sock) != SOCKET_STATE_CONNECTED) return -EINVAL;
    if (len == 0) return 0;

    if (!socket_tcp_recv_ready(sock)) {
        if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            return -EAGAIN;
        }
    }

    proc = sched_current();
    while (!socket_tcp_recv_ready(sock)) {
        int wait_rc;

        if (!socket_tcp_connection_established(sock)) {
            __asm__ __volatile__("cli");
            return 0;
        }

        if (!proc) {
            __asm__ __volatile__("sti; hlt; cli");
            continue;
        }

        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(sock, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_tcp_recv_ready(sock)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }

        __asm__ __volatile__("sti");
        while (proc->state != PROCESS_STATE_RUNNING) {
            __asm__ __volatile__("hlt");
        }
        __asm__ __volatile__("cli");
        socket_wait_clear_process(proc);
    }
    __asm__ __volatile__("cli");
    socket_wait_clear_process(proc);
    rc = socket_tcp_recv(sock, buf, len);
    return rc < 0 ? -ECONNRESET : rc;
}

static int process_handle_socket_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    int rc;
    socket_t* sock;
    process_t* proc;
    unsigned int done = 0u;

    if (!ent || !ent->valid) return -EBADF;
    sock = ent->socket;
    if (socket_state(sock) != SOCKET_STATE_CONNECTED) return -EINVAL;
    if (len == 0) return 0;

    proc = sched_current();
    while (done < len) {
        rc = socket_tcp_send(sock, buf + done, len - done);
        if (rc > 0) {
            done += (unsigned int)rc;
            if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
                break;
            }
            continue;
        }

        if (rc < 0 && rc != -EAGAIN) {
            if (done) {
                return (int)done;
            }
            if (rc == -ENOMEM || rc == -EPIPE) {
                return rc;
            }
            return -ECONNRESET;
        }

        if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            return done ? (int)done : -EAGAIN;
        }

        if (!socket_tcp_connection_established(sock)) {
            return done ? (int)done : -ECONNRESET;
        }

        if (!proc) {
            __asm__ __volatile__("sti; hlt; cli");
            continue;
        }

        proc->state = PROCESS_STATE_WAITING;
        rc = socket_wait(sock, proc, POLLOUT);
        if (rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return done ? (int)done : rc;
        }
        if (socket_tcp_send_ready(sock) ||
            !socket_tcp_connection_established(sock)) {
            proc->state = PROCESS_STATE_RUNNING;
        }

        __asm__ __volatile__("sti");
        while (proc->state != PROCESS_STATE_RUNNING) {
            __asm__ __volatile__("hlt");
        }
        __asm__ __volatile__("cli");
        socket_wait_clear_process(proc);
    }

    return (int)done;
}

static int process_handle_socket_seek(fd_entry_t* ent, int offset, int whence) {
    (void)ent;
    (void)offset;
    (void)whence;
    return -ENOSYS;
}

static short process_handle_socket_poll(fd_entry_t* ent, short events) {
    if (!ent || !ent->valid) return POLLERR;
    return socket_poll(ent->socket, events);
}

static int process_handle_console_read_common(fd_entry_t* ent, char* buf, unsigned int len, int echo) {
    process_t* proc = sched_current();
    unsigned int n = 0;

    if (!ent || !ent->valid || ent->writable) return -EBADF;
    if (!buf) return -EFAULT;
    if (len == 0) return 0;

    __asm__ volatile ("sti");

    while (n < len) {
        while (!keyboard_buf_available()) {
            if (proc) {
                proc->state = PROCESS_STATE_WAITING;
                keyboard_set_waiting_process(proc);
            }
            __asm__ volatile ("hlt");
        }

        char c = keyboard_buf_pop();
        if (echo) {
            terminal_putc(c);
        }
        buf[n++] = c;
        if (c == '\n') break;
    }

    __asm__ volatile ("cli");
    return (int)n;
}

static int process_handle_console_read(fd_entry_t* ent, char* buf, unsigned int len) {
    return process_handle_console_read_common(ent, buf, len, 1);
}

static int process_handle_console_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    if (!ent || !ent->valid || !ent->writable) return -EBADF;
    if (!buf) return -EFAULT;

    for (unsigned int i = 0; i < len; i++) {
        terminal_putc(buf[i]);
    }
    return (int)len;
}

static int process_handle_console_seek(fd_entry_t* ent, int offset, int whence) {
    (void)ent;
    (void)offset;
    (void)whence;
    return -ENOSYS;
}

static short process_handle_console_poll(fd_entry_t* ent, short events) {
    short revents = 0;

    if (!ent || !ent->valid) return POLLERR;
    if (!ent->writable && (events & POLLIN) && keyboard_buf_available()) {
        revents |= POLLIN;
    }
    if (ent->writable && (events & POLLOUT)) {
        revents |= POLLOUT;
    }
    return revents;
}

static void process_handle_console_close(fd_entry_t* ent) {
    if (!ent) return;
    k_memset(ent, 0, sizeof(*ent));
}

static void process_handle_socket_close(fd_entry_t* ent) {
    if (!ent) return;
    socket_release(ent->socket);
    k_memset(ent, 0, sizeof(*ent));
}

static int timerfd_ready(fd_entry_t* ent) {
    if (!ent || ent->kind != PROCESS_HANDLE_KIND_TIMERFD) return 0;
    return ent->timer_deadline != 0u &&
           (int)(timer_get_ticks() - ent->timer_deadline) >= 0;
}

static unsigned long long timerfd_consume(fd_entry_t* ent) {
    unsigned int now;
    unsigned int elapsed;
    unsigned int expirations;

    if (!timerfd_ready(ent)) return 0;
    now = timer_get_ticks();
    expirations = 1u;

    if (ent->timer_interval != 0u) {
        elapsed = now - ent->timer_deadline;
        expirations += elapsed / ent->timer_interval;
        ent->timer_deadline += expirations * ent->timer_interval;
    } else {
        ent->timer_deadline = 0u;
    }

    return (unsigned long long)expirations;
}

static int process_handle_special_read(fd_entry_t* ent, char* buf, unsigned int len) {
    unsigned long long expirations;

    if (!ent || !ent->valid) return -EBADF;
    if (!buf) return -EFAULT;

    if (ent->kind == PROCESS_HANDLE_KIND_TIMERFD) {
        if (len < sizeof(unsigned long long)) return -EINVAL;
        if (!timerfd_ready(ent)) return -EAGAIN;

        expirations = timerfd_consume(ent);
        k_memcpy(buf, &expirations, sizeof(expirations));
        return (int)sizeof(expirations);
    }

    if (ent->kind == PROCESS_HANDLE_KIND_SIGNALFD) {
        return -EAGAIN;
    }

    return -EINVAL;
}

static int process_handle_special_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    (void)ent;
    (void)buf;
    (void)len;
    return -EBADF;
}

static int process_handle_special_seek(fd_entry_t* ent, int offset, int whence) {
    (void)ent;
    (void)offset;
    (void)whence;
    return -EINVAL;
}

static short process_handle_special_poll(fd_entry_t* ent, short events) {
    short revents = 0;

    if (!ent || !ent->valid) return POLLERR;

    if (ent->kind == PROCESS_HANDLE_KIND_TIMERFD) {
        if ((events & POLLIN) && timerfd_ready(ent)) {
            revents |= POLLIN;
        }
        return revents;
    }

    if (ent->kind == PROCESS_HANDLE_KIND_SIGNALFD) {
        return 0;
    }

    return POLLERR;
}

static void process_handle_special_close(fd_entry_t* ent) {
    if (!ent) return;
    if (ent->aux_frame) {
        pmm_free_frame(ent->aux_frame);
    }
    k_memset(ent, 0, sizeof(*ent));
}

int process_fd_read(fd_entry_t* ent, char* buf, unsigned int len) {
    if (!ent || !ent->ops) return -EBADF;
    if (!ent->ops->read) return -ENOSYS;
    return ent->ops->read(ent, buf, len);
}

int process_fd_read_raw(fd_entry_t* ent, char* buf, unsigned int len) {
    if (!ent || !ent->ops) return -EBADF;
    if (ent->kind == PROCESS_HANDLE_KIND_CONSOLE) {
        return process_handle_console_read_common(ent, buf, len, 0);
    }
    return process_fd_read(ent, buf, len);
}

int process_fd_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    if (!ent || !ent->ops) return -EBADF;
    if (!ent->ops->write) return -ENOSYS;
    return ent->ops->write(ent, buf, len);
}

short process_fd_poll(fd_entry_t* ent, short events) {
    if (!ent || !ent->ops || !ent->ops->poll) return POLLERR;
    return ent->ops->poll(ent, events);
}

int process_fd_seek(fd_entry_t* ent, int offset, int whence) {
    if (!ent || !ent->ops) return -EBADF;
    if (!ent->ops->seek) return -ENOSYS;
    return ent->ops->seek(ent, offset, whence);
}

int process_fd_flush(fd_entry_t* ent) {
    if (!ent || !ent->ops || !ent->ops->flush) return 0;
    return ent->ops->flush(ent);
}

int process_fd_set_flags(fd_entry_t* ent, unsigned int flags) {
    if (!ent || !ent->valid) return -EBADF;
    ent->flags = flags & SYS_FD_FLAG_NONBLOCK;
    return 0;
}

unsigned int process_fd_get_flags(fd_entry_t* ent) {
    if (!ent || !ent->valid) return 0;
    return ent->flags;
}

void process_claim_for_wait(process_t* proc) {
    if (!proc) return;
    proc->reaper_claimed = 1;
}

/*
 * Copy launch argv into process-owned storage.  The incoming argv may point at
 * shell parser storage or at a temporary SYS_EXEC validation buffer; after this
 * returns, bootstrap uses only proc->user_arg_data/user_argv.
 */
int process_set_args(process_t* proc, int argc, char** argv) {
    unsigned int used = 0;

    if (!proc) return -EINVAL;
    if (argc < 0 || argc > PROCESS_MAX_ARGS) return -EINVAL;
    if (argc > 0 && !argv) return -EFAULT;

    proc->user_argc = 0;
    proc->user_argv[0] = 0;
    proc->user_arg_data[0] = '\0';

    for (int i = 0; i < argc; i++) {
        if (!argv[i]) return -EFAULT;

        int len = k_strlen(argv[i]) + 1;
        if (used + (unsigned int)len > PROCESS_ARG_BYTES) {
            return -EINVAL;
        }

        proc->user_argv[i] = &proc->user_arg_data[used];
        k_memcpy(proc->user_argv[i], argv[i], (k_size_t)len);
        used += (unsigned int)len;
    }

    proc->user_argc = argc;
    proc->user_argv[argc] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Kernel-task bootstrap                                              */
/* ------------------------------------------------------------------ */

static void process_kernel_task_bootstrap(void) {
    process_t* proc = sched_current();

    if (!proc || !proc->kernel_entry) {
        terminal_puts("process: kernel task bootstrap failed\n");
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }

    proc->kernel_entry();

    terminal_puts("process: kernel task returned\n");
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

process_t* process_create(const char* name) {
    u32 frame = pmm_alloc_frame();
    int fd_rc;

    if (!frame) {
        terminal_puts("process: out of frames for process_t\n");
        return 0;
    }

    process_t* proc = (process_t*)paging_phys_to_kernel_virt(frame);
    proc_zero(proc);

    proc->state = PROCESS_STATE_UNUSED;
    proc->heap_base = USER_HEAP_BASE;
    proc->heap_brk = USER_HEAP_BASE;
    fd_rc = process_fd_table_init(proc, PROCESS_FD_LIMIT_DEFAULT);
    if (fd_rc < 0) {
        terminal_puts("process: out of frames for fd table\n");
        pmm_free_frame(frame);
        return 0;
    }
    process_init_standard_fds(proc);
    if (name) {
        str_copy_n(proc->name, name, PROCESS_NAME_MAX);
    }

    return proc;
}

process_t* process_create_kernel_task(const char* name, void (*entry)(void)) {
    process_t* proc = process_create(name);
    if (!proc) return 0;

    proc->kernel_stack_frame = pmm_alloc_frame();
    if (!proc->kernel_stack_frame) {
        terminal_puts("process: out of frames for kernel stack\n");
        process_destroy(proc);
        return 0;
    }

    proc->pd = 0;
    proc->kernel_entry = entry;
    proc->state = PROCESS_STATE_RUNNING;

    {
        unsigned int* stack_top =
            (unsigned int*)((u8*)paging_phys_to_kernel_virt(proc->kernel_stack_frame) + PAGE_SIZE);
        stack_top--;
        *stack_top = (unsigned int)process_kernel_task_bootstrap;
        proc->sched_esp = (unsigned int)stack_top;
    }

    return proc;
}

void process_destroy(process_t* proc) {
    if (!proc) return;

    /*
     * If this process is currently parked as the keyboard waiter, clear the
     * slot before freeing the process_t frame.  Leaving a dangling pointer
     * in the keyboard driver would cause process_key_consumer() to write
     * through freed memory on the next keypress.
     */
    if (keyboard_get_waiting_process() == (void*)proc) {
        keyboard_set_waiting_process(0);
    }
    socket_wait_clear_process(proc);

    for (unsigned int i = 0; i < proc->fd_capacity; i++) {
        if (proc->fds[i].valid) {
            process_fd_close(&proc->fds[i]);
        }
    }
    process_fd_table_free(proc);

    if (proc->pd) {
        process_pd_destroy(proc->pd);
        proc->pd = 0;
    }

    if (proc->kernel_stack_frame) {
        pmm_free_frame(proc->kernel_stack_frame);
        proc->kernel_stack_frame = 0;
    }

    if (s_foreground == proc) {
        s_foreground = 0;
    }

    proc->state = PROCESS_STATE_EXITED;
    pmm_free_frame(paging_kernel_virt_to_phys(proc));
}

process_t* process_get_current(void) {
    return sched_current();
}

void process_set_foreground(process_t* proc) {
    s_foreground = proc;
    s_detach_requested = 0;
    if (proc) {
        keyboard_buf_clear();           /* discard any input that arrived before
                                           the process was ready to read it,
                                           e.g. the Enter that launched runelf */
        keyboard_set_consumer(process_key_consumer);
    } else {
        shell_register_consumer();
    }
}

process_t* process_get_foreground(void) {
    return s_foreground;
}

void process_deliver_pending_terminal_interrupt(unsigned int esp) {
    process_t* proc;

    if (!s_terminal_interrupt_pending) return;

    proc = sched_current();
    if (!proc || proc != s_foreground) {
        s_terminal_interrupt_pending = 0;
        return;
    }

    s_terminal_interrupt_pending = 0;
    proc->exit_status = PROCESS_TERMINATED_BY_CTRL_C;
    if (keyboard_get_waiting_process() == (void*)proc) {
        keyboard_set_waiting_process(0);
    }
    socket_wait_clear_process(proc);
    paging_switch(paging_get_kernel_pd());
    sched_kill(proc, esp);
}

static int process_wait_impl(process_t* proc, int allow_detach, int* detached) {
    if (!proc) return -1;

    if (detached) {
        *detached = 0;
    }
    process_set_foreground(proc);
    process_claim_for_wait(proc);

    while (proc->state != PROCESS_STATE_ZOMBIE) {
        if (allow_detach && s_detach_requested == proc) {
            s_detach_requested = 0;
            process_set_foreground(0);
            if (detached) {
                *detached = 1;
            }
            return 0;
        }
        __asm__ __volatile__("sti; hlt");
    }

    process_set_foreground(0);
    int status = proc->exit_status;
    process_destroy(proc);
    return status;
}

int process_wait(process_t* proc) {
    return process_wait_impl(proc, 0, 0);
}

int process_wait_detachable(process_t* proc, int* detached) {
    return process_wait_impl(proc, 1, detached);
}

/* ------------------------------------------------------------------ */
/* Reaper task                                                         */
/* ------------------------------------------------------------------ */

/*
 * reaper_task_main — runs as a permanent kernel task.
 *
 * On every wakeup it calls sched_reap_zombies() to destroy any processes
 * that exited without an explicit waiter (e.g. runelf_nowait or SYS_EXEC
 * children).  After each scan it halts until the next timer interrupt
 * wakes it, keeping CPU overhead near zero.
 */
static void reaper_task_main(void) {
    for (;;) {
        sched_reap_zombies();
        __asm__ __volatile__("sti; hlt");
    }
}

int process_start_reaper(void) {
    process_t* reaper = process_create_kernel_task("reaper", reaper_task_main);
    if (!reaper) {
        terminal_puts("process: failed to create reaper task\n");
        return 0;
    }
    if (!sched_enqueue(reaper)) {
        terminal_puts("process: failed to enqueue reaper task\n");
        process_destroy(reaper);
        return 0;
    }
    return 1;
}
