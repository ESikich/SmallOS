#include "syscall.h"
#include "terminal.h"
#include "timer.h"
#include "paging.h"
#include "keyboard.h"
#include "scheduler.h"
#include "process.h"
#include "../exec/elf_loader.h"

#define SYSCALL_ERR_INVALID   ((unsigned int)-1)
#define SYSCALL_MAX_WRITE_LEN 4096u
#define EXEC_NAME_MAX         32
#define SCHED_RESUME_RETADDR_OFFSET 8u  /* used by SYS_YIELD only */

/* ------------------------------------------------------------------ */
/* Copy-from-user validation                                          */
/* ------------------------------------------------------------------ */

/*
 * User virtual address space: [USER_CODE_BASE, USER_STACK_TOP)
 *
 *   0x400000   USER_CODE_BASE  — lowest mapped user address (ELF load base)
 *   0xC0000000 USER_STACK_TOP  — top of user virtual space (exclusive)
 *
 * user_buf_ok(ptr, len) returns 1 if the byte range [ptr, ptr+len) lies
 * entirely within user space, 0 otherwise.
 *
 * Rules:
 *   - ptr must be non-null and >= USER_CODE_BASE
 *   - ptr+len must be <= USER_STACK_TOP (checked without overflow)
 *   - len must be > 0 (callers handle len==0 before calling)
 *
 * This is an address-range check only — it does not walk page tables.
 * A user process could pass a valid-range address that is not backed by a
 * present PTE, which would page-fault in the kernel.  That fault path is
 * not handled today; this check closes the easier attack of passing an
 * explicit kernel address to read or corrupt kernel memory.
 */
static int user_buf_ok(unsigned int ptr, unsigned int len) {
    if (ptr < USER_CODE_BASE)       return 0;   /* catches null and kernel addrs */
    if (len == 0)                   return 0;
    /* Overflow-safe upper bound check: */
    if (len > USER_STACK_TOP - ptr) return 0;
    return 1;
}

/*
 * user_str_ok(ptr) — validate a user-supplied string pointer base.
 *
 * Only checks that the pointer itself falls inside user space.  The full
 * string length is unknown before scanning, so we cannot do a complete
 * range check.  Callers that copy the string (sys_exec_impl) bound the
 * copy explicitly and stop at '\0', so a base check is sufficient to
 * block kernel-address attacks.
 */
