#include "terminal.h"
#include "keyboard.h"
#include "timer.h"
#include "gdt.h"
#include "idt.h"
#include "shell.h"
#include "memory.h"
#include "pmm.h"
#include "paging.h"
#include "scheduler.h"
#include "process.h"
#include "ata.h"
#include "fat16.h"

void kernel_main(void) {
    terminal_init();
    terminal_puts("SimpleOS\n");

    gdt_init();
    paging_init();

    memory_init(0x100000);
    pmm_init();

    keyboard_init();
    timer_init(100);
    idt_init();

    sched_init();

    /*
     * ATA PIO driver — software reset + wait ready.
     * Must be called before fat16_init() and before sti.
     */
    ata_init();

    /*
     * FAT16 filesystem — reads FAT16_LBA from sector 0 offset 504
     * (patched by Makefile), validates BPB geometry.
     */
    fat16_init();

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