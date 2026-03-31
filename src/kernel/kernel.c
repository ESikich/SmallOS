#include "terminal.h"
#include "keyboard.h"
#include "timer.h"
#include "gdt.h"
#include "idt.h"
#include "shell.h"
#include "memory.h"
#include "pmm.h"
#include "paging.h"
#include "ramdisk.h"
#include "scheduler.h"
#include "process.h"

#define RAMDISK_BASE 0x10000u

void kernel_main(void) {
    terminal_init();
    terminal_puts("SimpleOS\n");

    gdt_init();
    paging_init();

    /*
     * Bump allocator owns 0x100000–0x1FFFFF (kernel structures).
     * PMM owns 0x200000+ (user frames).  Ranges are disjoint — no
     * ordering dependency between the two allocators.
     */
    memory_init(0x100000);
    pmm_init();

    keyboard_init();
    timer_init(100);
    idt_init();

    /*
     * Initialise the scheduler before enabling interrupts.  This build
     * starts scheduling by creating an explicit shell kernel task,
     * rather than treating the boot stack as an implicit shell slot.
     *
     * We intentionally do not add an idle task yet.  The goal of this
     * increment is only to prove that the shell itself can run as a
     * schedulable kernel task while preserving keyboard behaviour.
     */
    sched_init();

    ramdisk_init(RAMDISK_BASE);

    process_t* shell_proc = process_create_kernel_task("shell", shell_task_main);

    if (!shell_proc) {
        terminal_puts("kernel: failed to create shell task\n");
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }

    if (!sched_enqueue(shell_proc)) {
        terminal_puts("kernel: failed to enqueue shell task\n");
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }

    __asm__ __volatile__("sti");
    sched_start(shell_proc);

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}