#include "commands.h"
#include "shell.h"
#include "terminal.h"
#include "elf_loader.h"
#include "usb.h"
#include "mouse.h"
#include "vfs.h"
#include "process.h"
#include "timer.h"
#include "klib.h"

static void print_command_list(void);
static int run_app_command(command_t* cmd, const char* program);
static int resolve_app_command_path(const char* name, char* out, unsigned int out_size);
static int resolve_shell_path_arg(const char* input, char* out, unsigned int out_size);
static int command_name_has_path_sep(const char* name);
static int path_has_dot(const char* path);
static int path_copy3(char* out, unsigned int out_size, const char* a, const char* b, const char* c);
static void terminal_put_cwd(void);
static void terminal_put_int(int value);

static void terminal_put_int(int value) {
    if (value < 0) {
        terminal_putc('-');
        terminal_put_uint((unsigned int)(-value));
        return;
    }
    terminal_put_uint((unsigned int)value);
}

static int parse_uint_arg(const char* s, unsigned int* out) {
    unsigned int value = 0;

    if (!s || !*s || !out) {
        return 0;
    }

    while (*s) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        value = value * 10u + (unsigned int)(*s - '0');
        s++;
    }

    *out = value;
    return 1;
}

static void cmd_help(command_t* cmd) {
    (void)cmd;
    terminal_puts("Commands:\n");
    print_command_list();
}

static void cmd_clear(command_t* cmd) {
    (void)cmd;
    terminal_clear();
}

static int run_app_command(command_t* cmd, const char* program) {
    __asm__ __volatile__("cli");
    process_t* proc = elf_run_named(program, cmd->argc, cmd->argv);
    if (proc) {
        k_strncpy(proc->cwd, shell_get_cwd(), sizeof(proc->cwd));
        process_claim_for_wait(proc);
    }
    __asm__ __volatile__("sti");

    if (!proc) {
        terminal_puts(program);
        terminal_puts(": failed\n");
        return 0;
    }

    process_wait(proc);
    return 1;
}

static int command_name_has_path_sep(const char* name) {
    if (!name) {
        return 0;
    }

    for (const char* p = name; *p; p++) {
        if (*p == '/' || *p == '\\') {
            return 1;
        }
    }

    return 0;
}

static int path_has_dot(const char* path) {
    if (!path) {
        return 0;
    }

    for (const char* p = path; *p; p++) {
        if (*p == '.') {
            return 1;
        }
    }

    return 0;
}

static int path_copy3(char* out, unsigned int out_size, const char* a, const char* b, const char* c) {
    unsigned int pos = 0;
    const char* parts[3] = { a, b, c };

    if (!out || out_size == 0u) {
        return 0;
    }

    for (unsigned int part = 0; part < 3u; part++) {
        const char* s = parts[part];
        if (!s) {
            continue;
        }

        while (*s) {
            if (pos + 1u >= out_size) {
                out[0] = '\0';
                return 0;
            }
            out[pos++] = *s++;
        }
    }

    out[pos] = '\0';
    return 1;
}

static int executable_path_exists(const char* path) {
    u32 size = 0;
    int is_dir = 0;

    return vfs_stat(path, &size, &is_dir) && !is_dir;
}

static int resolve_app_command_path(const char* name, char* out, unsigned int out_size) {
    char candidate[SHELL_PATH_MAX];
    static const char* const search_prefixes[] = {
        "bin/",
        "usr/bin/",
        "usr/sbin/",
    };

    if (!name || name[0] == '\0') {
        return 0;
    }

    if (command_name_has_path_sep(name)) {
        if (!shell_resolve_path(name, candidate, sizeof(candidate))) {
            return 0;
        }
        if (executable_path_exists(candidate)) {
            k_strncpy(out, candidate, out_size);
            return 1;
        }
        if (!path_has_dot(candidate) &&
            path_copy3(candidate, sizeof(candidate), candidate, ".elf", 0) &&
            executable_path_exists(candidate)) {
            k_strncpy(out, candidate, out_size);
            return 1;
        }
        return 0;
    }

    for (unsigned int i = 0; i < sizeof(search_prefixes) / sizeof(search_prefixes[0]); i++) {
        if (path_copy3(candidate, sizeof(candidate), search_prefixes[i], name, ".elf") &&
            executable_path_exists(candidate)) {
            k_strncpy(out, candidate, out_size);
            return 1;
        }
    }

    if (path_has_dot(name) && executable_path_exists(name)) {
        k_strncpy(out, name, out_size);
        return 1;
    }

    if (!path_has_dot(name) &&
        path_copy3(candidate, sizeof(candidate), name, ".elf", 0) &&
        executable_path_exists(candidate)) {
        k_strncpy(out, candidate, out_size);
        return 1;
    }

    return 0;
}

