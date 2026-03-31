#include "scheduler.h"
#include "process.h"
#include "paging.h"
#include "gdt.h"
#include "terminal.h"

/* ------------------------------------------------------------------ */
/* tss_esp0_ptr — exported to sched_switch.asm                         */
/* ------------------------------------------------------------------ */

unsigned int* tss_esp0_ptr = 0;

/* ------------------------------------------------------------------ */
/* Assembly helper                                                      */
/* ------------------------------------------------------------------ */

extern void sched_switch(unsigned int* save_esp,
                         unsigned int  next_esp,
                         unsigned int  next_cr3,
                         unsigned int  next_esp0);

/* ------------------------------------------------------------------ */
/* Process table                                                        */
/* ------------------------------------------------------------------ */

static process_t*   s_table[SCHED_MAX_PROCS];
static int          s_count       = 0;
static int          s_current_idx = -1;
static unsigned int s_tick_count  = 0;
static unsigned int s_boot_esp    = 0;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static unsigned int sched_proc_cr3(process_t* proc) {
    if (!proc || proc->pd == 0) {
        return (unsigned int)paging_get_kernel_pd();
    }
    return (unsigned int)proc->pd;
}

static unsigned int sched_proc_esp0(process_t* proc) {
    if (!proc || proc->kernel_stack_frame == 0) {
        return 0x90000u;
    }
    return proc->kernel_stack_frame + 4096u;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void sched_init(void) {
    for (int i = 0; i < SCHED_MAX_PROCS; i++) {
        s_table[i] = 0;
    }

    s_count = 0;
    s_current_idx = -1;
    s_tick_count = 0;
    s_boot_esp = 0;

    tss_esp0_ptr = tss_get_esp0_ptr();
}

int sched_enqueue(process_t* proc) {
    if (!proc) return 0;
    if (s_count >= SCHED_MAX_PROCS) {
        terminal_puts("sched: table full\n");
        return 0;
    }
    s_table[s_count] = proc;
    s_count++;
    return 1;
}

void sched_dequeue(process_t* proc) {
    if (!proc) return;

    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_table[i] == proc) { idx = i; break; }
    }
    if (idx < 0) return;

    /* Compact the table. */
    for (int i = idx; i < s_count - 1; i++) {
        s_table[i] = s_table[i + 1];
    }
    s_table[s_count - 1] = 0;
    s_count--;

    if (s_count == 0) {
        s_current_idx = -1;
    } else if (s_current_idx >= s_count) {
        s_current_idx = 0;
    }

    s_tick_count  = 0;
}

void sched_start(process_t* first_proc) {
    if (!first_proc) {
        terminal_puts("sched: no first process\n");
        return;
    }

    for (int i = 0; i < s_count; i++) {
        if (s_table[i] == first_proc) {
            s_current_idx = i;
            break;
        }
    }

    if (s_current_idx < 0) {
        terminal_puts("sched: first process not enqueued\n");
        return;
    }

    sched_switch(&s_boot_esp,
                 first_proc->sched_esp,
                 sched_proc_cr3(first_proc),
                 sched_proc_esp0(first_proc));

    terminal_puts("sched: unexpected return from sched_start\n");
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

void sched_tick(unsigned int esp) {
    s_tick_count++;
    if (s_tick_count < SCHED_TICKS_PER_QUANTUM) return;
    s_tick_count = 0;

    if (s_count <= 1 || s_current_idx < 0) return;

    /*
     * Save the current ESP so this task can resume at exactly the point
     * where IRQ0 interrupted it.
     */
    s_table[s_current_idx]->sched_esp = esp;

    int next = (s_current_idx + 1) % s_count;
    int tries = 0;
    while (tries < s_count) {
        process_t* candidate = s_table[next];

        /*
         * A runnable task must have a valid saved kernel ESP.  Newly
         * created kernel tasks are seeded with one by process.c; user
         * processes get theirs the first time they enter the scheduler.
         */
        if (candidate->state == PROCESS_STATE_RUNNING &&
            candidate->sched_esp != 0) {
            break;
        }
        next = (next + 1) % s_count;
        tries++;
    }

    if (next == s_current_idx) return;   /* no eligible next process */

    int prev = s_current_idx;
    s_current_idx = next;

    process_t* cur = s_table[prev];
    process_t* nxt = s_table[next];

    /*
     * sched_switch(&cur->sched_esp, nxt->sched_esp, next_cr3, next_esp0)
     *
     * sched_switch overwrites cur->sched_esp with the ESP at the call
     * site inside sched_switch, which is on the path:
     *   sched_switch ret -> sched_tick ret -> irq0_handler_main ret
     *   -> irq0_stub iretd
     *
     * That is the correct resume point for this context next time it
     * is switched back to.
     */
    sched_switch(&cur->sched_esp,
                 nxt->sched_esp,
                 sched_proc_cr3(nxt),
                 sched_proc_esp0(nxt));

    /*
     * Execution resumes here when this context is switched back to.
     * Return normally to irq0_handler_main -> irq0_stub -> iretd.
     */
}

process_t* sched_current(void) {
    if (s_current_idx < 0 || s_current_idx >= s_count) {
        return 0;
    }
    return s_table[s_current_idx];
}