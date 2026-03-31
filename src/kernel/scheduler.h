#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "process.h"

/*
 * scheduler.h — Preemptive round-robin scheduler
 *
 * The scheduler maintains a fixed-size table of runnable process_t
 * pointers plus one slot for the idle/shell context.  On every timer
 * tick it checks whether a context switch is due and, if so, calls the
 * assembly helper sched_switch() to swap kernel stacks and CR3.
 *
 * Slot 0 is always the shell/idle context.  It is initialised by
 * sched_init() and never removed.
 *
 * Scheduling policy: simple round-robin, one tick per quantum.
 * SCHED_TICKS_PER_QUANTUM controls how many IRQ0 ticks a process
 * runs before being preempted.
 */

#define SCHED_MAX_PROCS        8    /* including slot 0 (shell) */
#define SCHED_TICKS_PER_QUANTUM 10  /* ~100 ms at 100 Hz */

/*
 * sched_init()
 *
 * Initialise the scheduler.  Must be called from kernel_main() after
 * paging and PMM are ready, before sti.  Registers the shell context
 * as slot 0 using the current kernel stack.
 */
void sched_init(void);

/*
 * sched_enqueue(proc)
 *
 * Add a process to the run queue.  Called by elf_run_image() just
 * before iret-ing into ring 3.  The process must have pd,
 * kernel_stack_frame, and sched_esp populated.
 *
 * Returns 1 on success, 0 if the table is full.
 */
int sched_enqueue(process_t* proc);

/*
 * sched_dequeue(proc)
 *
 * Remove a process from the run queue.  Called by elf_process_exit()
 * after the process has exited and its resources have been freed.
 * Switches back to the shell slot immediately.
 */
void sched_dequeue(process_t* proc);

/*
 * sched_tick(esp)
 *
 * Called from irq0_handler_main() on every timer interrupt, with the
 * current kernel ESP as argument (i.e. the top of the interrupted
 * context's register frame on the kernel stack).
 *
 * Saves esp into the current slot's sched_esp, advances the round-robin
 * counter, and calls sched_switch() if a new process should run.
 *
 * Must be called with interrupts disabled (inside IRQ handler — they
 * already are).
 */
void sched_tick(unsigned int esp);

/*
 * sched_current()
 *
 * Returns the process_t* currently running, or 0 for the shell slot.
 */
process_t* sched_current(void);

#endif /* SCHEDULER_H */