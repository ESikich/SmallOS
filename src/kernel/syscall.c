#include "syscall.h"
#include "terminal.h"
#include "timer.h"
#include "paging.h"
#include "keyboard.h"
#include "scheduler.h"
#include "../exec/elf_loader.h"

#define SYSCALL_ERR_INVALID   ((unsigned int)-1)
#define SYSCALL_MAX_WRITE_LEN 4096u
#define EXEC_NAME_MAX         32
#define SCHED_RESUME_RETADDR_OFFSET 8u

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

static int sys_read_impl(char* buf, unsigned int len) {
    if (buf == 0) return -1;
    if (len == 0) return 0;

    unsigned int n = 0;

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
 * Spawn a named ELF program asynchronously.
 *
 * The new program is created, seeded with a scheduler bootstrap context,
 * enqueued, and execution returns immediately to the caller. The spawned
 * process later exits through SYS_EXIT, which hands control to the
 * scheduler via sched_exit_current().
 *
 * name and argv are user virtual addresses — valid here because the
 * caller's CR3 is still active when the syscall fires.
 *
 * name is copied into a small kernel-side buffer before calling
 * elf_run_named(), because the loader later switches page directories
 * and must not depend on the caller's user pointer remaining valid.
 *
 * Returns 0 on success, -1 if the program was not found or failed to
 * load.
 */
static int sys_exec_impl(const char* name, int argc, char** argv) {
    if (name == 0) return -1;

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
                            (char*)regs->ebx, regs->ecx);
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