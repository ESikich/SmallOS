#include "commands.h"
#include "shell.h"
#include "terminal.h"
#include "elf_loader.h"
#include "memory.h"
#include "boot_info.h"
#include "pmm.h"
#include "ata.h"
#include "dhcp.h"
#include "e1000.h"
#include "usb.h"
#include "mouse.h"
#include "net.h"
#include "tcp.h"
#include "arp.h"
#include "ipv4.h"
#include "vfs.h"
#include "process.h"
#include "socket.h"
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

static void cmd_meminfo(command_t* cmd) {
    (void)cmd;

    unsigned int heap_base = memory_get_heap_base();
    unsigned int heap_top  = memory_get_heap_top();
    unsigned int heap_used = heap_top - heap_base;

    terminal_puts("heap:   base ");
    terminal_put_hex(heap_base);
    terminal_puts("  top ");
    terminal_put_hex(heap_top);
    terminal_puts("  used ");
    terminal_put_uint(heap_used / 1024);
    terminal_puts(" KB\n");

    unsigned int free_frames  = pmm_free_count();
    unsigned int total_frames = PMM_NUM_FRAMES;
    unsigned int used_frames  = total_frames - free_frames;

    terminal_puts("frames: ");
    terminal_put_uint(free_frames);
    terminal_puts(" free / ");
    terminal_put_uint(total_frames);
    terminal_puts(" total  (");
    terminal_put_uint(free_frames * 4);
    terminal_puts(" KB / ");
    terminal_put_uint(total_frames * 4);
    terminal_puts(" KB)\n");

    terminal_puts("used:   ");
    terminal_put_uint(used_frames);
    terminal_puts(" frames (");
    terminal_put_uint(used_frames * 4);
    terminal_puts(" KB)\n");

    terminal_puts("e820:   ");
    if (boot_info_e820_valid()) {
        terminal_put_uint(boot_info_e820_count());
        terminal_puts(" entries\n");
    } else {
        terminal_puts("unavailable\n");
    }
}

static void terminal_put_u64_hex(u64 value) {
    u32 high = (u32)(value >> 32);
    u32 low = (u32)value;

    if (high) {
        terminal_put_hex(high);
        terminal_putc('_');
        terminal_put_hex(low);
    } else {
        terminal_put_hex(low);
    }
}

static const char* e820_type_name(u32 type) {
    switch (type) {
    case 1: return "usable";
    case 2: return "reserved";
    case 3: return "acpi";
    case 4: return "nvs";
    case 5: return "bad";
    default: return "other";
    }
}

static void cmd_memmap(command_t* cmd) {
    (void)cmd;

    if (!boot_info_e820_valid()) {
        terminal_puts("memmap: E820 unavailable\n");
        return;
    }

    const boot_info_t* info = boot_info_get();
    terminal_puts("memmap: ");
    terminal_put_uint(info->e820_count);
    terminal_puts(" E820 entries\n");

    for (u32 i = 0; i < info->e820_count; i++) {
        const boot_e820_entry_t* ent = &info->e820[i];
        terminal_put_uint(i);
        terminal_puts(": base ");
        terminal_put_u64_hex(ent->base);
        terminal_puts("  length ");
        terminal_put_u64_hex(ent->length);
        terminal_puts("  type ");
        terminal_put_uint(ent->type);
        terminal_puts(" ");
        terminal_puts(e820_type_name(ent->type));
        terminal_putc('\n');
    }
}

