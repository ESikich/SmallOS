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
/* All kernel-visible state for one schedulable context in one place.   */
/* One instance is allocated for each ELF process or kernel task and    */
/* is freed when that context is destroyed.                             */
/*                                                                      */
/* Fields:                                                              */
/*   pd                 – physical/virtual address of the process page  */
/*                        directory (PMM-allocated, freed on exit)      */
/*   kernel_stack_frame – PMM frame used as the ring-0 syscall stack;   */
/*                        TSS ESP0 = kernel_stack_frame + PAGE_SIZE     */
/*   exit_ctx           – setjmp buffer saved just before iret into     */
/*                        ring 3; longjmp target for sys_exit           */
/*   sched_esp          – kernel ESP saved by the scheduler when this   */
/*                        process is preempted; restored on switch-in   */
/*   state              – lifecycle flag                                */
/*   name               – null-terminated name (truncated to 31 chars)  */
/*                                                                      */
/* Kernel tasks reuse the same structure:                               */
/*   pd == 0            – sentinel meaning "run on kernel page dir"     */
/*   kernel_entry       – C entry point used the first time the task    */
/*                        is scheduled                                  */
/* ------------------------------------------------------------------ */

#define PROCESS_NAME_MAX 32

typedef struct {
    u32*            pd;
    u32             kernel_stack_frame;
    jmp_buf         exit_ctx;
    unsigned int    sched_esp;          /* scheduler: saved kernel ESP  */
    process_state_t state;
    void          (*kernel_entry)(void);
    char            name[PROCESS_NAME_MAX];
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
 * process_create_kernel_task(name, entry)
 *
 * Allocate a kernel task with its own dedicated kernel stack.
 * The task starts executing at entry() the first time the scheduler
 * switches to it.
 *
 * Returns a pointer to the new task, or 0 on allocation failure.
 */
process_t* process_create_kernel_task(const char* name, void (*entry)(void));

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
 * Track the currently executing process.  Existing ELF launch / exit
 * code still uses this directly.  Kernel tasks are scheduled through
 * scheduler.c, so process_get_current() may also be backed by the
 * scheduler when one is active.
 */
void       process_set_current(process_t* proc);
process_t* process_get_current(void);

#endif /* PROCESS_H */