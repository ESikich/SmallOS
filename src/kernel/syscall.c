#include "syscall.h"
#include "terminal.h"
#include "timer.h"
#include "paging.h"
#include "elf_loader.h"
#include "keyboard.h"

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

/*
 * sys_exit_impl()
 *
 * Terminate the current ring-3 process and return to the shell.
 *
 * With ring-3 execution there is no longer a kernel call frame to return
 * through — elf_run_image() ended with an `iret` and has no stack frame
 * waiting. Instead we:
 *
 *   1. Restore the kernel page directory (so kernel data is writable again).
 *   2. Destroy the process page directory and free its PMM-backed frames.
 *   3. Long-jump back to the shell via the saved return context stored in
 *      elf_loader.c.
 *
 * elf_process_exit() (declared in elf_loader.h) performs steps 1–3 and
 * does not return — it longjmps to the context saved before the iret.
 */
static void sys_exit_impl(int code) {
    (void)code;
    elf_process_exit();
    /* unreachable */
}

static unsigned int sys_get_ticks_impl(void) {
    return timer_get_ticks();
}

/*
 * sys_read_impl(buf, len)
 *
 * Reads up to `len` bytes of keyboard input into `buf`.
 * Spins (HLT-waiting) until `len` bytes have been received.
 * Input is echoed to the terminal so the user can see what they typed.
 * A newline ('\n') terminates early — it is included in the returned data.
 *
 * Returns the number of bytes read, or -1 on error.
 *
 * Safety: buf is a user virtual address — valid only because the process PD
 * is still active when this syscall runs (CR3 has not been switched yet).
 */
static int sys_yield_impl(void) {
    /*
     * Transitional implementation.
     *
     * Kernel tasks are scheduler-owned, but runelf user processes still run
     * through the older foreground iret/longjmp path and are not safely
     * schedulable yet.  Until ELF processes become real scheduler-owned
     * tasks, SYS_YIELD is accepted as a no-op so user test programs can be
     * built and exercised without corrupting control flow.
     */
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
            regs->eax = (unsigned int)sys_yield_impl();
            break;

        default:
            regs->eax = SYSCALL_ERR_INVALID;
            break;
    }
}