static void cmd_netinfo(command_t* cmd) {
    socket_stats_t socket_stats;
    tcp_stats_t tcp_stats;

    (void)cmd;

    terminal_puts("netinfo: ");
    e1000_print_info();
    net_ipv4_print_config();

    socket_get_stats(&socket_stats);
    terminal_puts("sockets: ");
    terminal_put_uint(socket_stats.used_sockets);
    terminal_putc('/');
    terminal_put_uint(socket_stats.max_sockets);
    terminal_puts(" used tcp=");
    terminal_put_uint(socket_stats.tcp_sockets);
    terminal_puts(" open=");
    terminal_put_uint(socket_stats.open_sockets);
    terminal_puts(" bound=");
    terminal_put_uint(socket_stats.bound_sockets);
    terminal_puts(" listen=");
    terminal_put_uint(socket_stats.listening_sockets);
    terminal_puts(" conn=");
    terminal_put_uint(socket_stats.connected_sockets);
    terminal_putc('\n');

    tcp_get_stats(&tcp_stats);
    terminal_puts("tcp: listeners ");
    terminal_put_uint(tcp_stats.listeners);
    terminal_putc('/');
    terminal_put_uint(tcp_stats.max_listeners);
    terminal_puts(" conns ");
    terminal_put_uint(tcp_stats.connections);
    terminal_putc('/');
    terminal_put_uint(tcp_stats.max_connections);
    terminal_puts(" established=");
    terminal_put_uint(tcp_stats.established_connections);
    terminal_puts(" accepted=");
    terminal_put_uint(tcp_stats.accepted_connections);
    terminal_puts(" pending=");
    terminal_put_uint(tcp_stats.pending_connections);
    terminal_puts(" syn=");
    terminal_put_uint(tcp_stats.syn_recv_connections);
    terminal_puts(" fin=");
    terminal_put_uint(tcp_stats.fin_wait_connections);
    terminal_putc('\n');

    terminal_puts("tcp buffers: rx ");
    terminal_put_uint(tcp_stats.rx_rings);
    terminal_puts(" rings ");
    terminal_put_uint(tcp_stats.rx_bytes);
    terminal_puts(" bytes queued / ");
    terminal_put_uint(tcp_stats.rx_buffer_bytes / 1024u);
    terminal_puts(" KB allocated / ");
    terminal_put_uint(tcp_stats.max_rx_buffer_bytes / 1024u);
    terminal_puts(" KB cap, tx ");
    terminal_put_uint(tcp_stats.tx_rings);
    terminal_puts(" rings ");
    terminal_put_uint(tcp_stats.tx_bytes);
    terminal_puts(" bytes queued / ");
    terminal_put_uint(tcp_stats.tx_buffer_bytes / 1024u);
    terminal_puts(" KB allocated / ");
    terminal_put_uint(tcp_stats.max_tx_buffer_bytes / 1024u);
    terminal_puts(" KB cap\n");
}

static void cmd_netsend(command_t* cmd) {
    (void)cmd;

    if (!e1000_send_test_frame()) {
        terminal_puts("netsend: failed\n");
        return;
    }

    terminal_puts("netsend: queued test frame\n");
}

static void cmd_netrecv(command_t* cmd) {
    (void)cmd;

    if (!net_poll_once()) {
        terminal_puts("netrecv: no packet\n");
        return;
    }

    terminal_puts("netrecv: dispatched packet\n");
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

static int net_route_for_target(u32 target_ip, u32* out_sender_ip, u32* out_next_hop) {
    u32 sender_ip = net_ipv4_local_ip();
    u32 netmask = net_ipv4_netmask();
    u32 gateway = net_ipv4_gateway();
    u32 next_hop = target_ip;

    if (!net_ipv4_is_configured() || sender_ip == 0u) {
        terminal_puts("net: IPv4 is not configured\n");
        return 0;
    }

    if (netmask != 0u && (target_ip & netmask) != (sender_ip & netmask)) {
        if (gateway == 0u) {
            terminal_puts("net: no default gateway\n");
            return 0;
        }
        next_hop = gateway;
    }

    if (out_sender_ip) *out_sender_ip = sender_ip;
    if (out_next_hop) *out_next_hop = next_hop;
    return 1;
}

static void cmd_dhcp(command_t* cmd) {
    (void)cmd;

    if (!dhcp_configure()) {
        terminal_puts("dhcp: failed\n");
        return;
    }
    terminal_puts("dhcp: ok\n");
}

static void cmd_arpgw(command_t* cmd) {
    (void)cmd;

    u32 sender_ip = net_ipv4_local_ip();
    u32 target_ip  = net_ipv4_gateway();
    u8 mac[6];

    if (!net_ipv4_is_configured() || sender_ip == 0u || target_ip == 0u) {
        terminal_puts("arpgw: IPv4 gateway is not configured\n");
        return;
    }

    terminal_puts("arpgw: who-has ");
    arp_print_ip(target_ip);
    terminal_puts(" from ");
    arp_print_ip(sender_ip);
    terminal_putc('\n');

    if (!arp_resolve(sender_ip, target_ip, mac)) {
        terminal_puts("arpgw: no reply\n");
        return;
    }

    terminal_puts("arpgw: ");
    arp_print_ip(target_ip);
    terminal_puts(" is-at ");
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned int i = 0; i < 6; i++) {
        terminal_putc(hex[mac[i] >> 4]);
        terminal_putc(hex[mac[i] & 0xF]);
        if (i != 5) {
            terminal_putc(':');
        }
    }
    terminal_putc('\n');
}