static int resolve_shell_path_arg(const char* input, char* out, unsigned int out_size) {
    if (!shell_resolve_path(input, out, out_size)) {
        terminal_puts("ext2: invalid path: ");
        terminal_puts(input ? input : "");
        terminal_putc('\n');
        return 0;
    }

    return 1;
}

static void terminal_put_cwd(void) {
    const char* cwd = shell_get_cwd();
    if (!cwd || cwd[0] == '\0') {
        terminal_putc('/');
        return;
    }

    terminal_putc('/');
    terminal_puts(cwd);
}

static void cmd_cd(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("usage: cd <path>\n");
        return;
    }

    if (!shell_set_cwd(cmd->argv[1])) {
        terminal_puts("cd: failed\n");
        return;
    }

    terminal_puts("cd: ");
    terminal_put_cwd();
    terminal_putc('\n');
}

static void cmd_mousetest(command_t* cmd) {
    sys_mouse_state_t mouse;
    mouse_debug_state_t before;
    mouse_debug_state_t after;
    unsigned int deadline;
    unsigned int last_sequence;
    unsigned int events = 0;
    int usb_open;

    (void)cmd;

    usb_open = usb_mouse_open_port(0u);
    if (usb_open) {
        terminal_puts("mousetest: usb poll active\n");
    }

    if (!mouse_read_state(&mouse)) {
        terminal_puts("mousetest: mouse unavailable\n");
        mouse_debug_snapshot(&after);
        terminal_puts("mousetest: ready=");
        terminal_put_uint(after.ready);
        terminal_puts(" init_step=");
        terminal_put_uint(after.init_step);
        terminal_puts(" init_fail=");
        terminal_put_uint(after.init_fail);
        terminal_puts(" cfg=");
        terminal_put_hex(after.config_before);
        terminal_putc('/');
        terminal_put_hex(after.config_after);
        terminal_puts(" irq=");
        terminal_put_uint(after.irq_count);
        terminal_puts(" bytes=");
        terminal_put_uint(after.byte_count);
        terminal_puts(" aux=");
        terminal_put_uint(after.aux_status_count);
        terminal_puts(" packets=");
        terminal_put_uint(after.packet_count);
        terminal_puts(" packet_size=");
        terminal_put_uint(after.packet_size);
        terminal_puts(" device_id=");
        terminal_put_uint(after.device_id);
        terminal_putc('\n');
        if (usb_open) {
            usb_mouse_close();
        }
        return;
    }

    last_sequence = mouse.sequence;
    mouse_debug_snapshot(&before);
    deadline = timer_get_ticks() + timer_ms_to_ticks_round_up(5000u);
    terminal_puts("mousetest: move/click mouse for 5 seconds\n");

    while ((int)(timer_get_ticks() - deadline) < 0) {
        if (usb_open) {
            int usb_poll = usb_mouse_poll_once();
            if (usb_poll < 0) {
                usb_open = 0;
            }
        }
        if (!mouse_read_state(&mouse)) {
            terminal_puts("mousetest: mouse became unavailable\n");
            if (usb_open) {
                usb_mouse_close();
            }
            return;
        }
        if (mouse.sequence != last_sequence ||
            mouse.dx != 0 || mouse.dy != 0 || mouse.wheel != 0) {
            last_sequence = mouse.sequence;
            events++;
            terminal_puts("mousetest: seq=");
            terminal_put_uint(mouse.sequence);
            terminal_puts(" dx=");
            terminal_put_int(mouse.dx);
            terminal_puts(" dy=");
            terminal_put_int(mouse.dy);
            terminal_puts(" wheel=");
            terminal_put_int(mouse.wheel);
            terminal_puts(" buttons=");
            terminal_put_uint(mouse.buttons);
            terminal_putc('\n');
        }
    }

    terminal_puts("mousetest: events=");
    terminal_put_uint(events);
    terminal_putc('\n');
    mouse_debug_snapshot(&after);
    terminal_puts("mousetest: irq=");
    terminal_put_uint(after.irq_count - before.irq_count);
    terminal_puts(" bytes=");
    terminal_put_uint(after.byte_count - before.byte_count);
    terminal_puts(" aux=");
    terminal_put_uint(after.aux_status_count - before.aux_status_count);
    terminal_puts(" packets=");
    terminal_put_uint(after.packet_count - before.packet_count);
    terminal_puts(" vmware=");
    terminal_put_uint(after.vmware_packet_count - before.vmware_packet_count);
    terminal_puts(" vmware_on=");
    terminal_put_uint(after.vmware_enabled);
    terminal_puts(" packet_size=");
    terminal_put_uint(after.packet_size);
    terminal_puts(" device_id=");
    terminal_put_uint(after.device_id);
    terminal_puts(" ready=");
    terminal_put_uint(after.ready);
    terminal_puts(" init=");
    terminal_put_uint(after.init_step);
    terminal_putc('/');
    terminal_put_uint(after.init_fail);
    terminal_puts(" syncdrop=");
    terminal_put_uint(after.sync_drop_count - before.sync_drop_count);
    terminal_puts(" overflow=");
    terminal_put_uint(after.overflow_drop_count - before.overflow_drop_count);
    terminal_putc('\n');
    if (usb_open) {
        usb_mouse_close();
    }
}

