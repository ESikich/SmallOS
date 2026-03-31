#include "process.h"
#include "pmm.h"
#include "paging.h"
#include "terminal.h"
#include "scheduler.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

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
/* Global: currently-running process                                    */
/* ------------------------------------------------------------------ */

static process_t* s_current = 0;

/* ------------------------------------------------------------------ */
/* Kernel-task bootstrap                                                */
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
/* Public API                                                           */
/* ------------------------------------------------------------------ */

process_t* process_create(const char* name) {
    /*
     * Allocate the process_t from a PMM frame.
     *
     * sizeof(process_t) < PAGE_SIZE (jmp_buf is ~24 bytes on i686), so
     * the struct fits comfortably in one 4 KB frame.  The frame is
     * freed by process_destroy().
     *
     * We deliberately use the PMM (not kmalloc) so the allocation can
     * be reclaimed.  Kernel structures that are truly permanent (GDT,
     * IDT, page tables) continue to use kmalloc_page.
     */
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

    /*
     * Kernel tasks run on the kernel page directory, so pd == 0 keeps
     * the existing scheduler sentinel meaning "use paging_get_kernel_pd()".
     */
    proc->pd = 0;
    proc->kernel_entry = entry;
    proc->state = PROCESS_STATE_RUNNING;

    /*
     * Seed the task's saved ESP so the first sched_switch() loads this
     * stack and RETs into process_kernel_task_bootstrap().
     */
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

    /* Free the page directory and all private frames. */
    if (proc->pd) {
        process_pd_destroy(proc->pd);
        proc->pd = 0;
    }

    /* Free the dedicated kernel stack frame. */
    if (proc->kernel_stack_frame) {
        pmm_free_frame(proc->kernel_stack_frame);
        proc->kernel_stack_frame = 0;
    }

    proc->state = PROCESS_STATE_EXITED;

    /* Free the process_t frame itself. */
    pmm_free_frame((u32)proc);
}

void process_set_current(process_t* proc) {
    s_current = proc;
}

process_t* process_get_current(void) {
    /*
     * Prefer the explicit foreground process pointer when one is set.
     *
     * This matters during the current transition period:
     *   - the shell is now a scheduler-owned kernel task
     *   - user ELF programs still run through the old foreground
     *     iret/longjmp path
     *
     * While a foreground ELF program is running, sched_current() still
     * names the shell task, so returning it here would make sys_exit()
     * tear down the shell instead of the user process.
     */
    if (s_current) {
        return s_current;
    }

    return sched_current();
}