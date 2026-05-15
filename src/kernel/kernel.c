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
#include "usb.h"
#include "fb_console.h"
#include "elf_loader.h"
#include "vfs.h"
#include "../drivers/dhcp.h"
#include "../drivers/net.h"
#include "../drivers/tcp.h"
#include "../drivers/ntp.h"

extern unsigned char bss_end;

#define BOOT_LOG_PATH "var/log/boot.log"
#define BOOT_LOG_CAPACITY 8192u
static char s_boot_log[BOOT_LOG_CAPACITY];
static unsigned int s_boot_log_len = 0;
static int s_boot_log_enabled = 1;
static int s_boot_log_fs_ready = 0;

static void boot_log_append_char(char ch) {
    if (!s_boot_log_enabled) return;
    if (s_boot_log_len + 1u >= BOOT_LOG_CAPACITY) return;
    s_boot_log[s_boot_log_len++] = ch;
    s_boot_log[s_boot_log_len] = '\0';
}

static void boot_log_append(const char* s) {
    if (!s) return;
    while (*s) {
        boot_log_append_char(*s++);
    }
}

static void boot_putc(char ch) {
    terminal_putc(ch);
    boot_log_append_char(ch);
}

static void boot_puts(const char* s) {
    terminal_puts(s);
    boot_log_append(s);
}

static void boot_log_save(void) {
    if (!s_boot_log_fs_ready || s_boot_log_len == 0u) return;
    if (!vfs_write_path(BOOT_LOG_PATH, (const u8*)s_boot_log, s_boot_log_len)) {
        terminal_puts("boot: WARN var/log/boot.log write failed\n");
    }
}

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
    boot_puts("============================================================\n");
    boot_puts(" SmallOS boot diagnostics\n");
    boot_puts(" protected-mode kernel startup\n");
    boot_puts("============================================================\n");
}

static void boot_splash_status(const char* status, const char* name) {
    boot_puts("boot: ");
    boot_puts(status);
    boot_putc(' ');
    boot_puts(name);
    boot_putc('\n');
}

static void boot_splash_pass(const char* name) {
    boot_splash_status("PASS", name);
}

static void boot_splash_terminal_ready(void) {
#ifdef SMALLOS_SERIAL_CONSOLE
    boot_splash_pass("terminal: VGA text and serial console");
#else
    boot_splash_pass("terminal: VGA text console");
#endif
}

static void boot_splash_warn(const char* name) {
    boot_splash_status("WARN", name);
}

static void boot_splash_fail(const char* name, const char* detail) {
    boot_splash_status("FAIL", name);
    if (detail) {
        boot_puts("boot: ");
        boot_puts(detail);
        boot_putc('\n');
    }
    boot_log_save();
    boot_halt();
}

static void boot_splash_expect(int cond, const char* name, const char* detail) {
    if (!cond) {
        boot_splash_fail(name, detail);
    }

    boot_splash_pass(name);
}

