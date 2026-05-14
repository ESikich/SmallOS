#ifndef PROCESS_H
#define PROCESS_H

#include "paging.h"

/* ------------------------------------------------------------------ */
/* Process state                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    PROCESS_STATE_UNUSED  = 0,
    PROCESS_STATE_RUNNING = 1,
    PROCESS_STATE_EXITED  = 2,
    PROCESS_STATE_ZOMBIE  = 3,
    PROCESS_STATE_WAITING = 4,  /* blocked in SYS_READ, skipped by scheduler */
    PROCESS_STATE_SLEEPING = 5   /* blocked in SYS_SLEEP, woken by timer */
} process_state_t;

/* ------------------------------------------------------------------ */
/* Per-process descriptor handle table                                */
/* ------------------------------------------------------------------ */

/*
 * File descriptors 0, 1, 2 are reserved for stdin/stdout/stderr.
 * User-opened files start at fd 3. The fd table is PMM-backed process state:
 * it starts small and grows up to the per-process limit.
 */
#define PROCESS_FD_INITIAL_CAPACITY 16
#define PROCESS_FD_LIMIT_DEFAULT    128
#define PROCESS_FD_LIMIT_HARD       256
#define PROCESS_FD_MAX              PROCESS_FD_LIMIT_DEFAULT
#define PROCESS_FD_FIRST            3        /* first allocatable fd */
#define PROCESS_FD_NAME_MAX         128      /* max path length stored in fd entry */
#define PROCESS_FD_CACHE_PAGES      1024     /* 4 MB small-file read cache / 4 KB pages */

typedef struct fd_entry fd_entry_t;
typedef struct socket socket_t;

typedef enum {
    PROCESS_HANDLE_KIND_NONE  = 0,
    PROCESS_HANDLE_KIND_FILE  = 1,
    PROCESS_HANDLE_KIND_SOCKET = 2,
    PROCESS_HANDLE_KIND_CONSOLE = 3,
    PROCESS_HANDLE_KIND_EPOLL = 4,
    PROCESS_HANDLE_KIND_TIMERFD = 5,
    PROCESS_HANDLE_KIND_SIGNALFD = 6,
    PROCESS_HANDLE_KIND_PIPE = 7,
    PROCESS_HANDLE_KIND_PTY_MASTER = 8,
    PROCESS_HANDLE_KIND_PTY_SLAVE = 9
} process_handle_kind_t;

typedef enum {
    PROCESS_SOCKET_STATE_NONE      = 0,
    PROCESS_SOCKET_STATE_OPEN      = 1,
    PROCESS_SOCKET_STATE_BOUND     = 2,
    PROCESS_SOCKET_STATE_LISTENER  = 3,
    PROCESS_SOCKET_STATE_CONNECTING = 4,
    PROCESS_SOCKET_STATE_CONNECTED = 5
} process_socket_state_t;

typedef struct process_handle_ops {
    int  (*read)(fd_entry_t* ent, char* buf, unsigned int len);
    int  (*write)(fd_entry_t* ent, const char* buf, unsigned int len);
    int  (*seek)(fd_entry_t* ent, int offset, int whence);
    short (*poll)(fd_entry_t* ent, short events);
    int  (*flush)(fd_entry_t* ent);
    void (*close)(fd_entry_t* ent);
} process_handle_ops_t;

