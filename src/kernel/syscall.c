#include "syscall.h"
#include "terminal.h"
#include "timer.h"
#include "paging.h"
#include "elf_loader.h"
#include "keyboard.h"
#include "scheduler.h"

#define SYSCALL_ERR_INVALID  ((unsigned int)-1)
#define SYSCALL_MAX_WRITE_LEN 4096u

static int sys_write_impl(const char* buf, unsigned int len) {
    if (buf == 0) return -1;
    if (len == 0) return 0;
    if (len > SYSCALL_MAX_WRITE_LEN) return -1;

    unsigned int start = (unsigned int)buf;
    unsigned int end   = start + len;
    if (end < start) return -1;

    for (unsigned int i = 0; i < len; i++) {
        terminal_putc(buf[i]);
    }
    return (int)len;
}

static int sys_putc_impl(unsigned int ch) {
    terminal_putc((char)ch);
    return 1;
}

static void sys_exit_impl(int code) {
    (void)code;
    elf_process_exit();
    /* unreachable */
}

static unsigned int sys_get_ticks_impl(void) {
    return timer_get_ticks();
}

static int sys_yield_impl(unsigned int esp) {
    sched_yield_now(esp);
    return 0;
}

static int sys_read_impl(char* buf, unsigned int len) {
    if (buf == 0) return -1;
    if (len == 0) return 0;

    unsigned int n = 0;

    /* The syscall gate is an interrupt gate — IF is cleared on entry.
     * Re-enable interrupts for the duration of the read so keyboard IRQs
     * can fire and populate the input buffer. */
    __asm__ volatile ("sti");

    while (n < len) {
        while (!keyboard_buf_available()) {
            __asm__ volatile ("hlt");
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
 * Load and run a named ELF from the ramdisk as a nested foreground process.
 * The caller is suspended while the child runs, exactly as the shell is
 * suspended during `runelf`.  Returns when the child calls sys_exit().
 *
 * name and argv are user virtual addresses — valid here because the caller's
 * CR3 is still active when the syscall fires (same reason SYS_WRITE and
 * SYS_READ can accept user pointers directly).
 *
 * name is copied into a small kernel-side buffer before calling
 * elf_run_named(), because elf_run_named() calls ramdisk_find() after
 * paging_switch() has changed CR3 to the child's page directory — at that
 * point the original user-space name pointer would no longer be mapped.
 *
 * argv strings are copied inside elf_enter_ring3() onto the child's user
 * stack while the child's PD is active, so they need no pre-copy here.
 *
 * Returns 0 on success, -1 if the program was not found or failed to load.
 */
#define EXEC_NAME_MAX 32

static int sys_exec_impl(const char* name, int argc, char** argv) {
    if (name == 0) return -1;

    /* Copy name out of user space while caller's CR3 is still active. */
    char kname[EXEC_NAME_MAX];
    unsigned int i = 0;
    while (i < EXEC_NAME_MAX - 1 && name[i] != '\0') {
        kname[i] = name[i];
        i++;
    }
    kname[i] = '\0';

    if (i == 0) return -1;

    int ok = elf_run_named(kname, argc, argv);
    return ok ? 0 : -1;
}

void syscall_handler_main(syscall_regs_t* regs) {
    if (regs == 0) return;

    switch (regs->eax) {
        case SYS_WRITE:
            regs->eax = (unsigned int)sys_write_impl(
                            (const char*)regs->ebx, regs->ecx);
            break;

        case SYS_EXIT:
            sys_exit_impl((int)regs->ebx);
            /* does not return */
            break;

        case SYS_GET_TICKS:
            regs->eax = sys_get_ticks_impl();
            break;

        case SYS_PUTC:
            regs->eax = (unsigned int)sys_putc_impl(regs->ebx);
            break;

        case SYS_READ:
            regs->eax = (unsigned int)sys_read_impl(
                            (char*)regs->ebx, regs->ecx);
            break;

        case SYS_YIELD:
            /*
             * Pass regs as the esp argument — regs IS the kernel stack
             * pointer of the saved isr128_stub frame.  sched_yield_now
             * saves this as the resume point for this process.
             */
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