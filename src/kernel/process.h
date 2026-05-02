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
 * User-opened files start at fd 3.  PROCESS_FD_MAX controls the total
 * table size (including the reserved slots).
 */
#define PROCESS_FD_MAX      8
#define PROCESS_FD_FIRST    3        /* first allocatable fd */
#define PROCESS_FD_NAME_MAX 128      /* max path length stored in fd entry */
#define PROCESS_FD_CACHE_PAGES 1024  /* 4 MB small-file read cache / 4 KB pages */

typedef struct fd_entry fd_entry_t;

typedef enum {
    PROCESS_HANDLE_KIND_NONE  = 0,
    PROCESS_HANDLE_KIND_FILE  = 1,
    PROCESS_HANDLE_KIND_SOCKET = 2,
    PROCESS_HANDLE_KIND_CONSOLE = 3
} process_handle_kind_t;

typedef enum {
    PROCESS_SOCKET_STATE_NONE      = 0,
    PROCESS_SOCKET_STATE_OPEN      = 1,
    PROCESS_SOCKET_STATE_BOUND     = 2,
    PROCESS_SOCKET_STATE_LISTENER  = 3,
    PROCESS_SOCKET_STATE_CONNECTED = 4
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
    u32  socket_state;                     /* PROCESS_SOCKET_STATE_* */
    u32  socket_port;                      /* listener or peer port */
    char name[PROCESS_FD_NAME_MAX];        /* filename as passed to SYS_OPEN */
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

typedef struct {
    u32*            pd;                 /* PMM physical page-directory frame */
    u32             kernel_stack_frame; /* PMM physical kernel-stack frame */
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
    fd_entry_t      fds[PROCESS_FD_MAX];   /* per-process open handles */
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
void       process_fd_close(fd_entry_t* ent);
int        process_fd_read(fd_entry_t* ent, char* buf, unsigned int len);
int        process_fd_write(fd_entry_t* ent, const char* buf, unsigned int len);
short      process_fd_poll(fd_entry_t* ent, short events);
int        process_fd_flush(fd_entry_t* ent);
int        process_fd_seek(fd_entry_t* ent, int offset, int whence);
void       process_claim_for_wait(process_t* proc);
int        process_set_args(process_t* proc, int argc, char** argv);

process_t* process_get_current(void);

/*
 * Foreground terminal/input owner.
 *
 * When non-null, keyboard input should be routed to this process even if
 * the scheduler happens to be running some other task (for example the
 * shell blocked in process_wait()).
 */
void       process_set_foreground(process_t* proc);
process_t* process_get_foreground(void);

/*
 * Terminal interrupt delivery.
 *
 * Ctrl+C is interpreted by the foreground process input consumer as a request
 * to terminate the current foreground process with the conventional status
 * 130.  IRQ1 calls process_deliver_pending_terminal_interrupt() after the
 * keyboard consumer returns so a currently running foreground process can be
 * switched away using the IRQ frame ESP.
 */
void       process_deliver_pending_terminal_interrupt(unsigned int esp);

/*
 * Wait for a process to reach ZOMBIE, then destroy it from the current
 * safe stack and return its exit status.
 */
int        process_wait(process_t* proc);

/*
 * process_start_reaper()
 *
 * Create and enqueue the kernel reaper task.  The reaper wakes on every
 * timer tick, calls sched_reap_zombies() to free any unclaimed ZOMBIE
 * processes (e.g. runelf_nowait / SYS_EXEC children), then halts until
 * the next interrupt.  Call once from kernel_main() before sched_start().
 */
void       process_start_reaper(void);

#endif /* PROCESS_H */
