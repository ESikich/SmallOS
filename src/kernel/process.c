#include "process.h"
#include "pmm.h"
#include "paging.h"
#include "klib.h"
#include "terminal.h"
#include "scheduler.h"
#include "keyboard.h"
#include "timer.h"
#include "../drivers/tcp.h"
#include "uapi_poll.h"
#include "uapi_errno.h"
#include "uapi_syscall.h"
#include "vfs.h"
#include "socket.h"
#include "wait.h"
#include "../drivers/display.h"
#include "input.h"

typedef char process_t_must_fit_in_one_frame[(sizeof(process_t) <= 4096u) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static process_t* s_foreground_reader = 0;
static u32 s_foreground_pgid = 0;
static u32 s_next_pid = 1;
#define PROCESS_REGISTRY_MAX 64
static process_t* s_process_registry[PROCESS_REGISTRY_MAX];
static volatile int s_terminal_interrupt_pending = 0;
static process_t* s_terminal_interrupt_target = 0;
static volatile process_t* s_detach_requested = 0;
static volatile int s_detach_allowed = 0;

#define PROCESS_TERMINATED_BY_CTRL_C 130
#define PROCESS_SIGINT  2
#define PROCESS_SIGPIPE 13
#define PROCESS_SIGTERM 15

typedef struct special_wait_object {
    wait_queue_t read_waiters;
    unsigned int signal_mask;
    unsigned int pending_signals;
} special_wait_object_t;

#define PIPE_BUFFER_SIZE PAGE_SIZE

typedef struct pipe_object {
    wait_queue_t read_waiters;
    wait_queue_t write_waiters;
    u32 data_frame;
    unsigned int read_pos;
    unsigned int write_pos;
    unsigned int count;
    unsigned int read_refs;
    unsigned int write_refs;
} pipe_object_t;

typedef struct pty_buffer {
    wait_queue_t read_waiters;
    wait_queue_t write_waiters;
    u32 data_frame;
    unsigned int read_pos;
    unsigned int write_pos;
    unsigned int count;
} pty_buffer_t;

typedef struct pty_object {
    pty_buffer_t master_to_slave;
    pty_buffer_t slave_to_master;
    unsigned int master_refs;
    unsigned int slave_refs;
    unsigned int rows;
    unsigned int cols;
    u32 foreground_pgid;
} pty_object_t;

typedef struct kernel_signalfd_siginfo {
    u32 ssi_signo;
    u32 ssi_errno;
    u32 ssi_code;
    u32 ssi_pid;
    u32 ssi_uid;
    u32 ssi_fd;
    u32 ssi_tid;
    u32 ssi_band;
    u32 ssi_overrun;
    u32 ssi_trapno;
    int ssi_status;
    int ssi_int;
    unsigned long long ssi_ptr;
    unsigned long long ssi_utime;
    unsigned long long ssi_stime;
    unsigned long long ssi_addr;
    unsigned short ssi_addr_lsb;
    u8 pad[46];
} kernel_signalfd_siginfo_t;

static int process_group_force_exit(u32 pgid,
                                    int status,
                                    process_t* defer_current,
                                    int mark_terminal_interrupt);

static int process_registry_add(process_t* proc) {
    if (!proc) return 0;

    for (unsigned int i = 0; i < PROCESS_REGISTRY_MAX; i++) {
        if (!s_process_registry[i]) {
            s_process_registry[i] = proc;
            return 1;
        }
    }

    terminal_puts("process: registry full\n");
    return 0;
}

static void process_registry_remove(process_t* proc) {
    if (!proc) return;

    for (unsigned int i = 0; i < PROCESS_REGISTRY_MAX; i++) {
        if (s_process_registry[i] == proc) {
            s_process_registry[i] = 0;
            return;
        }
    }
}

process_t* process_find_by_pid(u32 pid) {
    if (pid == 0) return 0;

    for (unsigned int i = 0; i < PROCESS_REGISTRY_MAX; i++) {
        process_t* proc = s_process_registry[i];
        if (proc && proc->pid == pid) {
            return proc;
        }
    }

    return 0;
}

static process_t* process_find_child(process_t* parent, int pid) {
    if (!parent) return 0;

    for (unsigned int i = 0; i < PROCESS_REGISTRY_MAX; i++) {
        process_t* proc = s_process_registry[i];
        if (!proc || proc->parent_pid != parent->pid) {
            continue;
        }
        if (pid == -1 || proc->pid == (u32)pid) {
            return proc;
        }
    }

    return 0;
}

static process_t* process_find_zombie_child(process_t* parent, int pid) {
    if (!parent) return 0;

    for (unsigned int i = 0; i < PROCESS_REGISTRY_MAX; i++) {
        process_t* proc = s_process_registry[i];
        if (!proc || proc->parent_pid != parent->pid) {
            continue;
        }
        if (pid != -1 && proc->pid != (u32)pid) {
            continue;
        }
        if (proc->state == PROCESS_STATE_ZOMBIE) {
            return proc;
        }
    }

    return 0;
}

static void process_orphan_children(u32 parent_pid) {
    if (parent_pid == 0) return;

    for (unsigned int i = 0; i < PROCESS_REGISTRY_MAX; i++) {
        process_t* proc = s_process_registry[i];
        if (proc && proc->parent_pid == parent_pid) {
            proc->parent_pid = 0;
            proc->reaper_claimed = 0;
        }
    }
}

/*
 * process_key_consumer — keyboard consumer active while a user process
 * holds the foreground.  Ctrl+C is handled as a terminal interrupt for
 * the foreground process group; ordinary ASCII is buffered for SYS_READ.
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
        u32 pgid = s_foreground_pgid;
        int defaulted;
        if (pgid == 0) return;

        keyboard_buf_clear();
        if (process_group_signal_deliver(pgid, PROCESS_SIGINT)) {
            return;
        }

        defaulted = process_group_force_exit(pgid,
                                             PROCESS_TERMINATED_BY_CTRL_C,
                                             sched_current(),
                                             1);
        if (defaulted) {
            process_set_foreground(0);
            terminal_puts("^C\n");
        }
        return;
    }

    if (!s_foreground_reader || s_foreground_pgid == 0) {
        return;
    }

    if (ev.ctrl && ev.key == KEY_Z) {
        process_t* proc = s_foreground_reader;
        if (!proc) return;
        if (!s_detach_allowed) return;

        terminal_puts("^Z\n");
        keyboard_buf_clear();
        s_detach_requested = proc;
        s_foreground_reader = 0;
        s_foreground_pgid = 0;
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

static int process_fd_alloc_exact(process_t* proc, int fd, fd_entry_t** out_ent) {
    int rc;

    if (!proc || !out_ent) return -EINVAL;
    if (fd < 0 || (unsigned int)fd >= proc->fd_limit) return -EBADF;

    while ((unsigned int)fd >= proc->fd_capacity) {
        rc = process_fd_table_grow(proc, (unsigned int)fd + 1u);
        if (rc < 0) return rc;
    }

    if (proc->fds[fd].valid) {
        process_fd_close(&proc->fds[fd]);
    }
    *out_ent = &proc->fds[fd];
    return fd;
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

static int process_handle_pipe_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_pipe_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_pipe_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_pipe_poll(fd_entry_t* ent, short events);
static void process_handle_pipe_close(fd_entry_t* ent);

static int process_handle_pty_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_pty_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_pty_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_pty_poll(fd_entry_t* ent, short events);
static void process_handle_pty_close(fd_entry_t* ent);

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

static const process_handle_ops_t s_pipe_handle_ops = {
    .read = process_handle_pipe_read,
    .write = process_handle_pipe_write,
    .seek = process_handle_pipe_seek,
    .poll = process_handle_pipe_poll,
    .flush = 0,
    .close = process_handle_pipe_close,
};

static const process_handle_ops_t s_pty_handle_ops = {
    .read = process_handle_pty_read,
    .write = process_handle_pty_write,
    .seek = process_handle_pty_seek,
    .poll = process_handle_pty_poll,
    .flush = 0,
    .close = process_handle_pty_close,
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
    {
        int rc = vfs_file_init(ent, name, size, readable, writable);
        if (rc < 0) {
            k_memset(ent, 0, sizeof(*ent));
            return rc;
        }
    }
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

static pipe_object_t* pipe_object_from_ent(fd_entry_t* ent) {
    if (!ent || ent->kind != PROCESS_HANDLE_KIND_PIPE || !ent->aux_frame) return 0;
    return (pipe_object_t*)paging_phys_to_kernel_virt(ent->aux_frame);
}

static char* pipe_data(pipe_object_t* pipe) {
    if (!pipe || !pipe->data_frame) return 0;
    return (char*)paging_phys_to_kernel_virt(pipe->data_frame);
}

static pty_object_t* pty_object_from_ent(fd_entry_t* ent) {
    if (!ent || !ent->aux_frame) return 0;
    if (ent->kind != PROCESS_HANDLE_KIND_PTY_MASTER &&
        ent->kind != PROCESS_HANDLE_KIND_PTY_SLAVE) {
        return 0;
    }
    return (pty_object_t*)paging_phys_to_kernel_virt(ent->aux_frame);
}

static char* pty_buffer_data(pty_buffer_t* buffer) {
    if (!buffer || !buffer->data_frame) return 0;
    return (char*)paging_phys_to_kernel_virt(buffer->data_frame);
}

static void process_fd_pipe_ref(fd_entry_t* ent) {
    pipe_object_t* pipe;

    if (!ent || !ent->valid || ent->kind != PROCESS_HANDLE_KIND_PIPE) return;
    pipe = pipe_object_from_ent(ent);
    if (!pipe) return;
    if (ent->readable) pipe->read_refs++;
    if (ent->writable) pipe->write_refs++;
}

static void process_fd_pty_ref(fd_entry_t* ent) {
    pty_object_t* pty;

    if (!ent || !ent->valid) return;
    pty = pty_object_from_ent(ent);
    if (!pty) return;
    if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER) pty->master_refs++;
    if (ent->kind == PROCESS_HANDLE_KIND_PTY_SLAVE) pty->slave_refs++;
}

static void process_fd_share_ref(fd_entry_t* ent) {
    if (!ent || !ent->valid) return;
    if (ent->kind == PROCESS_HANDLE_KIND_PIPE) {
        process_fd_pipe_ref(ent);
    } else if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER ||
               ent->kind == PROCESS_HANDLE_KIND_PTY_SLAVE) {
        process_fd_pty_ref(ent);
    } else if (ent->kind == PROCESS_HANDLE_KIND_SOCKET) {
        socket_retain(ent->socket);
    } else if (ent->kind == PROCESS_HANDLE_KIND_FILE) {
        vfs_file_retain(ent);
    } else if (ent->kind == PROCESS_HANDLE_KIND_EPOLL ||
               ent->kind == PROCESS_HANDLE_KIND_TIMERFD ||
               ent->kind == PROCESS_HANDLE_KIND_SIGNALFD) {
        ent->aux_frame = 0;
    }
}

int process_fd_pipe(process_t* proc, int fds[2], unsigned int flags) {
    fd_entry_t* read_ent;
    fd_entry_t* write_ent;
    pipe_object_t* pipe;
    u32 pipe_frame;
    u32 data_frame;
    int read_fd;
    int write_fd;

    if (!proc || !fds) return -EINVAL;
    if ((flags & ~(SYS_FD_FLAG_NONBLOCK | SYS_FD_FLAG_CLOEXEC)) != 0u) {
        return -EINVAL;
    }

    pipe_frame = pmm_alloc_frame();
    if (!pipe_frame) return -ENOMEM;
    data_frame = pmm_alloc_frame();
    if (!data_frame) {
        pmm_free_frame(pipe_frame);
        return -ENOMEM;
    }

    pipe = (pipe_object_t*)paging_phys_to_kernel_virt(pipe_frame);
    k_memset(pipe, 0, PAGE_SIZE);
    k_memset(paging_phys_to_kernel_virt(data_frame), 0, PAGE_SIZE);
    wait_queue_init(&pipe->read_waiters);
    wait_queue_init(&pipe->write_waiters);
    pipe->data_frame = data_frame;
    pipe->read_refs = 1;
    pipe->write_refs = 1;

    read_fd = process_fd_alloc_entry(proc, &read_ent);
    if (read_fd < 0) {
        pmm_free_frame(data_frame);
        pmm_free_frame(pipe_frame);
        return read_fd;
    }
    read_ent->valid = 1;

    write_fd = process_fd_alloc_entry(proc, &write_ent);
    if (write_fd < 0) {
        k_memset(read_ent, 0, sizeof(*read_ent));
        pmm_free_frame(data_frame);
        pmm_free_frame(pipe_frame);
        return write_fd;
    }

    k_memset(read_ent, 0, sizeof(*read_ent));
    read_ent->valid = 1;
    read_ent->kind = PROCESS_HANDLE_KIND_PIPE;
    read_ent->ops = &s_pipe_handle_ops;
    read_ent->readable = 1;
    read_ent->writable = 0;
    read_ent->flags = flags & SYS_FD_FLAG_NONBLOCK;
    read_ent->fd_flags = flags & SYS_FD_FLAG_CLOEXEC;
    read_ent->aux_frame = pipe_frame;
    k_memcpy(read_ent->name, "pipe:r", 7);

    k_memset(write_ent, 0, sizeof(*write_ent));
    write_ent->valid = 1;
    write_ent->kind = PROCESS_HANDLE_KIND_PIPE;
    write_ent->ops = &s_pipe_handle_ops;
    write_ent->readable = 0;
    write_ent->writable = 1;
    write_ent->flags = flags & SYS_FD_FLAG_NONBLOCK;
    write_ent->fd_flags = flags & SYS_FD_FLAG_CLOEXEC;
    write_ent->aux_frame = pipe_frame;
    k_memcpy(write_ent->name, "pipe:w", 7);

    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}

int process_fd_pty(process_t* proc, int fds[2], unsigned int master_flags) {
    fd_entry_t* master_ent;
    fd_entry_t* slave_ent;
    pty_object_t* pty;
    u32 pty_frame;
    u32 m2s_frame;
    u32 s2m_frame;
    int master_fd;
    int slave_fd;

    if (!proc || !fds) return -EINVAL;
    if ((master_flags & ~(SYS_FD_FLAG_NONBLOCK | SYS_FD_FLAG_CLOEXEC)) != 0u) {
        return -EINVAL;
    }

    pty_frame = pmm_alloc_frame();
    if (!pty_frame) return -ENOMEM;
    m2s_frame = pmm_alloc_frame();
    if (!m2s_frame) {
        pmm_free_frame(pty_frame);
        return -ENOMEM;
    }
    s2m_frame = pmm_alloc_frame();
    if (!s2m_frame) {
        pmm_free_frame(m2s_frame);
        pmm_free_frame(pty_frame);
        return -ENOMEM;
    }

    pty = (pty_object_t*)paging_phys_to_kernel_virt(pty_frame);
    k_memset(pty, 0, PAGE_SIZE);
    k_memset(paging_phys_to_kernel_virt(m2s_frame), 0, PAGE_SIZE);
    k_memset(paging_phys_to_kernel_virt(s2m_frame), 0, PAGE_SIZE);
    wait_queue_init(&pty->master_to_slave.read_waiters);
    wait_queue_init(&pty->master_to_slave.write_waiters);
    wait_queue_init(&pty->slave_to_master.read_waiters);
    wait_queue_init(&pty->slave_to_master.write_waiters);
    pty->master_to_slave.data_frame = m2s_frame;
    pty->slave_to_master.data_frame = s2m_frame;
    pty->master_refs = 1;
    pty->slave_refs = 1;
    pty->rows = 25;
    pty->cols = 80;

    master_fd = process_fd_alloc_entry(proc, &master_ent);
    if (master_fd < 0) {
        pmm_free_frame(s2m_frame);
        pmm_free_frame(m2s_frame);
        pmm_free_frame(pty_frame);
        return master_fd;
    }
    master_ent->valid = 1;

    slave_fd = process_fd_alloc_entry(proc, &slave_ent);
    if (slave_fd < 0) {
        k_memset(master_ent, 0, sizeof(*master_ent));
        pmm_free_frame(s2m_frame);
        pmm_free_frame(m2s_frame);
        pmm_free_frame(pty_frame);
        return slave_fd;
    }

    k_memset(master_ent, 0, sizeof(*master_ent));
    master_ent->valid = 1;
    master_ent->kind = PROCESS_HANDLE_KIND_PTY_MASTER;
    master_ent->ops = &s_pty_handle_ops;
    master_ent->readable = 1;
    master_ent->writable = 1;
    master_ent->flags = master_flags & SYS_FD_FLAG_NONBLOCK;
    master_ent->fd_flags = master_flags & SYS_FD_FLAG_CLOEXEC;
    master_ent->aux_frame = pty_frame;
    k_memcpy(master_ent->name, "pty:master", 11);

    k_memset(slave_ent, 0, sizeof(*slave_ent));
    slave_ent->valid = 1;
    slave_ent->kind = PROCESS_HANDLE_KIND_PTY_SLAVE;
    slave_ent->ops = &s_pty_handle_ops;
    slave_ent->readable = 1;
    slave_ent->writable = 1;
    slave_ent->aux_frame = pty_frame;
    k_memcpy(slave_ent->name, "pty:slave", 10);

    fds[0] = master_fd;
    fds[1] = slave_fd;
    return 0;
}

int process_fd_pty_set_size(fd_entry_t* ent, unsigned int rows, unsigned int cols) {
    pty_object_t* pty = pty_object_from_ent(ent);
    if (!pty) return -ENOTTY;
    if (rows == 0u || cols == 0u || rows > 200u || cols > 240u) return -EINVAL;
    pty->rows = rows;
    pty->cols = cols;
    return 0;
}

int process_fd_terminal_size(fd_entry_t* ent, unsigned int* out_rows, unsigned int* out_cols) {
    pty_object_t* pty = pty_object_from_ent(ent);
    if (!pty || !out_rows || !out_cols) return -ENOTTY;
    *out_rows = pty->rows ? pty->rows : 25u;
    *out_cols = pty->cols ? pty->cols : 80u;
    return 0;
}

int process_fd_pty_set_foreground(fd_entry_t* ent, u32 pgid) {
    pty_object_t* pty = pty_object_from_ent(ent);
    if (!pty) return -ENOTTY;
    pty->foreground_pgid = pgid;
    return 0;
}

u32 process_fd_pty_get_foreground(fd_entry_t* ent) {
    pty_object_t* pty = pty_object_from_ent(ent);
    return pty ? pty->foreground_pgid : 0u;
}

int process_fd_dup(process_t* proc, int oldfd, int minfd, unsigned int fd_flags) {
    fd_entry_t* old_ent;
    fd_entry_t* new_ent;
    int new_fd;

    if (!proc) return -EINVAL;
    if (minfd < 0 || (unsigned int)minfd >= proc->fd_limit) return -EBADF;
    old_ent = process_fd_get(proc, oldfd);
    if (!old_ent) return -EBADF;
    if ((fd_flags & ~SYS_FD_FLAG_CLOEXEC) != 0u) return -EINVAL;

    for (;;) {
        while ((unsigned int)minfd >= proc->fd_capacity) {
            int rc = process_fd_table_grow(proc, (unsigned int)minfd + 1u);
            if (rc < 0) return rc;
        }
        for (unsigned int fd = (unsigned int)minfd; fd < proc->fd_capacity; fd++) {
            if (!proc->fds[fd].valid) {
                new_fd = (int)fd;
                new_ent = &proc->fds[fd];
                *new_ent = *old_ent;
                new_ent->fd_flags = fd_flags;
                process_fd_share_ref(new_ent);
                return new_fd;
            }
        }
        if (proc->fd_capacity >= proc->fd_limit) return -ENFILE;
        minfd = (int)proc->fd_capacity;
    }
}

int process_fd_dup2(process_t* proc, int oldfd, int newfd, unsigned int fd_flags, int reject_same) {
    fd_entry_t* old_ent;
    fd_entry_t* new_ent;
    int rc;

    if (!proc) return -EINVAL;
    if ((fd_flags & ~SYS_FD_FLAG_CLOEXEC) != 0u) return -EINVAL;
    old_ent = process_fd_get(proc, oldfd);
    if (!old_ent) return -EBADF;
    if (newfd < 0 || (unsigned int)newfd >= proc->fd_limit) return -EBADF;
    if (oldfd == newfd) {
        return reject_same ? -EINVAL : newfd;
    }

    rc = process_fd_alloc_exact(proc, newfd, &new_ent);
    if (rc < 0) return rc;
    *new_ent = *old_ent;
    new_ent->fd_flags = fd_flags;
    process_fd_share_ref(new_ent);
    return newfd;
}

int process_fd_dup_from(process_t* dst, int newfd, process_t* src, int oldfd, unsigned int fd_flags) {
    fd_entry_t* old_ent;
    fd_entry_t* new_ent;
    int rc;

    if (!dst || !src) return -EINVAL;
    if ((fd_flags & ~SYS_FD_FLAG_CLOEXEC) != 0u) return -EINVAL;
    old_ent = process_fd_get(src, oldfd);
    if (!old_ent) return -EBADF;
    if (newfd < 0 || (unsigned int)newfd >= dst->fd_limit) return -EBADF;

    rc = process_fd_alloc_exact(dst, newfd, &new_ent);
    if (rc < 0) return rc;
    *new_ent = *old_ent;
    new_ent->fd_flags = fd_flags;
    process_fd_share_ref(new_ent);
    return newfd;
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
    if (socket_state(sock) != SOCKET_STATE_CONNECTED &&
        socket_state(sock) != SOCKET_STATE_CONNECTING) return -EINVAL;
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
            if (!socket_tcp_connect_pending(sock)) {
                __asm__ __volatile__("cli");
                return 0;
            }

            if (!proc) {
                __asm__ __volatile__("sti; hlt; cli");
                continue;
            }

            proc->state = PROCESS_STATE_WAITING;
            wait_rc = socket_wait(sock, proc, POLLOUT);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                socket_wait_clear_process(proc);
                return wait_rc;
            }
            __asm__ __volatile__("sti");
            while (proc->state != PROCESS_STATE_RUNNING) {
                __asm__ __volatile__("hlt");
            }
            __asm__ __volatile__("cli");
            socket_wait_clear_process(proc);
            continue;
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
    if (socket_state(sock) != SOCKET_STATE_CONNECTED &&
        socket_state(sock) != SOCKET_STATE_CONNECTING) return -EINVAL;
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
                if (rc == -EPIPE && proc) {
                    (void)process_signal_deliver(proc, PROCESS_SIGPIPE);
                }
                return rc;
            }
            return -ECONNRESET;
        }

        if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            return done ? (int)done : -EAGAIN;
        }

        if (!socket_tcp_connection_established(sock)) {
            if (!socket_tcp_connect_pending(sock)) {
                return done ? (int)done : -ECONNRESET;
            }
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

    while (n < len) {
        for (;;) {
            __asm__ volatile ("cli");
            if (keyboard_buf_available()) {
                break;
            }
            if (proc) {
                proc->state = PROCESS_STATE_WAITING;
                keyboard_set_waiting_process(proc);
            }
            __asm__ volatile ("sti; hlt");
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

    terminal_write(buf, len);
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

static int process_handle_pipe_read(fd_entry_t* ent, char* buf, unsigned int len) {
    pipe_object_t* pipe;
    char* data;
    process_t* proc;
    unsigned int done = 0u;

    if (!ent || !ent->valid || !ent->readable) return -EBADF;
    if (!buf) return -EFAULT;
    if (len == 0) return 0;
    pipe = pipe_object_from_ent(ent);
    data = pipe_data(pipe);
    if (!pipe || !data) return -EIO;

    proc = sched_current();
    while (pipe->count == 0u && pipe->write_refs != 0u) {
        int wait_rc;
        if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            return -EAGAIN;
        }
        if (!proc) {
            __asm__ __volatile__("sti; hlt; cli");
            continue;
        }
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = wait_queue_add(&pipe->read_waiters, proc);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            wait_queue_remove_proc(proc);
            return wait_rc;
        }
        if (pipe->count != 0u || pipe->write_refs == 0u) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        __asm__ __volatile__("sti");
        while (proc->state != PROCESS_STATE_RUNNING) {
            __asm__ __volatile__("hlt");
        }
        __asm__ __volatile__("cli");
        wait_queue_remove_proc(proc);
    }
    wait_queue_remove_proc(proc);

    while (done < len && pipe->count != 0u) {
        buf[done++] = data[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1u) % PIPE_BUFFER_SIZE;
        pipe->count--;
    }

    if (done != 0u) {
        wait_queue_wake_all(&pipe->write_waiters);
    }
    return (int)done;
}

static int process_handle_pipe_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    pipe_object_t* pipe;
    char* data;
    process_t* proc;
    unsigned int done = 0u;
    unsigned int atomic_len;

    if (!ent || !ent->valid || !ent->writable) return -EBADF;
    if (!buf) return -EFAULT;
    if (len == 0) return 0;
    pipe = pipe_object_from_ent(ent);
    data = pipe_data(pipe);
    if (!pipe || !data) return -EIO;

    proc = sched_current();
    atomic_len = (len <= SYS_PIPE_BUF) ? len : 0u;
    while (done < len) {
        unsigned int needed = atomic_len ? atomic_len : 1u;
        if (pipe->read_refs == 0u) {
            if (done != 0u) return (int)done;
            if (proc) (void)process_signal_deliver(proc, PROCESS_SIGPIPE);
            return -EPIPE;
        }

        while ((PIPE_BUFFER_SIZE - pipe->count) < needed && pipe->read_refs != 0u) {
            int wait_rc;
            if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
                return done ? (int)done : -EAGAIN;
            }
            if (!proc) {
                __asm__ __volatile__("sti; hlt; cli");
                continue;
            }
            proc->state = PROCESS_STATE_WAITING;
            wait_rc = wait_queue_add(&pipe->write_waiters, proc);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                wait_queue_remove_proc(proc);
                return done ? (int)done : wait_rc;
            }
            if ((PIPE_BUFFER_SIZE - pipe->count) >= needed || pipe->read_refs == 0u) {
                proc->state = PROCESS_STATE_RUNNING;
                break;
            }
            __asm__ __volatile__("sti");
            while (proc->state != PROCESS_STATE_RUNNING) {
                __asm__ __volatile__("hlt");
            }
            __asm__ __volatile__("cli");
            wait_queue_remove_proc(proc);
        }
        wait_queue_remove_proc(proc);

        if (pipe->read_refs == 0u) {
            if (done != 0u) return (int)done;
            if (proc) (void)process_signal_deliver(proc, PROCESS_SIGPIPE);
            return -EPIPE;
        }

        while (done < len && pipe->count < PIPE_BUFFER_SIZE) {
            data[pipe->write_pos] = buf[done++];
            pipe->write_pos = (pipe->write_pos + 1u) % PIPE_BUFFER_SIZE;
            pipe->count++;
        }
        wait_queue_wake_all(&pipe->read_waiters);

        if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            break;
        }
    }

    return (int)done;
}

static int process_handle_pipe_seek(fd_entry_t* ent, int offset, int whence) {
    (void)ent;
    (void)offset;
    (void)whence;
    return -EINVAL;
}

static short process_handle_pipe_poll(fd_entry_t* ent, short events) {
    pipe_object_t* pipe;
    short revents = 0;

    if (!ent || !ent->valid) return POLLERR;
    pipe = pipe_object_from_ent(ent);
    if (!pipe) return POLLERR;

    if ((events & POLLIN) && ent->readable &&
        (pipe->count != 0u || pipe->write_refs == 0u)) {
        revents |= POLLIN;
    }
    if ((events & POLLOUT) && ent->writable &&
        pipe->read_refs != 0u && pipe->count < PIPE_BUFFER_SIZE) {
        revents |= POLLOUT;
    }
    if ((ent->readable && pipe->write_refs == 0u) ||
        (ent->writable && pipe->read_refs == 0u)) {
        revents |= POLLHUP;
    }
    return revents;
}

static void process_handle_pipe_close(fd_entry_t* ent) {
    pipe_object_t* pipe;
    u32 pipe_frame;
    u32 data_frame;

    if (!ent) return;
    pipe = pipe_object_from_ent(ent);
    pipe_frame = ent->aux_frame;
    if (!pipe) {
        k_memset(ent, 0, sizeof(*ent));
        return;
    }

    if (ent->readable && pipe->read_refs > 0u) pipe->read_refs--;
    if (ent->writable && pipe->write_refs > 0u) pipe->write_refs--;
    wait_queue_wake_all(&pipe->read_waiters);
    wait_queue_wake_all(&pipe->write_waiters);

    data_frame = pipe->data_frame;
    if (pipe->read_refs == 0u && pipe->write_refs == 0u) {
        if (data_frame) pmm_free_frame(data_frame);
        if (pipe_frame) pmm_free_frame(pipe_frame);
    }
    k_memset(ent, 0, sizeof(*ent));
}

static int pty_buffer_read(pty_buffer_t* buffer,
                           char* buf,
                           unsigned int len,
                           unsigned int* writer_refs,
                           unsigned int flags) {
    char* data;
    process_t* proc;
    unsigned int done = 0u;

    if (!buffer || !buf) return -EFAULT;
    if (len == 0) return 0;
    data = pty_buffer_data(buffer);
    if (!data) return -EIO;

    proc = sched_current();
    while (buffer->count == 0u && (!writer_refs || *writer_refs != 0u)) {
        int wait_rc;
        if ((flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            return -EAGAIN;
        }
        if (!proc) {
            __asm__ __volatile__("sti; hlt; cli");
            continue;
        }
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = wait_queue_add(&buffer->read_waiters, proc);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            wait_queue_remove_proc(proc);
            return wait_rc;
        }
        if (buffer->count != 0u || (writer_refs && *writer_refs == 0u)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        __asm__ __volatile__("sti");
        while (proc->state != PROCESS_STATE_RUNNING) {
            __asm__ __volatile__("hlt");
        }
        __asm__ __volatile__("cli");
        wait_queue_remove_proc(proc);
    }
    wait_queue_remove_proc(proc);

    while (done < len && buffer->count != 0u) {
        buf[done++] = data[buffer->read_pos];
        buffer->read_pos = (buffer->read_pos + 1u) % PIPE_BUFFER_SIZE;
        buffer->count--;
    }
    if (done != 0u) wait_queue_wake_all(&buffer->write_waiters);
    return (int)done;
}

static int pty_buffer_write(pty_buffer_t* buffer,
                            const char* buf,
                            unsigned int len,
                            unsigned int* reader_refs,
                            unsigned int flags) {
    char* data;
    process_t* proc;
    unsigned int done = 0u;

    if (!buffer || !buf) return -EFAULT;
    if (len == 0) return 0;
    data = pty_buffer_data(buffer);
    if (!data) return -EIO;

    proc = sched_current();
    while (done < len) {
        if (reader_refs && *reader_refs == 0u) {
            return done ? (int)done : -EIO;
        }

        while (buffer->count >= PIPE_BUFFER_SIZE && (!reader_refs || *reader_refs != 0u)) {
            int wait_rc;
            if ((flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
                return done ? (int)done : -EAGAIN;
            }
            if (!proc) {
                __asm__ __volatile__("sti; hlt; cli");
                continue;
            }
            proc->state = PROCESS_STATE_WAITING;
            wait_rc = wait_queue_add(&buffer->write_waiters, proc);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                wait_queue_remove_proc(proc);
                return done ? (int)done : wait_rc;
            }
            if (buffer->count < PIPE_BUFFER_SIZE || (reader_refs && *reader_refs == 0u)) {
                proc->state = PROCESS_STATE_RUNNING;
                break;
            }
            __asm__ __volatile__("sti");
            while (proc->state != PROCESS_STATE_RUNNING) {
                __asm__ __volatile__("hlt");
            }
            __asm__ __volatile__("cli");
            wait_queue_remove_proc(proc);
        }
        wait_queue_remove_proc(proc);

        if (reader_refs && *reader_refs == 0u) return done ? (int)done : -EIO;

        while (done < len && buffer->count < PIPE_BUFFER_SIZE) {
            data[buffer->write_pos] = buf[done++];
            buffer->write_pos = (buffer->write_pos + 1u) % PIPE_BUFFER_SIZE;
            buffer->count++;
        }
        wait_queue_wake_all(&buffer->read_waiters);

        if ((flags & SYS_FD_FLAG_NONBLOCK) != 0u) break;
    }
    return (int)done;
}

static int process_handle_pty_read_common(fd_entry_t* ent,
                                          char* buf,
                                          unsigned int len,
                                          int echo) {
    pty_object_t* pty;
    pty_buffer_t* in;
    pty_buffer_t* out;
    unsigned int done = 0u;

    if (!ent || !ent->valid || !ent->readable) return -EBADF;
    if (!buf) return -EFAULT;
    if (len == 0) return 0;
    pty = pty_object_from_ent(ent);
    if (!pty) return -EIO;

    if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER) {
        return pty_buffer_read(&pty->slave_to_master, buf, len,
                               &pty->slave_refs, ent->flags);
    }

    in = &pty->master_to_slave;
    out = &pty->slave_to_master;
    while (done < len) {
        int rc = pty_buffer_read(in, &buf[done], 1u, &pty->master_refs, ent->flags);
        if (rc <= 0) return done ? (int)done : rc;
        if (echo) {
            (void)pty_buffer_write(out, &buf[done], 1u, &pty->master_refs, 0);
        }
        done++;
        if (buf[done - 1] == '\n') break;
    }
    return (int)done;
}

static int process_handle_pty_read(fd_entry_t* ent, char* buf, unsigned int len) {
    return process_handle_pty_read_common(ent, buf, len, 1);
}

static int process_handle_pty_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    pty_object_t* pty;

    if (!ent || !ent->valid || !ent->writable) return -EBADF;
    if (!buf) return -EFAULT;
    if (len == 0) return 0;
    pty = pty_object_from_ent(ent);
    if (!pty) return -EIO;

    if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER) {
        for (unsigned int i = 0; i < len; i++) {
            u32 interrupt_pgid = pty->foreground_pgid
                               ? pty->foreground_pgid
                               : s_foreground_pgid;
            if ((unsigned char)buf[i] == 3u && interrupt_pgid != 0u) {
                char interrupt_text[] = "^C\n";
                if (!process_group_signal_deliver(interrupt_pgid, PROCESS_SIGINT)) {
                    (void)process_group_kill(interrupt_pgid,
                                             PROCESS_TERMINATED_BY_CTRL_C);
                }
                (void)pty_buffer_write(&pty->slave_to_master,
                                       interrupt_text,
                                       sizeof(interrupt_text) - 1u,
                                       &pty->master_refs,
                                       0);
                if (i > 0) {
                    return (int)i;
                }
                return 1;
            }
        }
        return pty_buffer_write(&pty->master_to_slave, buf, len,
                                &pty->slave_refs, ent->flags);
    }
    return pty_buffer_write(&pty->slave_to_master, buf, len,
                            &pty->master_refs, ent->flags);
}

static int process_handle_pty_seek(fd_entry_t* ent, int offset, int whence) {
    (void)ent;
    (void)offset;
    (void)whence;
    return -EINVAL;
}

static short process_handle_pty_poll(fd_entry_t* ent, short events) {
    pty_object_t* pty;
    pty_buffer_t* read_buf;
    pty_buffer_t* write_buf;
    unsigned int peer_refs;
    short revents = 0;

    if (!ent || !ent->valid) return POLLERR;
    pty = pty_object_from_ent(ent);
    if (!pty) return POLLERR;

    if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER) {
        read_buf = &pty->slave_to_master;
        write_buf = &pty->master_to_slave;
        peer_refs = pty->slave_refs;
    } else {
        read_buf = &pty->master_to_slave;
        write_buf = &pty->slave_to_master;
        peer_refs = pty->master_refs;
    }

    if ((events & POLLIN) && ent->readable &&
        (read_buf->count != 0u || peer_refs == 0u)) {
        revents |= POLLIN;
    }
    if ((events & POLLOUT) && ent->writable &&
        peer_refs != 0u && write_buf->count < PIPE_BUFFER_SIZE) {
        revents |= POLLOUT;
    }
    if (peer_refs == 0u) revents |= POLLHUP;
    return revents;
}

static void process_handle_pty_close(fd_entry_t* ent) {
    pty_object_t* pty;
    u32 pty_frame;
    u32 m2s_frame;
    u32 s2m_frame;

    if (!ent) return;
    pty = pty_object_from_ent(ent);
    pty_frame = ent->aux_frame;
    if (!pty) {
        k_memset(ent, 0, sizeof(*ent));
        return;
    }

    if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER && pty->master_refs > 0u) {
        pty->master_refs--;
    }
    if (ent->kind == PROCESS_HANDLE_KIND_PTY_SLAVE && pty->slave_refs > 0u) {
        pty->slave_refs--;
    }
    wait_queue_wake_all(&pty->master_to_slave.read_waiters);
    wait_queue_wake_all(&pty->master_to_slave.write_waiters);
    wait_queue_wake_all(&pty->slave_to_master.read_waiters);
    wait_queue_wake_all(&pty->slave_to_master.write_waiters);

    m2s_frame = pty->master_to_slave.data_frame;
    s2m_frame = pty->slave_to_master.data_frame;
    if (pty->master_refs == 0u && pty->slave_refs == 0u) {
        if (m2s_frame) pmm_free_frame(m2s_frame);
        if (s2m_frame) pmm_free_frame(s2m_frame);
        if (pty_frame) pmm_free_frame(pty_frame);
    }
    k_memset(ent, 0, sizeof(*ent));
}

static void process_handle_socket_close(fd_entry_t* ent) {
    if (!ent) return;
    socket_release(ent->socket);
    k_memset(ent, 0, sizeof(*ent));
}

static int special_wait_object_kind(int kind) {
    return kind == PROCESS_HANDLE_KIND_TIMERFD ||
           kind == PROCESS_HANDLE_KIND_SIGNALFD;
}

static special_wait_object_t* special_wait_object(fd_entry_t* ent, int create) {
    special_wait_object_t* obj;

    if (!ent || !special_wait_object_kind(ent->kind)) return 0;

    if (!ent->aux_frame && create) {
        ent->aux_frame = pmm_alloc_frame();
        if (!ent->aux_frame) return 0;
        obj = (special_wait_object_t*)paging_phys_to_kernel_virt(ent->aux_frame);
        k_memset(obj, 0, PAGE_SIZE);
        wait_queue_init(&obj->read_waiters);
        return obj;
    }

    if (!ent->aux_frame) return 0;
    return (special_wait_object_t*)paging_phys_to_kernel_virt(ent->aux_frame);
}

static int special_wait_readable(fd_entry_t* ent, process_t* proc) {
    special_wait_object_t* obj;

    if (!ent || !proc) return -EINVAL;
    obj = special_wait_object(ent, 1);
    if (!obj) return -ENOMEM;
    return wait_queue_add(&obj->read_waiters, proc);
}

static unsigned int signal_bit(int signum) {
    if (signum <= 0 || signum >= 32) return 0u;
    return 1u << (unsigned int)signum;
}

static int signalfd_ready(fd_entry_t* ent) {
    special_wait_object_t* obj;

    if (!ent || ent->kind != PROCESS_HANDLE_KIND_SIGNALFD) return 0;
    obj = special_wait_object(ent, 0);
    return obj && (obj->pending_signals & obj->signal_mask) != 0u;
}

static int signalfd_consume(fd_entry_t* ent, kernel_signalfd_siginfo_t* out) {
    special_wait_object_t* obj;
    unsigned int pending;

    if (!ent || !out || ent->kind != PROCESS_HANDLE_KIND_SIGNALFD) return -EINVAL;
    obj = special_wait_object(ent, 0);
    if (!obj) return -EAGAIN;

    pending = obj->pending_signals & obj->signal_mask;
    if (pending == 0u) return -EAGAIN;

    for (unsigned int signum = 1u; signum < 32u; signum++) {
        unsigned int bit = 1u << signum;
        if ((pending & bit) == 0u) continue;

        obj->pending_signals &= ~bit;
        k_memset(out, 0, sizeof(*out));
        out->ssi_signo = signum;
        return 0;
    }

    return -EAGAIN;
}

static int timerfd_ready_at(fd_entry_t* ent, unsigned int now) {
    if (!ent || ent->kind != PROCESS_HANDLE_KIND_TIMERFD) return 0;
    return ent->timer_deadline != 0u &&
           (int)(now - ent->timer_deadline) >= 0;
}

static int timerfd_ready(fd_entry_t* ent) {
    return timerfd_ready_at(ent, timer_get_ticks());
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
    process_t* proc;

    if (!ent || !ent->valid) return -EBADF;
    if (!buf) return -EFAULT;

    if (ent->kind == PROCESS_HANDLE_KIND_TIMERFD) {
        if (len < sizeof(unsigned long long)) return -EINVAL;

        if (!timerfd_ready(ent) &&
            (ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            return -EAGAIN;
        }

        proc = sched_current();
        while (!timerfd_ready(ent)) {
            int wait_rc;

            if (!proc) {
                __asm__ __volatile__("sti; hlt; cli");
                continue;
            }

            proc->state = PROCESS_STATE_WAITING;
            wait_rc = special_wait_readable(ent, proc);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                wait_queue_remove_proc(proc);
                return wait_rc;
            }
            if (timerfd_ready(ent)) {
                proc->state = PROCESS_STATE_RUNNING;
                break;
            }

            __asm__ __volatile__("sti");
            while (proc->state != PROCESS_STATE_RUNNING) {
                __asm__ __volatile__("hlt");
            }
            __asm__ __volatile__("cli");
            wait_queue_remove_proc(proc);
        }
        wait_queue_remove_proc(proc);

        expirations = timerfd_consume(ent);
        k_memcpy(buf, &expirations, sizeof(expirations));
        return (int)sizeof(expirations);
    }

    if (ent->kind == PROCESS_HANDLE_KIND_SIGNALFD) {
        kernel_signalfd_siginfo_t info;

        if (len < sizeof(info)) return -EINVAL;

        if (!signalfd_ready(ent) &&
            (ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            return -EAGAIN;
        }

        proc = sched_current();
        while (!signalfd_ready(ent)) {
            int wait_rc;

            if (!proc) {
                __asm__ __volatile__("sti; hlt; cli");
                continue;
            }

            proc->state = PROCESS_STATE_WAITING;
            wait_rc = special_wait_readable(ent, proc);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                wait_queue_remove_proc(proc);
                return wait_rc;
            }
            if (signalfd_ready(ent)) {
                proc->state = PROCESS_STATE_RUNNING;
                break;
            }

            __asm__ __volatile__("sti");
            while (proc->state != PROCESS_STATE_RUNNING) {
                __asm__ __volatile__("hlt");
            }
            __asm__ __volatile__("cli");
            wait_queue_remove_proc(proc);
        }
        wait_queue_remove_proc(proc);

        if (signalfd_consume(ent, &info) < 0) {
            return -EAGAIN;
        }
        k_memcpy(buf, &info, sizeof(info));
        return (int)sizeof(info);
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
        if ((events & POLLIN) && signalfd_ready(ent)) {
            revents |= POLLIN;
        }
        return revents;
    }

    return POLLERR;
}

static void process_handle_special_close(fd_entry_t* ent) {
    if (!ent) return;
    if (special_wait_object_kind(ent->kind)) {
        special_wait_object_t* obj = special_wait_object(ent, 0);
        if (obj) {
            wait_queue_wake_all(&obj->read_waiters);
        }
    }
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
    if (ent->kind == PROCESS_HANDLE_KIND_PTY_SLAVE) {
        return process_handle_pty_read_common(ent, buf, len, 0);
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

int process_fd_wait(fd_entry_t* ent, process_t* proc, short events) {
    int rc;

    if (!ent || !ent->valid) return -EBADF;
    if (!proc) return -EINVAL;

    if (ent->kind == PROCESS_HANDLE_KIND_SOCKET) {
        return socket_wait(ent->socket, proc, events);
    }

    if (special_wait_object_kind(ent->kind)) {
        if ((events & POLLIN) == 0) return 0;
        rc = special_wait_readable(ent, proc);
        if (rc < 0) {
            wait_queue_remove_proc(proc);
        }
        return rc;
    }

    if (ent->kind == PROCESS_HANDLE_KIND_PIPE) {
        pipe_object_t* pipe = pipe_object_from_ent(ent);
        if (!pipe) return -EIO;
        if ((events & POLLIN) && ent->readable) {
            rc = wait_queue_add(&pipe->read_waiters, proc);
            if (rc < 0) {
                wait_queue_remove_proc(proc);
                return rc;
            }
        }
        if ((events & POLLOUT) && ent->writable) {
            rc = wait_queue_add(&pipe->write_waiters, proc);
            if (rc < 0) {
                wait_queue_remove_proc(proc);
                return rc;
            }
        }
        return 0;
    }

    if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER ||
        ent->kind == PROCESS_HANDLE_KIND_PTY_SLAVE) {
        pty_object_t* pty = pty_object_from_ent(ent);
        pty_buffer_t* read_buf;
        pty_buffer_t* write_buf;
        if (!pty) return -EIO;
        if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER) {
            read_buf = &pty->slave_to_master;
            write_buf = &pty->master_to_slave;
        } else {
            read_buf = &pty->master_to_slave;
            write_buf = &pty->slave_to_master;
        }
        if ((events & POLLIN) && ent->readable) {
            rc = wait_queue_add(&read_buf->read_waiters, proc);
            if (rc < 0) {
                wait_queue_remove_proc(proc);
                return rc;
            }
        }
        if ((events & POLLOUT) && ent->writable) {
            rc = wait_queue_add(&write_buf->write_waiters, proc);
            if (rc < 0) {
                wait_queue_remove_proc(proc);
                return rc;
            }
        }
        return 0;
    }

    return 0;
}

int process_fd_seek(fd_entry_t* ent, int offset, int whence) {
    if (!ent || !ent->ops) return -EBADF;
    if (!ent->ops->seek) return -ENOSYS;
    return ent->ops->seek(ent, offset, whence);
}

int process_fd_flush(fd_entry_t* ent) {
    if (!ent || !ent->ops || !ent->ops->flush) return 1;
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

int process_fd_set_fd_flags(fd_entry_t* ent, unsigned int flags) {
    if (!ent || !ent->valid) return -EBADF;
    ent->fd_flags = flags & SYS_FD_FLAG_CLOEXEC;
    return 0;
}

unsigned int process_fd_get_fd_flags(fd_entry_t* ent) {
    if (!ent || !ent->valid) return 0;
    return ent->fd_flags;
}

void process_close_cloexec_fds(process_t* proc) {
    if (!proc || !proc->fds) return;
    for (unsigned int i = 0; i < proc->fd_capacity; i++) {
        fd_entry_t* ent = &proc->fds[i];
        if (ent->valid && (ent->fd_flags & SYS_FD_FLAG_CLOEXEC) != 0u) {
            process_fd_close(ent);
        }
    }
}

int process_copy_fd_table(process_t* dst, process_t* src) {
    if (!dst || !src || !dst->fds || !src->fds) return -EINVAL;
    while (dst->fd_capacity < src->fd_capacity) {
        int rc = process_fd_table_grow(dst, src->fd_capacity);
        if (rc < 0) return rc;
    }

    for (unsigned int i = 0; i < dst->fd_capacity; i++) {
        if (dst->fds[i].valid) {
            process_fd_close(&dst->fds[i]);
        }
    }
    k_memset(dst->fds, 0, dst->fd_table_frames * PAGE_SIZE);

    for (unsigned int i = 0; i < src->fd_capacity; i++) {
        if (!src->fds[i].valid) continue;
        dst->fds[i] = src->fds[i];
        process_fd_share_ref(&dst->fds[i]);
    }
    return 0;
}

int process_fd_set_signalfd_mask(fd_entry_t* ent, unsigned int mask) {
    special_wait_object_t* obj;

    if (!ent || !ent->valid || ent->kind != PROCESS_HANDLE_KIND_SIGNALFD) {
        return -EBADF;
    }

    obj = special_wait_object(ent, 1);
    if (!obj) return -ENOMEM;
    obj->signal_mask = mask;
    obj->pending_signals &= mask;
    return 0;
}

void process_wake_timerfds(process_t* proc, unsigned int now) {
    if (!proc || !proc->fds) return;

    for (unsigned int i = 0; i < proc->fd_capacity; i++) {
        fd_entry_t* ent = &proc->fds[i];
        special_wait_object_t* obj;

        if (!ent->valid || ent->kind != PROCESS_HANDLE_KIND_TIMERFD) {
            continue;
        }
        if (!timerfd_ready_at(ent, now)) continue;

        obj = special_wait_object(ent, 0);
        if (!obj) continue;
        wait_queue_wake_all(&obj->read_waiters);
    }
}

void process_claim_for_wait(process_t* proc) {
    if (!proc) return;
    proc->reaper_claimed = 1;
}

int process_wait_pid(process_t* parent,
                     int pid,
                     int options,
                     int* out_pid,
                     int* out_status) {
    process_t* child;

    if (!parent || !out_pid || !out_status) return -EINVAL;
    if (pid == 0 || pid < -1) return -EINVAL;
    if ((options & ~1) != 0) return -EINVAL;

    child = process_find_child(parent, pid);
    if (!child) return -ECHILD;

    while (1) {
        child = process_find_zombie_child(parent, pid);
        if (child) {
            int child_pid = (int)child->pid;
            int status = child->exit_status;

            *out_pid = child_pid;
            *out_status = status;
            sched_dequeue(child);
            process_destroy(child);
            return 0;
        }

        if (options & 1) {
            *out_pid = 0;
            *out_status = 0;
            return 0;
        }

        __asm__ __volatile__("sti; hlt; cli");
    }
}

int process_kill_pid(int pid, int status, unsigned int esp) {
    process_t* proc;

    if (pid <= 0) return -EINVAL;

    proc = process_find_by_pid((u32)pid);
    if (!proc || proc->state == PROCESS_STATE_ZOMBIE ||
        proc->state == PROCESS_STATE_EXITED) {
        return -ESRCH;
    }
    if (!proc->pd) {
        return -EPERM;
    }

    proc->exit_status = status;
    if (keyboard_get_waiting_process() == (void*)proc) {
        keyboard_set_waiting_process(0);
    }
    input_forget_waiting_process(proc);
    socket_wait_clear_process(proc);

    if (proc == sched_current()) {
        paging_switch(paging_get_kernel_pd());
    }
    sched_kill(proc, esp);
    return 0;
}

int process_reap_unclaimed_zombies(void) {
    int reaped = 0;
    process_t* current = sched_current();

    for (int i = PROCESS_REGISTRY_MAX - 1; i >= 0; i--) {
        process_t* proc = s_process_registry[i];

        if (!proc) continue;
        if (proc == current) continue;
        if (proc->state != PROCESS_STATE_ZOMBIE) continue;
        if (proc->reaper_claimed) continue;

        sched_dequeue(proc);
        process_destroy(proc);
        reaped++;
    }

    return reaped;
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

int process_set_env(process_t* proc, int envc, char** envp) {
    unsigned int used = 0;

    if (!proc) return -EINVAL;
    if (envc < 0 || envc > PROCESS_MAX_ENVS) return -EINVAL;
    if (envc > 0 && !envp) return -EFAULT;

    proc->user_envc = 0;
    proc->user_envp[0] = 0;
    proc->user_env_data[0] = '\0';

    for (int i = 0; i < envc; i++) {
        if (!envp[i]) return -EFAULT;

        int len = k_strlen(envp[i]) + 1;
        if (used + (unsigned int)len > PROCESS_ENV_BYTES) {
            return -EINVAL;
        }

        proc->user_envp[i] = &proc->user_env_data[used];
        k_memcpy(proc->user_envp[i], envp[i], (k_size_t)len);
        used += (unsigned int)len;
    }

    proc->user_envc = envc;
    proc->user_envp[envc] = 0;
    return 0;
}

int process_set_default_env(process_t* proc) {
    char* envp[] = {
        "PATH=/bin:/usr/bin:/usr/sbin",
        "HOME=/",
        "SHELL=/bin/shell.elf",
        "TMPDIR=/tmp",
        0
    };

    return process_set_env(proc, 4, envp);
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

    __asm__ __volatile__("sti");
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

    proc->pid = s_next_pid++;
    if (s_next_pid == 0) {
        s_next_pid = 1;
    }
    {
        process_t* parent = sched_current();
        proc->parent_pid = parent ? parent->pid : 0;
    }
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
    (void)process_set_default_env(proc);
    if (name) {
        str_copy_n(proc->name, name, PROCESS_NAME_MAX);
    }

    if (!process_registry_add(proc)) {
        process_fd_table_free(proc);
        pmm_free_frame(frame);
        return 0;
    }

    return proc;
}

process_t* process_create_kernel_task(const char* name, void (*entry)(void)) {
    process_t* proc = process_create(name);
    if (!proc) return 0;

    proc->kernel_stack_frames = PROCESS_KERNEL_STACK_FRAMES;
    proc->kernel_stack_frame = pmm_alloc_contiguous_frames(proc->kernel_stack_frames);
    if (!proc->kernel_stack_frame) {
        terminal_puts("process: out of frames for kernel stack\n");
        process_destroy(proc);
        return 0;
    }
    k_memset(paging_phys_to_kernel_virt(proc->kernel_stack_frame),
             0,
             proc->kernel_stack_frames * PAGE_SIZE);

    proc->pd = 0;
    proc->kernel_entry = entry;
    proc->state = PROCESS_STATE_RUNNING;

    {
        unsigned int* stack_top =
            (unsigned int*)((u8*)paging_phys_to_kernel_virt(proc->kernel_stack_frame) +
                            proc->kernel_stack_frames * PAGE_SIZE);
        stack_top--;
        *stack_top = (unsigned int)process_kernel_task_bootstrap;
        proc->sched_esp = (unsigned int)stack_top;
    }

    return proc;
}

process_t* process_fork_from_syscall(unsigned int regs_esp, unsigned int frame_top) {
    process_t* parent = sched_current();
    process_t* child;
    unsigned int frame_bytes;
    unsigned int child_stack_top;
    unsigned int copy_start;
    unsigned int child_copy_start;
    unsigned int child_regs_esp;

    if (!parent || !parent->pd || !parent->kernel_stack_frame) return 0;
    if (regs_esp < 8u || regs_esp >= frame_top) return 0;

    child = process_create(parent->name);
    if (!child) return 0;

    child->pd = process_pd_clone_user(parent->pd);
    if (!child->pd) {
        process_destroy(child);
        return 0;
    }

    child->kernel_stack_frames = parent->kernel_stack_frames ? parent->kernel_stack_frames
                                                             : PROCESS_KERNEL_STACK_FRAMES;
    child->kernel_stack_frame = pmm_alloc_contiguous_frames(child->kernel_stack_frames);
    if (!child->kernel_stack_frame) {
        process_destroy(child);
        return 0;
    }
    k_memset(paging_phys_to_kernel_virt(child->kernel_stack_frame),
             0,
             child->kernel_stack_frames * PAGE_SIZE);

    copy_start = regs_esp - 8u;
    frame_bytes = frame_top - copy_start;
    if (frame_bytes > child->kernel_stack_frames * PAGE_SIZE) {
        process_destroy(child);
        return 0;
    }

    child_stack_top = (unsigned int)paging_phys_to_kernel_virt(child->kernel_stack_frame) +
                      child->kernel_stack_frames * PAGE_SIZE;
    child_copy_start = child_stack_top - frame_bytes;
    child_regs_esp = child_copy_start + 8u;
    k_memcpy((void*)child_copy_start, (const void*)copy_start, frame_bytes);
    ((unsigned int*)child_copy_start)[1] = child_regs_esp;
    ((unsigned int*)child_regs_esp)[11] = 0u; /* saved eax: fork returns 0 in child */
    child->sched_esp = child_copy_start;

    child->heap_base = parent->heap_base;
    child->heap_brk = parent->heap_brk;
    child->pgid = parent->pgid;
    child->user_entry = parent->user_entry;
    child->user_argc = parent->user_argc;
    k_memcpy(child->user_arg_data, parent->user_arg_data, sizeof(child->user_arg_data));
    for (int i = 0; i <= PROCESS_MAX_ARGS; i++) {
        if (parent->user_argv[i]) {
            unsigned int off = (unsigned int)(parent->user_argv[i] - parent->user_arg_data);
            child->user_argv[i] = child->user_arg_data + off;
        } else {
            child->user_argv[i] = 0;
        }
    }
    child->user_envc = parent->user_envc;
    k_memcpy(child->user_env_data, parent->user_env_data, sizeof(child->user_env_data));
    for (int i = 0; i <= PROCESS_MAX_ENVS; i++) {
        if (parent->user_envp[i]) {
            unsigned int off = (unsigned int)(parent->user_envp[i] - parent->user_env_data);
            child->user_envp[i] = child->user_env_data + off;
        } else {
            child->user_envp[i] = 0;
        }
    }
    k_memcpy(child->cwd, parent->cwd, sizeof(child->cwd));

    if (process_copy_fd_table(child, parent) < 0) {
        process_destroy(child);
        return 0;
    }

    child->state = PROCESS_STATE_RUNNING;
    if (!sched_enqueue(child)) {
        process_destroy(child);
        return 0;
    }

    return child;
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
    input_forget_waiting_process(proc);
    socket_wait_clear_process(proc);
    wait_queue_remove_proc(proc);
    display_release(proc);

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
        u32 frames = proc->kernel_stack_frames ? proc->kernel_stack_frames : 1u;
        pmm_free_contiguous_frames(proc->kernel_stack_frame, frames);
        proc->kernel_stack_frame = 0;
        proc->kernel_stack_frames = 0;
    }

    if (s_foreground_reader == proc) {
        s_foreground_reader = 0;
        s_foreground_pgid = 0;
    }
    if (s_terminal_interrupt_target == proc) {
        s_terminal_interrupt_target = 0;
        s_terminal_interrupt_pending = 0;
    }

    process_orphan_children(proc->pid);
    process_registry_remove(proc);

    proc->state = PROCESS_STATE_EXITED;
    pmm_free_frame(paging_kernel_virt_to_phys(proc));
}

