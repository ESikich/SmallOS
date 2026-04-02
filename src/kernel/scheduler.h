#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "process.h"

/*
 * scheduler.h — Preemptive round-robin scheduler
 *
 * The scheduler maintains a fixed-size table of runnable process_t
 * pointers. On every timer tick it checks whether a context switch is
 * due and, if so, calls the assembly helper sched_switch() to swap
 * kernel stacks and CR3.
 *
 * Scheduling policy: simple round-robin.
 * SCHED_TICKS_PER_QUANTUM controls how many IRQ0 ticks a process
 * runs before being preempted.
 */

#define SCHED_MAX_PROCS         8
#define SCHED_TICKS_PER_QUANTUM 10  /* ~100 ms at 100 Hz */

/*
 * sched_init()
 *
 * Initialise the scheduler.  Must be called from kernel_main() after
 * paging and PMM are ready, before sti.
 */
void sched_init(void);

/*
 * sched_enqueue(proc)
 *
 * Add a process to the run queue.
 *
 * The process must have a valid first-entry or resume scheduler stack in
 * sched_esp before it is eligible to run:
 *
 *   - Kernel tasks: process_create_kernel_task() seeds sched_esp
 *   - ELF tasks:    elf_seed_sched_context() seeds sched_esp
 *
 * Returns 1 on success, 0 if the table is full.
 */
int sched_enqueue(process_t* proc);

/*
 * sched_dequeue(proc)
 *
 * Remove a process from the run queue.
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
 * sched_yield_now(esp)
 *
 * Voluntarily yield the remainder of the current quantum and immediately
 * switch to the next runnable process.
 *
 * Called from sys_yield_impl() via the SYS_YIELD syscall path.  esp is
 * the kernel stack pointer of the isr128_stub frame — structurally
 * identical to an irq0_stub frame, so sched_switch can resume it via
 * iretd exactly as it would a timer-preempted context.
 *
 * Resets the tick counter so the next process gets a full quantum.
 *
 * Must be called with interrupts disabled (the int 0x80 gate clears IF).
 */
void sched_yield_now(unsigned int esp);

/*
 * sched_exit_current(esp)
 *
 * Terminate the current task without returning to it.
 *
 * Used by elf_process_exit() while executing on the exiting task's
 * kernel stack.  The scheduler removes the task from the run queue,
 * marks it for deferred destruction, and switches directly to the next
 * runnable task.  The actual process_destroy() happens later on a safe
 * stack inside the scheduler.
 */
void sched_exit_current(unsigned int esp);

/*
 * sched_current()
 *
 * Returns the process_t* currently running, or 0 before sched_start().
 */
process_t* sched_current(void);

/*
 * sched_start(first_proc)
 *
 * Enter the scheduler for the first time by switching away from the
 * boot stack to first_proc's saved kernel stack.
 */
void sched_start(process_t* first_proc);

#endif /* SCHEDULER_H */