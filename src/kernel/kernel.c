#include "terminal.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "gdt.h"
#include "idt.h"
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
#include "usb_storage.h"
#include "fb_console.h"
#include "elf_loader.h"
#include "vfs.h"
#include "ports.h"
#include "../drivers/dhcp.h"
#include "../drivers/net.h"
#include "../drivers/tcp.h"
#include "../drivers/ntp.h"

extern unsigned char bss_end;

#define BOOT_LOG_PATH "/var/log/boot.txt"
#define BOOT_LOG_CAPACITY 32768u
static char s_boot_log[BOOT_LOG_CAPACITY];
static unsigned int s_boot_log_len = 0;
static int s_boot_log_enabled = 1;
static int s_boot_log_fs_ready = 0;
static int s_boot_log_terminal_hooked = 0;
static int s_boot_log_read_only_notice = 0;
static int s_boot_log_line_start = 1;
static char s_boot_terminal_prefix[80];

static unsigned long long boot_read_cycles(void) {
    unsigned int lo;
    unsigned int hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

static void boot_log_append_raw_char(char ch) {
    if (s_boot_log_len + 1u >= BOOT_LOG_CAPACITY) return;
    s_boot_log[s_boot_log_len++] = ch;
    s_boot_log[s_boot_log_len] = '\0';
}

static void boot_log_append_raw(const char* s) {
    if (!s) return;
    while (*s) {
        boot_log_append_raw_char(*s++);
    }
}

static void boot_log_append_raw_uint(unsigned int value) {
    char buf[10];
    unsigned int i = 0;

    do {
        buf[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value > 0u && i < sizeof(buf));
    while (i > 0u) {
        boot_log_append_raw_char(buf[--i]);
    }
}

static void boot_log_append_raw_hex64(unsigned long long value) {
    static const char hex[] = "0123456789ABCDEF";

    boot_log_append_raw("0x");
    for (int i = 15; i >= 0; i--) {
        boot_log_append_raw_char(hex[(value >> (unsigned int)(i * 4)) & 0xFu]);
    }
}

static unsigned int boot_log_elapsed_ms(void) {
    unsigned int hz = timer_get_hz();
    unsigned int ticks = timer_get_ticks();
    unsigned int seconds;
    unsigned int rem;

    if (hz == 0u) return 0u;
    seconds = ticks / hz;
    rem = ticks % hz;
    return seconds * 1000u + (rem * 1000u) / hz;
}

static void boot_log_append_prefix(void) {
    boot_log_append_raw("[ms=");
    boot_log_append_raw_uint(boot_log_elapsed_ms());
    boot_log_append_raw(" tick=");
    boot_log_append_raw_uint(timer_get_ticks());
    boot_log_append_raw(" cyc=");
    boot_log_append_raw_hex64(boot_read_cycles());
    boot_log_append_raw("] ");
}

static void boot_prefix_char(char** cursor, char* end, char ch) {
    if (*cursor + 1 >= end) return;
    **cursor = ch;
    *cursor += 1;
    **cursor = '\0';
}

static void boot_prefix_string(char** cursor, char* end, const char* s) {
    if (!s) return;
    while (*s) {
        boot_prefix_char(cursor, end, *s++);
    }
}

static void boot_prefix_uint(char** cursor, char* end, unsigned int value) {
    char buf[10];
    unsigned int i = 0;

    do {
        buf[i++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value > 0u && i < sizeof(buf));
    while (i > 0u) {
        boot_prefix_char(cursor, end, buf[--i]);
    }
}

static void boot_prefix_hex64(char** cursor, char* end, unsigned long long value) {
    static const char hex[] = "0123456789ABCDEF";

    boot_prefix_string(cursor, end, "0x");
    for (int i = 15; i >= 0; i--) {
        boot_prefix_char(cursor, end, hex[(value >> (unsigned int)(i * 4)) & 0xFu]);
    }
}

static const char* boot_terminal_prefix(void) {
    char* cursor = s_boot_terminal_prefix;
    char* end = s_boot_terminal_prefix + sizeof(s_boot_terminal_prefix);

    s_boot_terminal_prefix[0] = '\0';
    boot_prefix_string(&cursor, end, "[ms=");
    boot_prefix_uint(&cursor, end, boot_log_elapsed_ms());
    boot_prefix_string(&cursor, end, " tick=");
    boot_prefix_uint(&cursor, end, timer_get_ticks());
    boot_prefix_string(&cursor, end, " cyc=");
    boot_prefix_hex64(&cursor, end, boot_read_cycles());
    boot_prefix_string(&cursor, end, "] ");
    return s_boot_terminal_prefix;
}

static void boot_log_append_char(char ch) {
    if (!s_boot_log_enabled) return;
    if (s_boot_log_line_start) {
        boot_log_append_prefix();
        s_boot_log_line_start = 0;
    }
    boot_log_append_raw_char(ch);
    if (ch == '\n') {
        s_boot_log_line_start = 1;
    }
}

static void boot_log_append(const char* s) {
    if (!s) return;
    while (*s) {
        boot_log_append_char(*s++);
    }
}

static void boot_log_terminal_hook(char ch) {
    boot_log_append_char(ch);
}

static void boot_log_capture_begin(void) {
    terminal_set_line_prefix_hook(boot_terminal_prefix);
    terminal_set_output_hook(boot_log_terminal_hook);
    s_boot_log_terminal_hooked = 1;
}

static void boot_log_capture_end(void) {
    terminal_set_output_hook(0);
    terminal_set_line_prefix_hook(0);
    s_boot_log_terminal_hooked = 0;
}

static void boot_putc(char ch) {
    terminal_putc(ch);
    if (!s_boot_log_terminal_hooked) {
        boot_log_append_char(ch);
    }
}

static void boot_puts(const char* s) {
    terminal_puts(s);
    if (!s_boot_log_terminal_hooked) {
        boot_log_append(s);
    }
}

static void boot_log_save(void) {
    if (!s_boot_log_fs_ready || s_boot_log_len == 0u) return;
    if (ext2_is_read_only()) {
        if (!s_boot_log_read_only_notice) {
            s_boot_log_read_only_notice = 1;
            terminal_puts("boot: INFO boot log not saved on read-only filesystem\n");
        }
        return;
    }
    if (!vfs_write_path(BOOT_LOG_PATH, (const u8*)s_boot_log, s_boot_log_len)) {
        terminal_puts("boot: WARN /var/log/boot.txt write failed\n");
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
    terminal_set_display_enabled(1);
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
    keyboard_debug_state_t keyboard_dbg;
    mouse_debug_state_t mouse_dbg;

    usb_debug_snapshot(&usb_dbg);
    keyboard_debug_snapshot(&keyboard_dbg);
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

    boot_puts("diag: keyboard ps2=");
    boot_put_uint(keyboard_dbg.ps2_init_ok);
    boot_puts(" init=");
    boot_put_uint(keyboard_dbg.ps2_init_step);
    boot_putc('/');
    boot_put_uint(keyboard_dbg.ps2_init_fail);
    boot_puts(" cfg=");
    boot_put_hex(keyboard_dbg.ps2_config_before);
    boot_putc('/');
    boot_put_hex(keyboard_dbg.ps2_config_after);
    boot_puts(" irq=");
    boot_put_uint(keyboard_dbg.irq_count);
    boot_puts(" inject=");
    boot_put_uint(keyboard_dbg.injected_scancode_count);
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

typedef struct {
    unsigned char master;
    unsigned char slave;
} boot_pic_masks_t;

static boot_pic_masks_t boot_pic_timer_only_begin(void) {
    boot_pic_masks_t masks;

    masks.master = inb(0x21);
    masks.slave = inb(0xA1);
    outb(0x21, 0xFE);  /* IRQ0 only: keep keyboard/process IRQs out of early boot. */
    outb(0xA1, 0xFF);
    return masks;
}

static void boot_pic_timer_only_end(boot_pic_masks_t masks) {
    outb(0x21, masks.master);
    outb(0xA1, masks.slave);
}

static void boot_mount_ext2(int ata_ready) {
    boot_pic_masks_t pic_masks;

    /*
     * Prefer the writable ATA path for IDE-style disks. Some hardware can
     * reset the ATA channel successfully but still fail real sector reads, so
     * the loader-provided ext2 copy remains a second-chance mount fallback.
     *
     * Keep only IRQ0 running here so boot timestamps and USB/OHCI waits use
     * real PIT time without allowing keyboard/process IRQ paths to run before
     * the scheduler has a current task.
     */
    pic_masks = boot_pic_timer_only_begin();
    __asm__ __volatile__("sti");

    if (ata_ready) {
        ext2_use_block_device(ata_block_device());
        if (ext2_init()) {
            __asm__ __volatile__("cli");
            boot_pic_timer_only_end(pic_masks);
            boot_splash_pass("ext2: volume mounted");
            return;
        }
        boot_splash_warn("ext2: ATA mount failed");
    }

    if (usb_storage_init()) {
        block_device_t* usb_dev = usb_storage_block_device();
        if (usb_dev) {
            ext2_use_block_device(usb_dev);
            if (ext2_init()) {
                __asm__ __volatile__("cli");
                boot_pic_timer_only_end(pic_masks);
                if (usb_dev->read_only) {
                    boot_puts("usb: storage read-only\n");
                }
                boot_splash_pass("ext2: volume mounted from USB");
                return;
            }
            boot_splash_warn("ext2: USB mount failed");
        }
    }

    if (boot_info_ramdisk_valid()) {
        ext2_use_boot_ramdisk(1);
        __asm__ __volatile__("cli");
        boot_pic_timer_only_end(pic_masks);
        if (ata_ready) {
            boot_splash_warn("storage: using boot ramdisk fallback");
        } else {
            boot_splash_warn("ata: unavailable, using boot ramdisk");
        }
        boot_splash_pass("storage: boot ramdisk fallback");
        boot_splash_expect(ext2_init(),
                           "ext2: volume mounted",
                           "ext2 volume failed on boot ramdisk");
        return;
    }

    __asm__ __volatile__("cli");
    boot_pic_timer_only_end(pic_masks);
    boot_splash_fail("ext2: volume mounted",
                     "ext2 volume failed superblock or partition validation");
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

static void boot_show_splash(void) {
    char* splash_argv[] = { "bootsplash", "boot/splash.bmp", 0 };
    process_t* splash_proc = elf_run_named("bin/bootsplash", 2, splash_argv);

    if (splash_proc) {
        process_claim_for_wait(splash_proc);
        process_wait(splash_proc);
    }
}

static void boot_sequence_task_main(void) {
    usb_debug_state_t usb_dbg;
    keyboard_debug_state_t keyboard_dbg;

    boot_print_hardware_diag_summary();
    boot_log_save();

    /*
     * Keep this ordering for real Wyse USB boot: the shell image is loaded
     * from USB storage before boot HID is claimed, but it stays suspended so
     * all boot/HID diagnostics are captured before the first prompt appears.
     */
    char* user_shell_argv[] = { "shell", 0 };
    process_t* user_shell_proc = elf_run_named_suspended("bin/shell", 1, user_shell_argv);

    if (usb_probe_hid()) {
        boot_puts("usb: boot HID ready\n");
    } else {
        boot_puts("usb: WARN boot HID unavailable\n");
    }
    if (usb_start_service()) {
        boot_puts("usb: HID service task queued\n");
    } else {
        boot_puts("usb: WARN HID service task unavailable\n");
    }

    boot_log_save();
    boot_log_capture_end();
    boot_show_splash();
    terminal_set_display_enabled(1);
    terminal_clear();
    terminal_puts("SmallOS ready\n");
    usb_debug_snapshot(&usb_dbg);
    keyboard_debug_snapshot(&keyboard_dbg);
    terminal_puts("Input: ");
    if (usb_dbg.keyboard_active) {
        terminal_puts("USB boot keyboard ready on port ");
        terminal_put_uint(usb_dbg.keyboard_port);
    } else {
        terminal_puts("no USB boot keyboard yet");
    }
    terminal_puts("  controllers=");
    terminal_put_uint(usb_dbg.controller_count);
    terminal_puts(" uhci=");
    terminal_put_uint(usb_dbg.uhci_count);
    terminal_puts(" ohci=");
    terminal_put_uint(usb_dbg.ohci_count);
    terminal_puts(" ehci=");
    terminal_put_uint(usb_dbg.ehci_count);
    terminal_puts(" xhci=");
    terminal_put_uint(usb_dbg.xhci_count);
    terminal_putc('\n');
    terminal_puts("Input: usb_mouse=");
    terminal_put_uint(usb_dbg.mouse_active);
    terminal_puts(" port=");
    terminal_put_uint(usb_dbg.mouse_port);
    terminal_puts(" ep=");
    terminal_put_uint(usb_dbg.mouse_endpoint);
    terminal_puts(" pkt=");
    terminal_put_uint(usb_dbg.mouse_packet_size);
    terminal_puts(" int=");
    terminal_put_uint(usb_dbg.mouse_interval);
    terminal_puts(" polls=");
    terminal_put_uint(usb_dbg.mouse_poll_count);
    terminal_puts(" reports=");
    terminal_put_uint(usb_dbg.mouse_report_count);
    terminal_puts(" cc=");
    terminal_put_hex(usb_dbg.mouse_last_cc);
    terminal_putc('\n');
    terminal_puts("Input: ps2=");
    terminal_put_uint(keyboard_dbg.ps2_init_ok);
    terminal_puts(" irq=");
    terminal_put_uint(keyboard_dbg.irq_count);
    terminal_puts(" usb_ep=");
    terminal_put_uint(usb_dbg.keyboard_endpoint);
    terminal_puts(" pkt=");
    terminal_put_uint(usb_dbg.keyboard_packet_size);
    terminal_puts(" int=");
    terminal_put_uint(usb_dbg.keyboard_interval);
    terminal_puts(" polls=");
    terminal_put_uint(usb_dbg.keyboard_poll_count);
    terminal_puts(" reports=");
    terminal_put_uint(usb_dbg.keyboard_report_count);
    terminal_puts(" cc=");
    terminal_put_hex(usb_dbg.keyboard_last_cc);
    terminal_putc('\n');

    if (user_shell_proc) {
        terminal_puts("Launching user shell\n");
        /*
         * Install the console reader before releasing the suspended shell.
         * Real PS/2 and USB keyboard input can arrive as soon as the prompt is
         * visible, so the consumer must already point at the user process.
         */
        process_set_foreground(user_shell_proc);
        user_shell_proc->state = PROCESS_STATE_RUNNING;
        process_wait(user_shell_proc);
        terminal_puts("User shell exited; no kernel shell fallback is linked\n");
    } else {
        terminal_puts("User shell unavailable; no kernel shell fallback is linked\n");
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
    boot_log_capture_begin();
    terminal_set_display_enabled(0);
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

    int ata_ready = ata_init();
    if (ata_ready) {
        boot_splash_pass("ata: primary channel ready");
    } else if (boot_info_ramdisk_valid()) {
        boot_splash_warn("ata: unavailable, boot ramdisk available");
    } else {
        boot_splash_warn("ata: unavailable, probing USB storage");
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

    boot_sync_clock();

    boot_mount_ext2(ata_ready);
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

    __asm__ __volatile__("cli");
    sched_start(boot_proc);

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}
