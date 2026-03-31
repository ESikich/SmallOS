#include "process.h"
#include "pmm.h"
#include "paging.h"
#include "terminal.h"

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
    return s_current;
}