process_t* process_get_current(void) {
    return sched_current();
}

void process_init_user_group(process_t* proc) {
    process_t* parent;

    if (!proc) return;

    parent = sched_current();
    if (parent && parent->pgid != 0) {
        proc->pgid = parent->pgid;
    } else {
        proc->pgid = proc->pid;
    }
}

void process_set_foreground(process_t* proc) {
    s_foreground_reader = proc;
    s_foreground_pgid = proc ? proc->pgid : 0;
    s_detach_requested = 0;
    keyboard_reset_modifiers();
    if (proc) {
        keyboard_buf_clear();           /* discard any input that arrived before
                                           the process was ready to read it,
                                           e.g. the Enter that launched runelf */
        input_clear_events();
        keyboard_set_consumer(process_key_consumer);
    } else {
        keyboard_set_consumer(process_key_consumer);
    }
}

void process_set_foreground_preserve_input(process_t* proc) {
    s_foreground_reader = proc;
    s_foreground_pgid = proc ? proc->pgid : 0;
    s_detach_requested = 0;
    keyboard_reset_modifiers();
    if (proc) {
        keyboard_set_consumer(process_key_consumer);
    } else {
        keyboard_set_consumer(process_key_consumer);
    }
}

process_t* process_get_foreground(void) {
    return s_foreground_reader;
}