static int boot_is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static void boot_put_uint_width(unsigned int value, unsigned int width) {
    char buf[10];
    unsigned int i = 0;

    do {
        buf[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value > 0u && i < sizeof(buf));
    while (i < width && i < sizeof(buf)) {
        buf[i++] = '0';
    }
    while (i > 0u) {
        boot_putc(buf[--i]);
    }
}

static void boot_put_uint(unsigned int value) {
    boot_put_uint_width(value, 1u);
}

static void boot_put_hex(unsigned int value) {
    static const char hex[] = "0123456789ABCDEF";
    boot_puts("0x");
    for (int i = 7; i >= 0; i--) {
        boot_putc(hex[(value >> (unsigned int)(i * 4)) & 0xFu]);
    }
}

static void boot_print_utc_time(unsigned int unix_time) {
    static const unsigned int month_days[] = {
        31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u
    };
    unsigned int days = unix_time / 86400u;
    unsigned int rem = unix_time % 86400u;
    int year = 1970;
    unsigned int month = 0;

    while (1) {
        unsigned int yd = boot_is_leap_year(year) ? 366u : 365u;
        if (days < yd) break;
        days -= yd;
        year++;
    }

    while (month < 12u) {
        unsigned int md = month_days[month];
        if (month == 1u && boot_is_leap_year(year)) md++;
        if (days < md) break;
        days -= md;
        month++;
    }

    boot_puts("ntp: time ");
    boot_put_uint_width((unsigned int)year, 4u);
    boot_putc('-');
    boot_put_uint_width(month + 1u, 2u);
    boot_putc('-');
    boot_put_uint_width(days + 1u, 2u);
    boot_putc(' ');
    boot_put_uint_width(rem / 3600u, 2u);
    boot_putc(':');
    boot_put_uint_width((rem / 60u) % 60u, 2u);
    boot_putc(':');
    boot_put_uint_width(rem % 60u, 2u);
    boot_puts(" UTC\n");
}

static void boot_print_hardware_diag_summary(void) {
    usb_debug_state_t usb_dbg;
    mouse_debug_state_t mouse_dbg;

    usb_debug_snapshot(&usb_dbg);
    mouse_debug_snapshot(&mouse_dbg);

    boot_puts("diag: build usb-hid-diag1\n");
    boot_puts("diag: usb controllers=");
    boot_put_uint(usb_dbg.controller_count);
    boot_puts(" uhci=");
    boot_put_uint(usb_dbg.uhci_count);
    boot_puts(" ohci=");
    boot_put_uint(usb_dbg.ohci_count);
    boot_puts(" ehci=");
    boot_put_uint(usb_dbg.ehci_count);
    boot_puts(" xhci=");
    boot_put_uint(usb_dbg.xhci_count);
    boot_puts(" last_prog=");
    boot_put_hex(usb_dbg.last_prog_if);
    boot_puts(" last_bar=");
    boot_put_hex(usb_dbg.last_bar);
    boot_putc('\n');

    boot_puts("diag: mouse ready=");
    boot_put_uint(mouse_dbg.ready);
    boot_puts(" init=");
    boot_put_uint(mouse_dbg.init_step);
    boot_putc('/');
    boot_put_uint(mouse_dbg.init_fail);
    boot_puts(" cfg=");
    boot_put_hex(mouse_dbg.config_before);
    boot_putc('/');
    boot_put_hex(mouse_dbg.config_after);
    boot_puts(" irq=");
    boot_put_uint(mouse_dbg.irq_count);
    boot_puts(" bytes=");
    boot_put_uint(mouse_dbg.byte_count);
    boot_puts(" packets=");
    boot_put_uint(mouse_dbg.packet_count);
    boot_putc('\n');
}

static void boot_sync_clock(void) {
    unsigned int unix_time = 0;

    boot_puts("ntp: syncing clock\n");
    __asm__ __volatile__("sti");
    if (ntp_sync(NTP_DEFAULT_SERVER_IP, &unix_time)) {
        timer_set_realtime_seconds(unix_time);
        __asm__ __volatile__("cli");
        boot_splash_pass("ntp: clock synchronized");
        boot_print_utc_time(unix_time);
    } else {
        __asm__ __volatile__("cli");
        boot_splash_warn("ntp: clock sync failed");
    }
}

static void boot_configure_network(void) {
    boot_puts("dhcp: configuring IPv4\n");
    __asm__ __volatile__("sti");
    if (dhcp_configure()) {
        __asm__ __volatile__("cli");
        boot_splash_pass("dhcp: IPv4 lease acquired");
    } else {
        __asm__ __volatile__("cli");
        boot_splash_warn("dhcp: IPv4 lease unavailable");
    }
}

static void boot_splash_boot_info(void) {
    boot_splash_expect(boot_info_validate(),
                       "boot info: SMOS v3 contract",
                       "boot info header or memory-map bounds are invalid");

    if (boot_info_e820_valid()) {
        boot_splash_pass("memory map: E820 available");
    } else {
        boot_splash_warn("memory map: using fixed PMM range");
    }
}

static void boot_splash_replay_after_framebuffer(void) {
    int old_log_enabled = s_boot_log_enabled;

    s_boot_log_enabled = 0;
    boot_splash_begin();
    boot_splash_terminal_ready();
    s_boot_log_enabled = old_log_enabled;

    boot_splash_pass("terminal: framebuffer console");

    s_boot_log_enabled = 0;
    boot_splash_boot_info();
    s_boot_log_enabled = old_log_enabled;
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

static void boot_sequence_task_main(void) {
    char* splash_argv[] = { "bootsplash", "boot/splash.bmp", 0 };
    process_t* splash_proc = elf_run_named("bin/bootsplash", 2, splash_argv);

    if (splash_proc) {
        process_claim_for_wait(splash_proc);
        process_wait(splash_proc);
    }

    boot_print_hardware_diag_summary();
    boot_log_save();

    boot_puts("SmallOS ready\n");
    boot_log_save();

    char* user_shell_argv[] = { "shell", 0 };
    process_t* user_shell_proc = elf_run_named("bin/shell", 1, user_shell_argv);
    if (user_shell_proc) {
        boot_puts("Launching user shell\n");
        process_wait(user_shell_proc);
        boot_puts("User shell exited; starting kernel shell fallback\n");
    } else {
        boot_puts("User shell unavailable; starting kernel shell fallback\n");
    }

    process_t* shell_proc = process_create_kernel_task("shell", shell_task_main);

    if (!shell_proc) {
        boot_splash_fail("shell: task created",
                         "kernel could not allocate the shell process");
    }

    if (!sched_enqueue(shell_proc)) {
        boot_splash_fail("shell: task queued",
                         "kernel could not enqueue the shell process");
    }

    process_t* current = sched_current();
    if (current) {
        current->state = PROCESS_STATE_WAITING;
    }

    for (;;) {
        __asm__ __volatile__("sti; hlt");
    }
}

void kernel_main(void) {
    terminal_init();
    boot_splash_begin();
    boot_splash_terminal_ready();
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
        boot_splash_replay_after_framebuffer();
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
        mouse_debug_state_t mouse_dbg;
        mouse_debug_snapshot(&mouse_dbg);
        boot_splash_warn("mouse: PS/2 unavailable");
        boot_puts("mouse: init_step=");
        boot_put_uint(mouse_dbg.init_step);
        boot_puts(" init_fail=");
        boot_put_uint(mouse_dbg.init_fail);
        boot_puts(" cfg=");
        boot_put_hex(mouse_dbg.config_before);
        boot_putc('/');
        boot_put_hex(mouse_dbg.config_after);
        boot_putc('\n');
    }

    usb_init();
    boot_splash_pass("usb: passive controller probe complete");

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

    if (boot_info_ramdisk_valid()) {
        boot_splash_pass("storage: boot ramdisk available");
    } else {
        /*
         * ATA PIO driver — software reset + wait ready.
         * Must be called before ext2_init() and before sti when no boot
         * ramdisk was provided by stage 2.
         */
        boot_splash_expect(ata_init(),
                           "ata: primary channel ready",
                           "ATA primary channel failed to become ready");
    }

    /*
     * PCI bus scan — discover devices now so NIC work can bind to the
     * real hardware/QEMU enumeration path later.
     */
    pci_init();
    boot_splash_pass("pci: config-space scan complete");

    /*
     * e1000 NIC — bind to a supported Intel PRO/1000 device and set up
     * basic DMA rings so networking can grow from a known-good device.
     */
    if (e1000_init()) {
        boot_splash_pass("e1000: Intel PRO/1000 ready");
        boot_configure_network();
    } else {
        boot_splash_warn("e1000: Intel PRO/1000 not present");
        net_ipv4_clear_config();
    }

    /*
     * TCP service — drain NIC RX, maintain the tiny TCP state machine,
     * and wake socket waiters used by guest TCP services.
     */
    boot_splash_expect(tcp_init(),
                       "tcp: service task queued",
                       "TCP service task could not be created");

    boot_splash_expect(usb_start_service(),
                       "usb: HID service task queued",
                       "USB HID service task could not be created");

    boot_sync_clock();

    /*
     * ext2 filesystem — discovers the partition from MBR entry 1 and
     * validates the superblock before user programs can be loaded.
     */
    boot_splash_expect(ext2_init(),
                       "ext2: volume mounted",
                       "ext2 volume failed superblock or partition validation");
    s_boot_log_fs_ready = 1;
    boot_log_save();

    process_t* boot_proc = process_create_kernel_task("bootseq", boot_sequence_task_main);

    if (!boot_proc) {
        boot_splash_fail("boot sequence: task created",
                         "kernel could not allocate the boot sequence process");
    }
    boot_splash_pass("boot sequence: task created");

    if (!sched_enqueue(boot_proc)) {
        boot_splash_fail("boot sequence: task queued",
                         "kernel could not enqueue the boot sequence process");
    }
    boot_splash_pass("boot sequence: task queued");

    boot_splash_expect(process_start_reaper(),
                       "reaper: task queued",
                       "zombie reaper task could not be started");
    boot_log_save();

    __asm__ __volatile__("sti");
    sched_start(boot_proc);

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