static void cmd_ping_target(const char* label, u32 sender_ip, u32 target_ip, u32 gateway_ip) {
    terminal_puts(label);
    terminal_puts(": ");
    ipv4_print_ip(target_ip);
    terminal_puts(" from ");
    ipv4_print_ip(sender_ip);
    terminal_putc('\n');

    if (!ipv4_ping_via_gateway(sender_ip, target_ip, gateway_ip)) {
        terminal_puts(label);
        terminal_puts(": failed\n");
        return;
    }

    terminal_puts(label);
    terminal_puts(": ok\n");
}

static void cmd_ping(command_t* cmd) {
    u32 sender_ip;
    u32 next_hop;
    u32 target_ip = 0;

    if (cmd->argc < 2) {
        target_ip = net_ipv4_gateway();
        if (target_ip == 0u) {
            terminal_puts("usage: ping <ip>\n");
            return;
        }
    } else if (!ipv4_parse_ip(cmd->argv[1], &target_ip)) {
        terminal_puts("ping: invalid ip\n");
        return;
    }

    if (!net_route_for_target(target_ip, &sender_ip, &next_hop)) {
        return;
    }
    cmd_ping_target("ping", sender_ip, target_ip, next_hop);
}

static void cmd_pinggw(command_t* cmd) {
    (void)cmd;

    if (!net_ipv4_is_configured() || net_ipv4_gateway() == 0u) {
        terminal_puts("pinggw: IPv4 gateway is not configured\n");
        return;
    }
    cmd_ping_target("pinggw", net_ipv4_local_ip(), net_ipv4_gateway(), net_ipv4_gateway());
}

static void cmd_pingpublic(command_t* cmd) {
    u32 sender_ip;
    u32 next_hop;

    (void)cmd;

    if (!net_route_for_target(0x01010101u, &sender_ip, &next_hop)) {
        return;
    }
    cmd_ping_target("pingpublic", sender_ip, 0x01010101u, next_hop);
    terminal_puts("pingpublic: note: some hypervisors do not forward public ICMP\n");
    terminal_puts("pingpublic: use pinggw for the supported gateway check\n");
}

static void cmd_netcheck(command_t* cmd) {
    (void)cmd;

    u8 mac[6];
    u32 sender_ip = net_ipv4_local_ip();
    u32 gateway_ip = net_ipv4_gateway();

    if (!net_ipv4_is_configured() || sender_ip == 0u || gateway_ip == 0u) {
        terminal_puts("netcheck: IPv4 gateway is not configured\n");
        return;
    }

    terminal_puts("netcheck: gateway arp\n");
    if (!arp_resolve(sender_ip, gateway_ip, mac)) {
        terminal_puts("netcheck: gateway arp failed\n");
        return;
    }
    terminal_puts("netcheck: gateway arp ok\n");

    terminal_puts("netcheck: gateway ping\n");
    if (!ipv4_ping(sender_ip, gateway_ip)) {
        terminal_puts("netcheck: gateway ping failed\n");
        return;
    }
    terminal_puts("netcheck: gateway ping ok\n");

    terminal_puts("netcheck: public ping\n");
    if (!ipv4_ping_via_gateway(sender_ip, 0x01010101u, gateway_ip)) {
        terminal_puts("netcheck: public ping failed\n");
        terminal_puts("netcheck: note: some hypervisors do not forward public ICMP\n");
        terminal_puts("netcheck: gateway is ok\n");
        return;
    }
    terminal_puts("netcheck: public ping ok\n");
}