u32 process_get_foreground_group(void) {
    return s_foreground_pgid;
}

int process_signal_deliver(process_t* proc, int signum) {
    unsigned int bit = signal_bit(signum);
    int delivered = 0;

    if (!proc || !proc->fds || bit == 0u) return 0;

    for (unsigned int i = 0; i < proc->fd_capacity; i++) {
        fd_entry_t* ent = &proc->fds[i];
        special_wait_object_t* obj;

        if (!ent->valid || ent->kind != PROCESS_HANDLE_KIND_SIGNALFD) {
            continue;
        }

        obj = special_wait_object(ent, 0);
        if (!obj || (obj->signal_mask & bit) == 0u) {
            continue;
        }

        obj->pending_signals |= bit;
        wait_queue_wake_all(&obj->read_waiters);
        delivered = 1;
    }

    return delivered;
}

int process_group_signal_deliver(u32 pgid, int signum) {
    process_t* targets[SCHED_MAX_PROCS];
    int count;
    int delivered = 0;

    if (pgid == 0) return 0;

    count = sched_snapshot_process_group(pgid, targets, SCHED_MAX_PROCS);
    for (int i = 0; i < count; i++) {
        if (process_signal_deliver(targets[i], signum)) {
            delivered = 1;
        }
    }

    return delivered;
}

