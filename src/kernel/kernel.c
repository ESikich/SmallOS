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

    __asm__ __volatile__("sti");

    ramdisk_init(RAMDISK_BASE);

    shell_init();

    for (;;) {}
}