static void cmd_usbinfo(command_t* cmd) {
    usb_debug_state_t usb;

    (void)cmd;

    usb_debug_snapshot(&usb);
    terminal_puts("usbinfo: controllers=");
    terminal_put_uint(usb.controller_count);
    terminal_puts(" uhci=");
    terminal_put_uint(usb.uhci_count);
    terminal_puts(" ohci=");
    terminal_put_uint(usb.ohci_count);
    terminal_puts(" ehci=");
    terminal_put_uint(usb.ehci_count);
    terminal_puts(" xhci=");
    terminal_put_uint(usb.xhci_count);
    terminal_puts(" powered_ports=");
    terminal_put_uint(usb.powered_port_count);
    terminal_putc('\n');

    terminal_puts("usbinfo: hid keyboard=");
    terminal_put_uint(usb.keyboard_active);
    terminal_puts(" port=");
    terminal_put_uint(usb.keyboard_port);
    terminal_puts(" ep=");
    terminal_put_uint(usb.keyboard_endpoint);
    terminal_puts(" pkt=");
    terminal_put_uint(usb.keyboard_packet_size);
    terminal_puts(" int=");
    terminal_put_uint(usb.keyboard_interval);
    terminal_puts(" polls=");
    terminal_put_uint(usb.keyboard_poll_count);
    terminal_puts(" reports=");
    terminal_put_uint(usb.keyboard_report_count);
    terminal_puts(" fail=");
    terminal_put_uint(usb.keyboard_fail_count);
    terminal_puts(" cc=");
    terminal_put_hex(usb.keyboard_last_cc);
    terminal_putc('\n');

    terminal_puts("usbinfo: hid mouse=");
    terminal_put_uint(usb.mouse_active);
    terminal_puts(" port=");
    terminal_put_uint(usb.mouse_port);
    terminal_puts(" ep=");
    terminal_put_uint(usb.mouse_endpoint);
    terminal_puts(" pkt=");
    terminal_put_uint(usb.mouse_packet_size);
    terminal_puts(" int=");
    terminal_put_uint(usb.mouse_interval);
    terminal_puts(" polls=");
    terminal_put_uint(usb.mouse_poll_count);
    terminal_puts(" reports=");
    terminal_put_uint(usb.mouse_report_count);
    terminal_puts(" fail=");
    terminal_put_uint(usb.mouse_fail_count);
    terminal_puts(" cc=");
    terminal_put_hex(usb.mouse_last_cc);
    terminal_putc('\n');

    terminal_puts("usbinfo: hid service=");
    terminal_put_uint(usb.service_active);
    terminal_puts(" storage=");
    terminal_put_uint(usb.storage_active);
    terminal_puts(" port=");
    terminal_put_uint(usb.storage_port);
    terminal_putc('\n');

    terminal_puts("usbinfo: last=");
    terminal_put_hex(((unsigned int)usb.last_bus << 8) |
                     ((unsigned int)usb.last_slot << 3) |
                     (unsigned int)usb.last_func);
    terminal_puts(" prog=");
    terminal_put_hex(usb.last_prog_if);
    terminal_puts(" bar=");
    terminal_put_hex(usb.last_bar);
    terminal_puts(" ports=");
    terminal_put_uint(usb.last_ports);
    terminal_puts(" status=");
    terminal_put_hex(usb.last_port_status0);
    terminal_putc('/');
    terminal_put_hex(usb.last_port_status1);
    terminal_putc('\n');
}