static int process_group_force_exit(u32 pgid,
                                    int status,
                                    process_t* defer_current,
                                    int mark_terminal_interrupt) {
    process_t* targets[SCHED_MAX_PROCS];
    int count;
    int killed = 0;

    if (pgid == 0) return 0;

    count = sched_snapshot_process_group(pgid, targets, SCHED_MAX_PROCS);
    for (int i = 0; i < count; i++) {
        process_t* proc = targets[i];
        if (!proc || proc->state == PROCESS_STATE_ZOMBIE) {
            continue;
        }

        proc->exit_status = status;
        if (keyboard_get_waiting_process() == (void*)proc) {
            keyboard_set_waiting_process(0);
        }
        input_forget_waiting_process(proc);
        socket_wait_clear_process(proc);

        if (proc == defer_current && mark_terminal_interrupt) {
            s_terminal_interrupt_target = proc;
            s_terminal_interrupt_pending = 1;
        } else {
            sched_kill(proc, 0);
        }
        killed = 1;
    }

    return killed;
}

int process_group_kill(u32 pgid, int status) {
    return process_group_force_exit(pgid, status, 0, 0);
}

void process_deliver_pending_terminal_interrupt(unsigned int esp) {
    process_t* proc;

    if (!s_terminal_interrupt_pending) return;

    proc = s_terminal_interrupt_target ? s_terminal_interrupt_target : sched_current();
    s_terminal_interrupt_pending = 0;
    s_terminal_interrupt_target = 0;
    if (!proc || proc != sched_current()) return;

    proc->exit_status = PROCESS_TERMINATED_BY_CTRL_C;
    if (keyboard_get_waiting_process() == (void*)proc) {
        keyboard_set_waiting_process(0);
    }
    input_forget_waiting_process(proc);
    socket_wait_clear_process(proc);
    paging_switch(paging_get_kernel_pd());
    sched_kill(proc, esp);
}

