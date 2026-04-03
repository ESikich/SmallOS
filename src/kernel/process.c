#include "process.h"
#include "pmm.h"
#include "paging.h"
#include "terminal.h"
#include "scheduler.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static process_t* s_foreground = 0;

static void proc_zero(process_t* p) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned int i = 0; i < sizeof(process_t); i++) b[i] = 0;
}

static void str_copy_n(char* dst, const char* src, unsigned int n) {
    unsigned int i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* Kernel-task bootstrap                                              */
/* ------------------------------------------------------------------ */

static void process_kernel_task_bootstrap(void) {
    process_t* proc = sched_current();

    if (!proc || !proc->kernel_entry) {
        terminal_puts("process: kernel task bootstrap failed\n");
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }

    proc->kernel_entry();

    terminal_puts("process: kernel task returned\n");
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

process_t* process_create(const char* name) {
    u32 frame = pmm_alloc_frame();
    if (!frame) {
        terminal_puts("process: out of frames for process_t\n");
        return 0;
    }

    process_t* proc = (process_t*)frame;
    proc_zero(proc);

    proc->state = PROCESS_STATE_UNUSED;
    if (name) {
        str_copy_n(proc->name, name, PROCESS_NAME_MAX);
    }

    return proc;
}

process_t* process_create_kernel_task(const char* name, void (*entry)(void)) {
    process_t* proc = process_create(name);
    if (!proc) return 0;

    proc->kernel_stack_frame = pmm_alloc_frame();
    if (!proc->kernel_stack_frame) {
        terminal_puts("process: out of frames for kernel stack\n");
        process_destroy(proc);
        return 0;
    }

    proc->pd = 0;
    proc->kernel_entry = entry;
    proc->state = PROCESS_STATE_RUNNING;

    {
        unsigned int* stack_top = (unsigned int*)(proc->kernel_stack_frame + 4096u);
        stack_top--;
        *stack_top = (unsigned int)process_kernel_task_bootstrap;
        proc->sched_esp = (unsigned int)stack_top;
    }

    return proc;
}

void process_destroy(process_t* proc) {
    if (!proc) return;

    if (proc->pd) {
        process_pd_destroy(proc->pd);
        proc->pd = 0;
    }

    if (proc->kernel_stack_frame) {
        pmm_free_frame(proc->kernel_stack_frame);
        proc->kernel_stack_frame = 0;
    }

    if (s_foreground == proc) {
        s_foreground = 0;
    }

    proc->state = PROCESS_STATE_EXITED;
    pmm_free_frame((u32)proc);
}

process_t* process_get_current(void) {
    return sched_current();
}

void process_set_foreground(process_t* proc) {
    s_foreground = proc;
}

process_t* process_get_foreground(void) {
    return s_foreground;
}

void process_wait(process_t* proc) {
    if (!proc) return;

    process_set_foreground(proc);

    while (proc->state != PROCESS_STATE_ZOMBIE) {
        __asm__ __volatile__("sti; hlt");
    }

    process_set_foreground(0);
    process_destroy(proc);
}

/* ------------------------------------------------------------------ */
/* Reaper task                                                         */
/* ------------------------------------------------------------------ */

/*
 * reaper_task_main — runs as a permanent kernel task.
 *
 * On every wakeup it calls sched_reap_zombies() to destroy any processes
 * that exited without an explicit waiter (e.g. runelf_nowait or SYS_EXEC
 * children).  After each scan it halts until the next timer interrupt
 * wakes it, keeping CPU overhead near zero.
 */
static void reaper_task_main(void) {
    for (;;) {
        sched_reap_zombies();
        __asm__ __volatile__("sti; hlt");
    }
}

void process_start_reaper(void) {
    process_t* reaper = process_create_kernel_task("reaper", reaper_task_main);
    if (!reaper) {
        terminal_puts("process: failed to create reaper task\n");
        return;
    }
    if (!sched_enqueue(reaper)) {
        terminal_puts("process: failed to enqueue reaper task\n");
        process_destroy(reaper);
    }
}