static void cmd_usbports(command_t* cmd) {
    (void)cmd;

    usb_dump_ports();
}

static void cmd_usbdiag(command_t* cmd) {
    (void)cmd;

    usb_diag();
}

static void cmd_usbpeek(command_t* cmd) {
    unsigned int port;

    if (cmd->argc < 2 || !parse_uint_arg(cmd->argv[1], &port)) {
        terminal_puts("usbpeek usage: usbpeek <ohci-port>\n");
        return;
    }

    usb_peek_port(port);
}

static void cmd_usbpower(command_t* cmd) {
    unsigned int powered;

    (void)cmd;

    terminal_puts("usbpower: OHCI root-hub power only\n");
    powered = usb_power_ohci_ports();
    terminal_puts("usbpower: total_powered=");
    terminal_put_uint(powered);
    terminal_putc('\n');
}

static void cmd_usbmouse(command_t* cmd) {
    unsigned int port = 0u;
    unsigned int seconds = 3u;

    if (cmd->argc >= 2 && !parse_uint_arg(cmd->argv[1], &port)) {
        terminal_puts("usbmouse usage: usbmouse [port] [seconds]\n");
        return;
    }
    if (cmd->argc >= 3 && !parse_uint_arg(cmd->argv[2], &seconds)) {
        terminal_puts("usbmouse usage: usbmouse [port] [seconds]\n");
        return;
    }

    (void)usb_mouse_poll_port(seconds, port);
}

static void cmd_runelf(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("Usage: runelf <n>\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    if (!resolve_shell_path_arg(cmd->argv[1], path, sizeof(path))) {
        return;
    }

    __asm__ __volatile__("cli");
    process_t* proc = elf_run_named(path, cmd->argc - 1, &cmd->argv[1]);
    if (proc) {
        k_strncpy(proc->cwd, shell_get_cwd(), sizeof(proc->cwd));
        process_claim_for_wait(proc);
    }

    if (!proc) {
        __asm__ __volatile__("sti");
        terminal_puts("runelf: failed\n");
        return;
    }

    __asm__ __volatile__("sti");
    process_wait(proc);
}

static void cmd_runelf_nowait(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("Usage: runelf_nowait <n>\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    if (!resolve_shell_path_arg(cmd->argv[1], path, sizeof(path))) {
        return;
    }

    process_t* proc = elf_run_named(path, cmd->argc - 1, &cmd->argv[1]);
    if (proc) {
        k_strncpy(proc->cwd, shell_get_cwd(), sizeof(proc->cwd));
    }
    if (!proc) {
        terminal_puts("runelf_nowait: failed\n");
    }
}

static command_entry_t commands[] = {
    { "help",          "show shell commands",           cmd_help },
    { "clear",         "clear the screen",              cmd_clear },
    { "mousetest",     "print mouse events for 5 seconds", cmd_mousetest },
    { "usbinfo",       "show USB controller power diagnostics", cmd_usbinfo },
    { "usbdiag",       "run USB port and descriptor diagnostics", cmd_usbdiag },
    { "usbports",      "dump passive USB port status", cmd_usbports },
    { "usbpeek",       "read OHCI address-0 device descriptor", cmd_usbpeek },
    { "usbpower",      "try OHCI root-hub port power", cmd_usbpower },
    { "usbmouse",      "probe OHCI boot mouse", cmd_usbmouse },
    { "cd",            "change the shell working directory", cmd_cd },
    { "runelf",        "run an ext2 ELF and wait",      cmd_runelf },
    { "runelf_nowait", "run an ext2 ELF and return",    cmd_runelf_nowait },
};

