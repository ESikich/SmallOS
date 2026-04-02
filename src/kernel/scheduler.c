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
static process_t*   s_reap_pending = 0;

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

static void sched_reap_if_needed(void) {
    process_t* reap = s_reap_pending;
    if (!reap) return;

    s_reap_pending = 0;
    process_destroy(reap);
}

static int sched_find_next_runnable_from(int start_idx) {
    if (s_count <= 0) return -1;

    int next = start_idx;
    for (int tries = 0; tries < s_count; tries++) {
        if (next >= s_count) next = 0;

        process_t* candidate = s_table[next];
        if (candidate &&
            candidate->state == PROCESS_STATE_RUNNING &&
            candidate->sched_esp != 0) {
            return next;
        }

        next++;
    }

    return -1;
}

/*
 * sched_do_switch(esp)
 *
 * Shared core used by both sched_tick() and sched_yield_now().
 * Saves esp into the current slot, picks the next eligible process,
 * and calls sched_switch() if one is found.
 */
static void sched_do_switch(unsigned int esp) {
    if (s_count <= 1 || s_current_idx < 0) return;

    s_table[s_current_idx]->sched_esp = esp;

    int next = sched_find_next_runnable_from((s_current_idx + 1) % s_count);
    if (next < 0 || next == s_current_idx) return;

    int prev = s_current_idx;
    s_current_idx = next;

    process_t* cur = s_table[prev];
    process_t* nxt = s_table[next];

    sched_switch(&cur->sched_esp,
                 nxt->sched_esp,
                 sched_proc_cr3(nxt),
                 sched_proc_esp0(nxt));

    /* Execution resumes here when this context is switched back to. */
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
    s_reap_pending = 0;

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

    for (int i = idx; i < s_count - 1; i++) {
        s_table[i] = s_table[i + 1];
    }
    s_table[s_count - 1] = 0;
    s_count--;

    if (s_count == 0) {
        s_current_idx = -1;
    } else if (idx < s_current_idx) {
        s_current_idx--;
    } else if (s_current_idx >= s_count) {
        s_current_idx = 0;
    }

    s_tick_count = 0;
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
    sched_reap_if_needed();

    s_tick_count++;
    if (s_tick_count < SCHED_TICKS_PER_QUANTUM) return;
    s_tick_count = 0;

    sched_do_switch(esp);
}

void sched_yield_now(unsigned int esp) {
    sched_reap_if_needed();

    /*
     * Bypass the quantum counter — the process is voluntarily giving up
     * the rest of its slice.  Reset the counter so the next process gets
     * a full quantum rather than inheriting whatever ticks remain.
     */
    s_tick_count = 0;

    sched_do_switch(esp);
}

void sched_exit_current(unsigned int esp) {
    if (s_current_idx < 0 || s_current_idx >= s_count) {
        terminal_puts("sched: no current task to exit\n");
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }

    process_t* cur = s_table[s_current_idx];
    if (!cur) {
        terminal_puts("sched: current slot empty on exit\n");
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }

    cur->sched_esp = esp;

    /*
     * Freeing the current task immediately would free the kernel stack
     * we are actively executing on.  Defer process_destroy() until the
     * scheduler next runs on a safe stack.
     */
    s_reap_pending = cur;
    sched_dequeue(cur);

    if (s_count <= 0 || s_current_idx < 0) {
        terminal_puts("sched: no runnable task after exit\n");
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }

    int next = sched_find_next_runnable_from(s_current_idx);
    if (next < 0) {
        terminal_puts("sched: no eligible next task after exit\n");
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }

    s_current_idx = next;

    process_t* nxt = s_table[next];
    unsigned int dead_esp = esp;

    sched_switch(&dead_esp,
                 nxt->sched_esp,
                 sched_proc_cr3(nxt),
                 sched_proc_esp0(nxt));

    terminal_puts("sched: unexpected return from sched_exit_current\n");
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

process_t* sched_current(void) {
    if (s_current_idx < 0 || s_current_idx >= s_count) {
        return 0;
    }
    return s_table[s_current_idx];
}