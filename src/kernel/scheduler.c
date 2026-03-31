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

static process_t  s_shell_proc;
static process_t* s_table[SCHED_MAX_PROCS];
static int        s_count       = 0;
static int        s_current_idx = 0;
static unsigned int s_tick_count = 0;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static void proc_zero_sched(process_t* p) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned int i = 0; i < sizeof(process_t); i++) b[i] = 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void sched_init(void) {
    proc_zero_sched(&s_shell_proc);
    s_shell_proc.state = PROCESS_STATE_RUNNING;
    s_shell_proc.name[0] = 's';
    s_shell_proc.name[1] = 'h';
    s_shell_proc.name[2] = 'e';
    s_shell_proc.name[3] = 'l';
    s_shell_proc.name[4] = 'l';
    s_shell_proc.name[5] = '\0';
    /*
     * pd == 0 is the sentinel meaning "use kernel PD".
     * sched_esp starts at 0 and is written the first time the timer
     * fires while the shell is running.
     */
    s_shell_proc.pd = 0;

    s_table[0] = &s_shell_proc;
    s_count = 1;
    s_current_idx = 0;

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
    for (int i = 1; i < s_count; i++) {
        if (s_table[i] == proc) { idx = i; break; }
    }
    if (idx < 0) return;

    /* Compact the table. */
    for (int i = idx; i < s_count - 1; i++) {
        s_table[i] = s_table[i + 1];
    }
    s_table[s_count - 1] = 0;
    s_count--;

    /* Reset to shell slot. elf_process_exit will longjmp back there. */
    s_current_idx = 0;
    s_tick_count  = 0;
}

void sched_tick(unsigned int esp) {
    s_tick_count++;
    if (s_tick_count < SCHED_TICKS_PER_QUANTUM) return;
    s_tick_count = 0;

    if (s_count <= 1) return;   /* only the shell — nothing to switch to */

    /*
     * Save the current context's kernel ESP.  This is the ESP value
     * passed in from irq0_stub — it points at the register frame on
     * the kernel stack just below irq0_handler_main's own frame.
     * sched_switch will also save the ESP inside itself (one level
     * deeper), but we need the *irq0_stub* frame level for the iretd
     * on the way back.  We therefore pass &cur->sched_esp to
     * sched_switch as the save pointer so that sched_switch overwrites
     * it with the ESP at call-return depth, giving us the right level
     * to resume from.
     *
     * For context: the resume path is:
     *   sched_switch ret -> sched_tick -> irq0_handler_main -> irq0_stub iretd
     * which is exactly what we want.
     */
    s_table[s_current_idx]->sched_esp = esp; /* record for debugging / future use */

    /* Pick next runnable slot. */
    int next = (s_current_idx + 1) % s_count;
    int tries = 0;
    while (tries < s_count) {
        process_t* candidate = s_table[next];
        /*
         * A slot is switchable if:
         *   - state == RUNNING
         *   - sched_esp != 0  (has been suspended at least once, so its
         *                      kernel stack has a valid resume address)
         *
         * The shell slot (pd==0) satisfies sched_esp!=0 after the first
         * time it was preempted.  A brand-new user process has sched_esp==0
         * until it has been preempted for the first time; skip it on the
         * very first tick to avoid jumping to ESP=0.
         *
         * Exception: if the candidate is the shell and it has never been
         * preempted (sched_esp==0), we still cannot switch to it — but
         * this should only happen before the first user process ever runs,
         * at which point s_count==1 and we returned above.
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

    unsigned int next_cr3;
    unsigned int next_esp0;

    if (nxt->pd == 0) {
        /* Shell slot — kernel PD. */
        next_cr3  = (unsigned int)paging_get_kernel_pd();
        next_esp0 = 0x90000;
    } else {
        next_cr3  = (unsigned int)nxt->pd;
        next_esp0 = nxt->kernel_stack_frame + 4096u;
    }

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
                 next_cr3,
                 next_esp0);

    /*
     * Execution resumes here when this context is switched back to.
     * Return normally to irq0_handler_main -> irq0_stub -> iretd.
     */
}

process_t* sched_current(void) {
    return s_table[s_current_idx];
}