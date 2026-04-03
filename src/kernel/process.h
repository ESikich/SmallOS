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
    PROCESS_STATE_ZOMBIE  = 3
} process_state_t;

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
    void          (*kernel_entry)(void);
    unsigned int    user_entry;
    int             user_argc;
    char*           user_argv[PROCESS_MAX_ARGS];
    char            user_arg_data[PROCESS_ARG_BYTES];
    char            name[PROCESS_NAME_MAX];
} process_t;

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

process_t* process_create(const char* name);
process_t* process_create_kernel_task(const char* name, void (*entry)(void));
void       process_destroy(process_t* proc);

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
 * safe stack and return.
 */
void       process_wait(process_t* proc);

#endif /* PROCESS_H */