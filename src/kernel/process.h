#ifndef PROCESS_H
#define PROCESS_H

#include "paging.h"
#include "setjmp.h"

/* ------------------------------------------------------------------ */
/* Process state                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    PROCESS_STATE_UNUSED  = 0,
    PROCESS_STATE_RUNNING = 1,
    PROCESS_STATE_EXITED  = 2
} process_state_t;

/* ------------------------------------------------------------------ */
/* process_t                                                            */
/* ------------------------------------------------------------------ */

#define PROCESS_NAME_MAX  32
#define PROCESS_MAX_ARGS  16
#define PROCESS_ARG_BYTES 256

typedef struct {
    u32*            pd;
    u32             kernel_stack_frame;
    jmp_buf         exit_ctx;
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
/* API                                                                  */
/* ------------------------------------------------------------------ */

process_t* process_create(const char* name);
process_t* process_create_kernel_task(const char* name, void (*entry)(void));
void       process_destroy(process_t* proc);

/*
 * Explicit foreground process tracking for the current hybrid model.
 *
 * While a foreground ELF is running through the older setjmp/longjmp
 * path, process_get_current() must return that process rather than the
 * scheduler-owned shell task.
 */
void       process_set_current(process_t* proc);
process_t* process_get_current(void);

#endif /* PROCESS_H */