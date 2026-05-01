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
#include "pci.h"
#include "e1000.h"
#include "../drivers/tcp.h"

static unsigned short kernel_read_tr(void) {
    unsigned short tr;
    __asm__ __volatile__("str %0" : "=r"(tr));
    return tr;
}

static unsigned int kernel_read_esp(void) {
    unsigned int esp;
    __asm__ __volatile__("mov %%esp, %0" : "=r"(esp));
    return esp;
}

static void kernel_selfcheck_fail(const char* msg) {
    terminal_puts("kernel: selfcheck FAIL: ");
    terminal_puts(msg);
    terminal_putc('\n');

    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

static void kernel_selfcheck_expect(int cond, const char* msg) {
    if (!cond) {
        kernel_selfcheck_fail(msg);
    }
}

static void kernel_selfcheck(void) {
    unsigned int esp = kernel_read_esp();

    terminal_puts("kernel: selfcheck\n");

    /*
     * These are the startup invariants the hand-rolled boot path must
     * already have established before we let the shell come up.
     */
    kernel_selfcheck_expect(kernel_read_tr() == SEG_TSS,
                            "task register not loaded with the TSS selector");
    kernel_selfcheck_expect(tss_get_kernel_stack() == KERNEL_BOOT_STACK_TOP,
                            "TSS kernel stack top mismatch");
    kernel_selfcheck_expect(memory_get_heap_top() == 0x100000u,
                            "heap top moved before startup allocations");
    kernel_selfcheck_expect(pmm_free_count() == PMM_NUM_FRAMES,
                            "PMM free frame count mismatch");
    kernel_selfcheck_expect(esp <= KERNEL_BOOT_STACK_TOP,
                            "kernel stack pointer above boot stack top");
    kernel_selfcheck_expect(esp > KERNEL_BOOT_STACK_TOP - 0x1000u,
                            "kernel stack pointer left the boot stack page");

    terminal_puts("kernel: selfcheck PASS\n");
}

void kernel_main(void) {
    terminal_init();
    terminal_puts("SmallOS\n");

    gdt_init();
    paging_init();

    memory_init(0x100000);
    pmm_init();
    kernel_selfcheck();

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
     * PCI bus scan — discover devices now so NIC work can bind to the
     * real hardware/QEMU enumeration path later.
     */
    pci_init();

    /*
     * e1000 NIC — bind to the Intel 82540EM QEMU exposes and set up
     * basic DMA rings so networking can grow from a known-good device.
     */
    e1000_init();

    /*
     * TCP bring-up — start a tiny passive listener so the kernel has a
     * live end-to-end TCP path before the socket ABI lands.
     */
    tcp_init();

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

    process_start_reaper();

    __asm__ __volatile__("sti");
    sched_start(shell_proc);

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