static int process_wait_impl(process_t* proc, int allow_detach, int* detached) {
    if (!proc) return -1;

    if (detached) {
        *detached = 0;
    }
    s_detach_allowed = allow_detach ? 1 : 0;
    /*
     * bootseq may install a suspended shell as foreground before resuming it.
     * Preserve that handoff instead of clearing input a second time here.
     */
    if (process_get_foreground() == proc) {
        process_set_foreground_preserve_input(proc);
    } else {
        process_set_foreground(proc);
    }
    process_claim_for_wait(proc);

    while (proc->state != PROCESS_STATE_ZOMBIE) {
        if (allow_detach && s_detach_requested == proc) {
            s_detach_requested = 0;
            process_set_foreground(0);
            s_detach_allowed = 0;
            if (detached) {
                *detached = 1;
            }
            return 0;
        }
        __asm__ __volatile__("sti; hlt");
    }

    process_set_foreground(0);
    s_detach_allowed = 0;
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

int process_wait_restore_foreground(process_t* proc, process_t* restore_proc) {
    int status;

    if (!proc) return -1;

    s_detach_allowed = 0;
    /*
     * Match process_wait_impl(): callers may already have assigned foreground
     * ownership before entering the blocking wait.
     */
    if (process_get_foreground() == proc) {
        process_set_foreground_preserve_input(proc);
    } else {
        process_set_foreground(proc);
    }
    process_claim_for_wait(proc);

    while (proc->state != PROCESS_STATE_ZOMBIE) {
        __asm__ __volatile__("sti; hlt");
    }

    process_set_foreground_preserve_input(restore_proc);
    status = proc->exit_status;
    process_destroy(proc);
    return status;
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
