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
/* Per-process file descriptor table                                  */
/* ------------------------------------------------------------------ */

/*
 * File descriptors 0, 1, 2 are reserved for stdin/stdout/stderr.
 * User-opened files start at fd 3.  PROCESS_FD_MAX controls the total
 * table size (including the reserved slots).
 */
#define PROCESS_FD_MAX      8
#define PROCESS_FD_FIRST    3        /* first allocatable fd */
#define PROCESS_FD_NAME_MAX 16       /* max filename length stored in fd entry */
#define PROCESS_FD_CACHE_PAGES 64    /* 256 KB max file / 4 KB per page */

typedef struct {
    int  valid;                            /* 1 if this slot is open */
    char name[PROCESS_FD_NAME_MAX];        /* filename as passed to SYS_OPEN */
    u32  size;                             /* file size in bytes */
    u32  offset;                           /* current read position */
    u32  cache_page_count;                 /* number of cached 4 KB pages */
    u32  cache_pages[PROCESS_FD_CACHE_PAGES]; /* PMM frames holding cached data */
} fd_entry_t;

/* ------------------------------------------------------------------ */
/* process_t                                                          */
/* ------------------------------------------------------------------ */

#define PROCESS_NAME_MAX  32
#define PROCESS_MAX_ARGS  16
#define PROCESS_ARG_BYTES 256

typedef struct {
    u32*            pd;
    u32             kernel_stack_frame;
    unsigned int    sched_esp;
    process_state_t state;
    int             exit_status;
    unsigned int    sleep_until;
    void          (*kernel_entry)(void);
    unsigned int    user_entry;
    int             user_argc;
    char*           user_argv[PROCESS_MAX_ARGS];
    char            user_arg_data[PROCESS_ARG_BYTES];
    char            name[PROCESS_NAME_MAX];
    fd_entry_t      fds[PROCESS_FD_MAX];   /* per-process open file table */
} process_t;

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

process_t* process_create(const char* name);
process_t* process_create_kernel_task(const char* name, void (*entry)(void));
void       process_destroy(process_t* proc);
void       process_fd_cache_free(fd_entry_t* ent);

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