static int user_str_ok(unsigned int ptr) {
    if (ptr < USER_CODE_BASE)  return 0;
    if (ptr >= USER_STACK_TOP) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Syscall implementations                                            */
/* ------------------------------------------------------------------ */

static int sys_write_impl(const char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (len > SYSCALL_MAX_WRITE_LEN) return -1;
    if (!user_buf_ok((unsigned int)buf, len)) return -1;

    for (unsigned int i = 0; i < len; i++) {
        terminal_putc(buf[i]);
    }
    return (int)len;
}

static int sys_putc_impl(unsigned int ch) {
    terminal_putc((char)ch);
    return 1;
}

static void sys_exit_impl(syscall_regs_t* regs) {
    (void)regs->ebx;
    paging_switch(paging_get_kernel_pd());
    sched_exit_current((unsigned int)regs);
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

static unsigned int sys_get_ticks_impl(void) {
    return timer_get_ticks();
}

static int sys_yield_impl(unsigned int esp) {
    sched_yield_now(esp - SCHED_RESUME_RETADDR_OFFSET);
    return 0;
}

/*
 * sys_read_impl — true blocking keyboard read.
 *
 * When the keyboard buffer is empty, the calling process is parked:
 *
 *   1. proc->state is set to PROCESS_STATE_WAITING so that
 *      sched_find_next_runnable_from() skips this task on every timer tick.
 *   2. keyboard_set_waiting_process(proc) registers the waiter so that
 *      process_key_consumer() (IRQ1 context) can wake it.
 *   3. sti; hlt — re-enables interrupts and suspends the CPU.
 *
 * While halted the timer IRQ fires normally.  sched_tick() sees that this
 * task is WAITING, skips it, and switches to another runnable task.  When
 * a keypress arrives, process_key_consumer() sets proc->state back to
 * PROCESS_STATE_RUNNING and clears the waiter slot.  On the next timer
 * tick sched_tick() selects this task again; execution resumes after the
 * hlt instruction, re-checks keyboard_buf_available(), finds the character,
 * and continues normally.
 *
 * The outer while loop re-checks the buffer on every wakeup, which
 * correctly handles spurious wakeups (none expected today, but the
 * guard is cheap and correct).
 *
 * IF management:
 *   The syscall gate is an interrupt gate so the CPU clears IF on entry.
 *   We re-enable with sti before the first hlt so IRQ1 can fire.  After
 *   all characters have been collected we restore cli before returning,
 *   matching the expected IF=0 postcondition of the syscall gate.
 */
static int sys_read_impl(char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -1;

    process_t* proc = (process_t*)sched_current();

    unsigned int n = 0;

    __asm__ volatile ("sti");

    while (n < len) {
        /* Park until at least one character is available. */
        while (!keyboard_buf_available()) {
            if (proc) {
                proc->state = PROCESS_STATE_WAITING;
                keyboard_set_waiting_process(proc);
            }
            __asm__ volatile ("hlt");
            /*
             * Execution resumes here after the timer IRQ switches back to
             * this task.  proc->state has been set to RUNNING by
             * process_key_consumer() before the scheduler selected us again.
             * Re-check the buffer — if it is still empty we park again.
             */
        }

        char c = keyboard_buf_pop();
        terminal_putc(c);
        buf[n++] = c;
        if (c == '\n') break;
    }

    __asm__ volatile ("cli");
    return (int)n;
}

/*
 * sys_exec_impl(name, argc, argv)
 *
 * Spawn a named ELF program asynchronously.
 *
 * name and argv are user virtual addresses — validated before use.
 * name is copied into a kernel-side buffer before calling elf_run_named()
 * because the loader later switches page directories and must not depend
 * on the caller's user pointer remaining valid.
 *
 * argv validation: we check that the argv array itself lies in user space.
 * Individual argv[i] string pointers are not validated here — they are
 * consumed by elf_seed_sched_context() while the caller's CR3 is still
 * active, so a bad argv[i] would fault rather than silently corrupt kernel
 * memory.  Full per-element argv validation is left for a future pass.
 *
 * Returns 0 on success, -1 if validation fails or the program was not found.
 */
static int sys_exec_impl(const char* name, int argc, char** argv) {
    if (!user_str_ok((unsigned int)name)) return -1;

    /* argv may legitimately be null if argc == 0 */
    if (argc > 0 && !user_buf_ok((unsigned int)argv,
                                  (unsigned int)argc * sizeof(char*))) {
        return -1;
    }

    char kname[EXEC_NAME_MAX];
    unsigned int i = 0;
    while (i < EXEC_NAME_MAX - 1 && name[i] != '\0') {
        kname[i] = name[i];
        i++;
    }
    kname[i] = '\0';

    if (i == 0) return -1;

    return elf_run_named(kname, argc, argv) ? 0 : -1;
}

void syscall_handler_main(syscall_regs_t* regs) {
    if (regs == 0) return;

    switch (regs->eax) {
        case SYS_WRITE:
            regs->eax = (unsigned int)sys_write_impl(
                            (const char*)regs->ebx, regs->ecx);
            break;

        case SYS_EXIT:
            sys_exit_impl(regs);
            break;

        case SYS_GET_TICKS:
            regs->eax = sys_get_ticks_impl();
            break;

        case SYS_PUTC:
            regs->eax = (unsigned int)sys_putc_impl(regs->ebx);
            break;

        case SYS_READ:
            regs->eax = (unsigned int)sys_read_impl(
                            (char*)regs->ebx,
                            regs->ecx);
            break;

        case SYS_YIELD:
            regs->eax = (unsigned int)sys_yield_impl((unsigned int)regs);
            break;

        case SYS_EXEC:
            regs->eax = (unsigned int)sys_exec_impl(
                            (const char*)regs->ebx,
                            (int)regs->ecx,
                            (char**)regs->edx);
            break;

        default:
            regs->eax = SYSCALL_ERR_INVALID;
            break;
    }
}