#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

static command_entry_t app_commands[] = {
    { "meminfo",       "show heap and frame usage",    0 },
    { "memmap",        "show BIOS E820 memory map",    0 },
    { "netinfo",       "show PCI NIC status",          0 },
    { "dhcp",          "request IPv4 config via DHCP", 0 },
    { "netsend",       "queue a test Ethernet frame",  0 },
    { "netrecv",       "poll and dispatch one Ethernet frame", 0 },
    { "arpgw",         "resolve the IPv4 gateway via ARP", 0 },
    { "ping",          "ping an IPv4 address",         0 },
    { "pinggw",        "ping the IPv4 gateway",        0 },
    { "pingpublic",    "try public ICMP",              0 },
    { "netcheck",      "check gateway and public connectivity", 0 },
    { "ataread",       "dump raw sector bytes",        0 },
    { "echo",          "print arguments via ELF",       0 },
    { "about",         "show the OS version via ELF",   0 },
    { "halt",          "halt the machine via ELF",      0 },
    { "reboot",        "reboot the machine via ELF",    0 },
    { "uptime",        "show tick and second counts via ELF", 0 },
    { "pwd",           "print the shell working directory", 0 },
    { "ls",            "list an ext2 directory",       0 },
    { "tree",          "print an ext2 directory tree", 0 },
    { "fsread",        "dump ext2 file bytes",         0 },
    { "cat",           "print an ext2 file",           0 },
    { "more",          "page a file or stdin",         0 },
    { "mkdir",         "create an ext2 directory",     0 },
    { "rmdir",         "remove an ext2 directory",     0 },
    { "rm",            "remove an ext2 file",          0 },
    { "touch",         "create or truncate an ext2 file", 0 },
    { "cp",            "copy an ext2 file",            0 },
    { "mv",            "move or rename an ext2 entry", 0 },
    { "edit",          "edit an ext2 text file",       0 },
};

#define APP_COMMAND_COUNT (sizeof(app_commands) / sizeof(app_commands[0]))

unsigned int commands_count(void) {
    return COMMAND_COUNT + APP_COMMAND_COUNT;
}

const char* commands_name_at(unsigned int index) {
    if (index < COMMAND_COUNT) {
        return commands[index].name;
    }

    index -= COMMAND_COUNT;
    if (index < APP_COMMAND_COUNT) {
        return app_commands[index].name;
    }

    return 0;
}

static void print_command_list(void) {
    for (unsigned int i = 0; i < commands_count(); i++) {
        const command_entry_t* entry = i < COMMAND_COUNT
                                     ? &commands[i]
                                     : &app_commands[i - COMMAND_COUNT];
        terminal_puts("  ");
        terminal_puts(entry->name);

        unsigned int name_len = 0;
        while (entry->name[name_len]) name_len++;
        if (name_len < 16) {
            for (unsigned int pad = name_len; pad < 16; pad++) {
                terminal_putc(' ');
            }
        } else {
            terminal_putc(' ');
        }

        terminal_puts(entry->help);
        terminal_putc('\n');
    }
}

void commands_execute(command_t* cmd) {
    if (cmd->argc == 0) return;

    for (unsigned int i = 0; i < COMMAND_COUNT; i++) {
        if (k_strcmp(cmd->argv[0], commands[i].name)) {
            commands[i].fn(cmd);
            return;
        }
    }

    char app_path[SHELL_PATH_MAX];
    if (resolve_app_command_path(cmd->argv[0], app_path, sizeof(app_path))) {
        run_app_command(cmd, app_path);
        return;
    }

    terminal_puts("Unknown command: ");
    terminal_puts(cmd->argv[0]);
    terminal_putc('\n');
}