struct fd_entry {
    int  valid;                            /* 1 if this slot is open */
    int  kind;                             /* PROCESS_HANDLE_KIND_* */
    const process_handle_ops_t* ops;       /* resource-specific lifetime hooks */
    int  readable;                         /* 1 if reads are permitted */
    int  writable;                         /* 1 if writes should buffer */
    int  dirty;                            /* 1 if buffered file writes need flush */
    int  is_dir;                           /* 1 for directory descriptors */
    u32  flags;                            /* SYS_FD_FLAG_* descriptor status flags */
    u32  fd_flags;                         /* per-descriptor flags, e.g. FD_CLOEXEC */
    u32  socket_state;                     /* PROCESS_SOCKET_STATE_* */
    u32  socket_port;                      /* listener or peer port */
    u32  socket_conn;                      /* global accepted TCP stream id */
    socket_t* socket;                      /* kernel socket object for socket fds */
    u32  aux_frame;                        /* kind-specific PMM frame */
    u32  timer_deadline;                   /* timerfd next expiry tick, 0 if disarmed */
    u32  timer_interval;                   /* timerfd periodic interval in ticks */
    char name[PROCESS_FD_NAME_MAX];        /* normalized path for file handles */
    u32  size;                             /* file size in bytes */
    u32  offset;                           /* current read position */
    u32  cache_page_count;                 /* number of cached 4 KB pages */
    u32  cache_pages_frame;                /* PMM frame holding cached page frame numbers */
};

/* ------------------------------------------------------------------ */
/* process_t                                                          */
/* ------------------------------------------------------------------ */

#define PROCESS_NAME_MAX  32
#define PROCESS_MAX_ARGS  16
#define PROCESS_ARG_BYTES 256
#define PROCESS_CWD_MAX   PROCESS_FD_NAME_MAX
#define PROCESS_KERNEL_STACK_FRAMES 8u
#define PROCESS_KERNEL_STACK_BYTES  (PROCESS_KERNEL_STACK_FRAMES * PAGE_SIZE)

typedef struct process {
    u32*            pd;                 /* PMM physical page-directory frame */
    u32             kernel_stack_frame; /* first PMM physical kernel-stack frame */
    u32             kernel_stack_frames;
    u32             pid;                /* kernel process id, unique until wrap */
    u32             parent_pid;         /* process that spawned this task, if any */
    u32             pgid;               /* lightweight process group id */
    unsigned int    sched_esp;
    volatile process_state_t state;
    int             exit_status;
    volatile int    reaper_claimed;  /* 1 once a waiter owns zombie cleanup */
    unsigned int    sleep_until;
    void          (*kernel_entry)(void);
    unsigned int    user_entry;
    int             user_argc;
    char*           user_argv[PROCESS_MAX_ARGS + 1];
    char            user_arg_data[PROCESS_ARG_BYTES];
    unsigned int    heap_base;
    unsigned int    heap_brk;
    char            cwd[PROCESS_CWD_MAX];  /* canonical path without leading slash */
    char            name[PROCESS_NAME_MAX];
    fd_entry_t*     fds;                /* PMM-backed per-process open handles */
    unsigned int    fd_capacity;        /* allocated slots in fds */
    unsigned int    fd_limit;           /* maximum allowed slots for this proc */
    u32             fd_table_frame;     /* first PMM frame backing fds */
    u32             fd_table_frames;    /* contiguous frame count backing fds */
} process_t;

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

process_t* process_create(const char* name);
process_t* process_create_kernel_task(const char* name, void (*entry)(void));
void       process_destroy(process_t* proc);
fd_entry_t* process_fd_get(process_t* proc, int fd);
int        process_fd_open_file(process_t* proc,
                                const char* name,
                                u32 size,
                                int writable);
int        process_fd_open_file_mode(process_t* proc,
                                     const char* name,
                                     u32 size,
                                     int readable,
                                     int writable);
