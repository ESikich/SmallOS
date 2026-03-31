#ifndef PROCESS_H
#define PROCESS_H

#include "paging.h"
#include <setjmp.h>

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
/*                                                                      */
/* All per-process kernel state in one place.  One instance is          */
/* allocated per runelf invocation and freed on exit.                   */
/*                                                                      */
/* Fields:                                                              */
/*   pd                 – physical/virtual address of the process page  */
/*                        directory (PMM-allocated, freed on exit)      */
/*   kernel_stack_frame – PMM frame used as the ring-0 syscall stack;   */
/*                        TSS ESP0 = kernel_stack_frame + PAGE_SIZE     */
/*   exit_ctx           – setjmp buffer saved just before iret into     */
/*                        ring 3; longjmp target for sys_exit           */
/*   state              – lifecycle flag                                 */
/*   name               – null-terminated name (truncated to 31 chars)  */
/* ------------------------------------------------------------------ */

#define PROCESS_NAME_MAX 32

typedef struct {
    u32*           pd;
    u32            kernel_stack_frame;
    jmp_buf        exit_ctx;
    process_state_t state;
    char           name[PROCESS_NAME_MAX];
} process_t;

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

/*
 * process_create(name)
 *
 * Allocate and zero-initialise a process_t.  Sets state to UNUSED.
 * The caller is responsible for filling in pd, kernel_stack_frame, and
 * exit_ctx before launching the process.
 *
 * Returns a pointer to the new process, or 0 on allocation failure.
 */
process_t* process_create(const char* name);

/*
 * process_destroy(proc)
 *
 * Free all resources held by the process:
 *   - the process page directory and all private frames (via paging)
 *   - the kernel stack frame (via PMM)
 *   - the process_t struct itself (via PMM)
 *
 * Safe to call with a null pointer (no-op).
 */
void process_destroy(process_t* proc);

/*
 * process_set_current(proc)
 * process_get_current()
 *
 * Track the currently executing process.  Only one process runs at a
 * time (no scheduler yet).  process_get_current() returns 0 when the
 * kernel is running outside any user process.
 */
void       process_set_current(process_t* proc);
process_t* process_get_current(void);

#endif /* PROCESS_H */