/*
 * ataread <lba>
 *
 * Reads one sector at the given decimal LBA and dumps the first 32 bytes
 * as hex.  Prints boot signature at offsets 510-511 when LBA is 0.
 */
static void cmd_ataread(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("usage: ataread <lba>\n");
        return;
    }

    unsigned int lba = 0;
    const char* s = cmd->argv[1];
    while (*s >= '0' && *s <= '9') {
        lba = lba * 10 + (unsigned int)(*s - '0');
        s++;
    }

    static unsigned char sector[512];
    if (!ata_read_sectors(lba, 1, sector)) {
        terminal_puts("ataread: read failed\n");
        return;
    }

    terminal_puts("lba ");
    terminal_put_uint(lba);
    terminal_puts(" bytes 0-31:\n  ");

    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 32; i++) {
        unsigned char b = sector[i];
        terminal_putc(hex[b >> 4]);
        terminal_putc(hex[b & 0xF]);
        terminal_putc(' ');
        if (i == 15) terminal_puts("\n  ");
    }
    terminal_putc('\n');

    if (lba == 0) {
        enum {
            MBR_PARTITION_TABLE_OFFSET = 446,
            MBR_PARTITION_ENTRY_SIZE = 16,
            MBR_PARTITION_LBA_OFFSET = 8,
            EXT2_PARTITION_ENTRY_INDEX = 1,
        };
        unsigned int entry_off = MBR_PARTITION_TABLE_OFFSET +
                                 EXT2_PARTITION_ENTRY_INDEX * MBR_PARTITION_ENTRY_SIZE;
        terminal_puts("sig: ");
        terminal_put_hex(sector[510]);
        terminal_putc(' ');
        terminal_put_hex(sector[511]);
        terminal_puts(" (expect 0x55 0xAA)\n");
        terminal_puts("ext2 partition lba: ");
        unsigned int partition_lba = sector[entry_off + MBR_PARTITION_LBA_OFFSET]
                                   | ((unsigned int)sector[entry_off + MBR_PARTITION_LBA_OFFSET + 1] << 8)
                                   | ((unsigned int)sector[entry_off + MBR_PARTITION_LBA_OFFSET + 2] << 16)
                                   | ((unsigned int)sector[entry_off + MBR_PARTITION_LBA_OFFSET + 3] << 24);
        terminal_put_uint(partition_lba);
        terminal_putc('\n');
    }
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
    { "meminfo",       "show heap and frame usage",     cmd_meminfo },
    { "memmap",        "show BIOS E820 memory map",     cmd_memmap },
    { "netinfo",       "show PCI NIC status",          cmd_netinfo },
    { "dhcp",          "request IPv4 config via DHCP", cmd_dhcp },
    { "netsend",       "queue a test Ethernet frame",  cmd_netsend },
    { "netrecv",       "poll and dispatch one Ethernet frame", cmd_netrecv },
    { "mousetest",     "print mouse events for 5 seconds", cmd_mousetest },
    { "usbinfo",       "show USB controller power diagnostics", cmd_usbinfo },
    { "usbdiag",       "run USB port and descriptor diagnostics", cmd_usbdiag },
    { "usbports",      "dump passive USB port status", cmd_usbports },
    { "usbpeek",       "read OHCI address-0 device descriptor", cmd_usbpeek },
    { "usbpower",      "try OHCI root-hub port power", cmd_usbpower },
    { "usbmouse",      "probe OHCI boot mouse", cmd_usbmouse },
    { "arpgw",         "resolve the IPv4 gateway via ARP", cmd_arpgw },
    { "ping",          "ping an IPv4 address",        cmd_ping },
    { "pinggw",        "ping the IPv4 gateway",       cmd_pinggw },
    { "pingpublic",    "try public ICMP",             cmd_pingpublic },
    { "netcheck",      "check gateway and public connectivity", cmd_netcheck },
    { "cd",            "change the shell working directory", cmd_cd },
    { "ataread",       "dump raw sector bytes",         cmd_ataread },
    { "runelf",        "run an ext2 ELF and wait",      cmd_runelf },
    { "runelf_nowait", "run an ext2 ELF and return",    cmd_runelf_nowait },
};

#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

static command_entry_t app_commands[] = {
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
