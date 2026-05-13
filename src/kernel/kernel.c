#include "terminal.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "gdt.h"
#include "idt.h"
#include "shell.h"
#include "memory.h"
#include "boot_info.h"
#include "pmm.h"
#include "input.h"
#include "paging.h"
#include "scheduler.h"
#include "process.h"
#include "ata.h"
#include "ext2.h"
#include "pci.h"
#include "e1000.h"
#include "fb_console.h"
#include "../drivers/tcp.h"

extern unsigned char bss_end;

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

static void boot_halt(void) {
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

static void boot_splash_begin(void) {
    terminal_clear();
    terminal_puts("============================================================\n");
    terminal_puts(" SmallOS boot diagnostics\n");
    terminal_puts(" protected-mode kernel startup\n");
    terminal_puts("============================================================\n");
}

static void boot_splash_status(const char* status, const char* name) {
    terminal_puts("boot: ");
    terminal_puts(status);
    terminal_putc(' ');
    terminal_puts(name);
    terminal_putc('\n');
}

static void boot_splash_pass(const char* name) {
    boot_splash_status("PASS", name);
}

static void boot_splash_warn(const char* name) {
    boot_splash_status("WARN", name);
}

static void boot_splash_fail(const char* name, const char* detail) {
    boot_splash_status("FAIL", name);
    if (detail) {
        terminal_puts("boot: ");
        terminal_puts(detail);
        terminal_putc('\n');
    }
    boot_halt();
}

static void boot_splash_expect(int cond, const char* name, const char* detail) {
    if (!cond) {
        boot_splash_fail(name, detail);
    }

    boot_splash_pass(name);
}

static void boot_splash_boot_info(void) {
    boot_splash_expect(boot_info_validate(),
                       "boot info: SMOS v2 contract",
                       "boot info header or memory-map bounds are invalid");

    if (boot_info_e820_valid()) {
        boot_splash_pass("memory map: E820 available");
    } else {
        boot_splash_warn("memory map: using fixed PMM range");
    }
}

static void kernel_selfcheck(void) {
    unsigned int esp = kernel_read_esp();

    /*
     * These are the startup invariants the hand-rolled boot path must
     * already have established before we let the shell come up.
     */
    boot_splash_expect(kernel_read_tr() == SEG_TSS,
                       "gdt: TSS selector loaded",
                       "task register does not contain the TSS selector");
    boot_splash_expect(tss_get_kernel_stack() == KERNEL_BOOT_STACK_TOP,
                       "tss: ESP0 uses boot stack top",
                       "TSS ESP0 does not match KERNEL_BOOT_STACK_TOP");
    boot_splash_expect(memory_get_heap_top() >= 0x100000u &&
                       memory_get_heap_top() < KERNEL_BOOT_STACK_TOP - PMM_FRAME_SIZE &&
                       (memory_get_heap_top() & 0xFFFu) == 0,
                       "memory: heap starts after kernel BSS",
                       "heap top is outside the kernel arena or not page-aligned");
    boot_splash_expect(pmm_free_count() > 0 &&
                       pmm_free_count() <= PMM_NUM_FRAMES,
                       "pmm: free frame baseline sane",
                       "PMM free frame count is outside the managed bitmap");
    boot_splash_expect(esp <= KERNEL_BOOT_STACK_TOP &&
                       esp > KERNEL_BOOT_STACK_TOP - 0x1000u,
                       "stack: ESP inside boot stack page",
                       "kernel stack pointer is outside the boot stack page");
}

void kernel_main(void) {
    terminal_init();
    boot_splash_begin();
    boot_splash_pass("terminal: VGA text and serial console");
    boot_splash_boot_info();

    gdt_init();
    paging_init();
    boot_splash_expect(paging_get_kernel_pd() != 0,
                       "paging: kernel directory installed",
                       "kernel page directory was not initialized");

    memory_init(PAGE_ALIGN((unsigned int)&bss_end));
    pmm_init();
    kernel_selfcheck();

    if (fb_console_init()) {
        boot_splash_begin();
        boot_splash_pass("terminal: VGA text and serial console");
        boot_splash_pass("terminal: framebuffer console");
        boot_splash_boot_info();
    } else {
#ifndef SMALLOS_FORCE_VGA_BACKEND
        boot_splash_warn("terminal: framebuffer unavailable, VGA text active");
#endif
    }

    input_init();
    keyboard_init();
    boot_splash_expect(keyboard_buf_available() == 0 &&
                       keyboard_get_waiting_process() == 0 &&
                       input_available() == 0 &&
                       input_get_waiting_process() == 0,
                       "keyboard: input state reset",
                       "keyboard buffer or waiter slot was not reset");
    if (mouse_init()) {
        boot_splash_pass("mouse: PS/2 packet stream enabled");
    } else {
        boot_splash_warn("mouse: PS/2 unavailable");
    }

    timer_init(SMALLOS_TIMER_HZ);
    boot_splash_expect(timer_get_hz() == SMALLOS_TIMER_HZ &&
                       timer_get_ticks() == 0,
                       "timer: PIT frequency configured",
                       "timer frequency or tick counter did not initialize");

    idt_init();
    boot_splash_pass("idt: IRQ and syscall gates installed");

    sched_init();
    boot_splash_expect(sched_current() == 0,
                       "scheduler: run queue reset",
                       "scheduler selected a current task before start");

    /*
     * ATA PIO driver — software reset + wait ready.
     * Must be called before ext2_init() and before sti.
     */
    boot_splash_expect(ata_init(),
                       "ata: primary channel ready",
                       "ATA primary channel failed to become ready");

    /*
     * PCI bus scan — discover devices now so NIC work can bind to the
     * real hardware/QEMU enumeration path later.
     */
    pci_init();
    boot_splash_pass("pci: config-space scan complete");

    /*
     * e1000 NIC — bind to the Intel 82540EM QEMU exposes and set up
     * basic DMA rings so networking can grow from a known-good device.
     */
    if (e1000_init()) {
        boot_splash_pass("e1000: Intel 82540EM ready");
    } else {
        boot_splash_warn("e1000: Intel 82540EM not present");
    }

    /*
     * TCP service — drain NIC RX, maintain the tiny TCP state machine,
     * and wake socket waiters used by guest TCP services.
     */
    boot_splash_expect(tcp_init(),
                       "tcp: service task queued",
                       "TCP service task could not be created");

    /*
     * ext2 filesystem — discovers the partition from MBR entry 1 and
     * validates the superblock before user programs can be loaded.
     */
    boot_splash_expect(ext2_init(),
                       "ext2: volume mounted",
                       "ext2 volume failed superblock or partition validation");

    process_t* shell_proc = process_create_kernel_task("shell", shell_task_main);

    if (!shell_proc) {
        boot_splash_fail("shell: task created",
                         "kernel could not allocate the shell process");
    }
    boot_splash_pass("shell: task created");

    if (!sched_enqueue(shell_proc)) {
        boot_splash_fail("shell: task queued",
                         "kernel could not enqueue the shell process");
    }
    boot_splash_pass("shell: task queued");

    boot_splash_expect(process_start_reaper(),
                       "reaper: task queued",
                       "zombie reaper task could not be started");

    terminal_puts("SmallOS ready\n");

    __asm__ __volatile__("sti");
    sched_start(shell_proc);

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