int        process_fd_open_socket(process_t* proc, const char* name);
int        process_fd_open_special(process_t* proc, int kind, const char* name);
int        process_fd_pipe(process_t* proc, int fds[2], unsigned int flags);
int        process_fd_pty(process_t* proc, int fds[2], unsigned int master_flags);
int        process_fd_pty_set_size(fd_entry_t* ent, unsigned int rows, unsigned int cols);
int        process_fd_terminal_size(fd_entry_t* ent, unsigned int* out_rows, unsigned int* out_cols);
int        process_fd_pty_set_foreground(fd_entry_t* ent, u32 pgid);
u32        process_fd_pty_get_foreground(fd_entry_t* ent);
int        process_fd_dup(process_t* proc, int oldfd, int minfd, unsigned int fd_flags);
int        process_fd_dup2(process_t* proc, int oldfd, int newfd, unsigned int fd_flags, int reject_same);
int        process_fd_dup_from(process_t* dst, int newfd, process_t* src, int oldfd, unsigned int fd_flags);
void       process_fd_close(fd_entry_t* ent);
int        process_fd_read(fd_entry_t* ent, char* buf, unsigned int len);
int        process_fd_read_raw(fd_entry_t* ent, char* buf, unsigned int len);
int        process_fd_write(fd_entry_t* ent, const char* buf, unsigned int len);
short      process_fd_poll(fd_entry_t* ent, short events);
int        process_fd_wait(fd_entry_t* ent, process_t* proc, short events);
int        process_fd_flush(fd_entry_t* ent);
int        process_fd_seek(fd_entry_t* ent, int offset, int whence);
int        process_fd_set_flags(fd_entry_t* ent, unsigned int flags);
unsigned int process_fd_get_flags(fd_entry_t* ent);
int        process_fd_set_fd_flags(fd_entry_t* ent, unsigned int flags);
unsigned int process_fd_get_fd_flags(fd_entry_t* ent);
void       process_close_cloexec_fds(process_t* proc);
int        process_copy_fd_table(process_t* dst, process_t* src);
process_t* process_fork_from_syscall(unsigned int regs_esp, unsigned int frame_top);
int        process_fd_set_signalfd_mask(fd_entry_t* ent, unsigned int mask);
void       process_wake_timerfds(process_t* proc, unsigned int now);
void       process_claim_for_wait(process_t* proc);
process_t* process_find_by_pid(u32 pid);
int        process_wait_pid(process_t* parent,
                            int pid,
                            int options,
                            int* out_pid,
                            int* out_status);
int        process_kill_pid(int pid, int status, unsigned int esp);
int        process_reap_unclaimed_zombies(void);
int        process_set_args(process_t* proc, int argc, char** argv);

process_t* process_get_current(void);

/*
 * Foreground terminal/input owner.
 *
 * Keyboard bytes are routed to one foreground reader, but terminal-generated
 * signals such as Ctrl+C target the reader's process group.  Children spawned
 * by a foreground user process inherit that group, so terminal interrupts can
 * cover the small async process trees SmallOS supports today.
 */
void       process_init_user_group(process_t* proc);
void       process_set_foreground(process_t* proc);
void       process_set_foreground_preserve_input(process_t* proc);
process_t* process_get_foreground(void);
u32        process_get_foreground_group(void);
int        process_signal_deliver(process_t* proc, int signum);
int        process_group_signal_deliver(u32 pgid, int signum);
int        process_group_kill(u32 pgid, int status);

/*
 * Terminal interrupt delivery.
 *
 * Ctrl+C is interpreted by the foreground input consumer as a request
 * to signal or terminate the current foreground process group with the
 * conventional status 130.  IRQ1 calls
 * process_deliver_pending_terminal_interrupt() after the keyboard consumer
 * returns so a currently running foreground member can be switched away using
 * the IRQ frame ESP.
 */
void       process_deliver_pending_terminal_interrupt(unsigned int esp);

/*
 * Wait for a process to reach ZOMBIE, then destroy it from the current
 * safe stack and return its exit status.
 */
int        process_wait(process_t* proc);
int        process_wait_detachable(process_t* proc, int* detached);
int        process_wait_restore_foreground(process_t* proc, process_t* restore_proc);

/*
 * process_start_reaper()
 *
 * Create and enqueue the kernel reaper task.  The reaper wakes on every
 * timer tick, calls sched_reap_zombies() to free any unclaimed ZOMBIE
 * processes (e.g. runelf_nowait / SYS_EXEC children), then halts until
 * the next interrupt.  Call once from kernel_main() before sched_start().
 * Returns 1 on success, 0 if the task could not be created or queued.
 */
int        process_start_reaper(void);

#endif /* PROCESS_H */
