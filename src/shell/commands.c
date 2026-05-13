#include "commands.h"
#include "shell.h"
#include "terminal.h"
#include "elf_loader.h"
#include "memory.h"
#include "boot_info.h"
#include "pmm.h"
#include "ata.h"
#include "e1000.h"
#include "net.h"
#include "tcp.h"
#include "arp.h"
#include "ipv4.h"
#include "ext2.h"
#include "vfs.h"
#include "process.h"
#include "scheduler.h"
#include "socket.h"
#include "klib.h"

#define SHELL_JOB_MAX 8
#define SHELL_SIGTERM 15

static void print_command_list(void);
static int run_app_command(command_t* cmd, const char* program);
static int run_pipeline(command_t* cmd);
static int resolve_app_command_path(const char* name, char* out, unsigned int out_size);
static int resolve_shell_path_arg(const char* input, char* out, unsigned int out_size);
static int command_name_has_path_sep(const char* name);
static int path_has_dot(const char* path);
static int path_copy3(char* out, unsigned int out_size, const char* a, const char* b, const char* c);
static void terminal_put_cwd(void);

typedef struct {
    int used;
    unsigned int id;
    process_t* proc;
    char command[SHELL_PATH_MAX];
} shell_job_t;

static shell_job_t s_jobs[SHELL_JOB_MAX];
static unsigned int s_next_job_id = 1;

static const char* job_state_name(process_t* proc) {
    if (!proc) return "missing";

    switch (proc->state) {
    case PROCESS_STATE_RUNNING:
        return "running";
    case PROCESS_STATE_WAITING:
        return "waiting";
    case PROCESS_STATE_SLEEPING:
        return "sleeping";
    case PROCESS_STATE_ZOMBIE:
        return "done";
    case PROCESS_STATE_EXITED:
        return "exited";
    case PROCESS_STATE_UNUSED:
    default:
        return "unknown";
    }
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

static shell_job_t* job_find(unsigned int id) {
    for (unsigned int i = 0; i < SHELL_JOB_MAX; i++) {
        if (s_jobs[i].used && s_jobs[i].id == id) {
            return &s_jobs[i];
        }
    }
    return 0;
}

static void job_clear(shell_job_t* job) {
    if (!job) return;
    job->used = 0;
    job->id = 0;
    job->proc = 0;
    job->command[0] = '\0';
}

static shell_job_t* job_alloc(process_t* proc, const char* command) {
    for (unsigned int i = 0; i < SHELL_JOB_MAX; i++) {
        if (!s_jobs[i].used) {
            s_jobs[i].used = 1;
            s_jobs[i].id = s_next_job_id++;
            s_jobs[i].proc = proc;
            k_strncpy(s_jobs[i].command, command, sizeof(s_jobs[i].command));
            return &s_jobs[i];
        }
    }
    return 0;
}

static int launch_background_job(command_t* cmd, const char* usage_name) {
    if (cmd->argc < 2) {
        terminal_puts("Usage: ");
        terminal_puts(usage_name);
        terminal_puts(" <n>\n");
        return 0;
    }

    char path[SHELL_PATH_MAX];
    if (!resolve_shell_path_arg(cmd->argv[1], path, sizeof(path))) {
        return 0;
    }

    __asm__ __volatile__("cli");
    process_t* proc = elf_run_named(path, cmd->argc - 1, &cmd->argv[1]);
    if (proc) {
        k_strncpy(proc->cwd, shell_get_cwd(), sizeof(proc->cwd));
        process_claim_for_wait(proc);
    }
    __asm__ __volatile__("sti");

    if (!proc) {
        terminal_puts(usage_name);
        terminal_puts(": failed\n");
        return 0;
    }

    shell_job_t* job = job_alloc(proc, path);
    if (!job) {
        terminal_puts(usage_name);
        terminal_puts(": job table full\n");
        sched_kill(proc, 0);
        process_destroy(proc);
        return 0;
    }

    terminal_puts("[");
    terminal_put_uint(job->id);
    terminal_puts("] ");
    terminal_puts(path);
    terminal_putc('\n');
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

static void cmd_pwd(command_t* cmd) {
    (void)cmd;
    terminal_puts("pwd: ");
    terminal_put_cwd();
    terminal_putc('\n');
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

static void cmd_arpgw(command_t* cmd) {
    (void)cmd;

    /* QEMU user networking defaults to 10.0.2.2 as the gateway. */
    u32 sender_ip = 0x0A00020Fu; /* 10.0.2.15 */
    u32 target_ip  = 0x0A000202u; /* 10.0.2.2  */
    u8 mac[6];

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
    if (cmd->argc < 2) {
        terminal_puts("usage: ping <ip>\n");
        return;
    }

    u32 target_ip = 0;
    if (!ipv4_parse_ip(cmd->argv[1], &target_ip)) {
        terminal_puts("ping: invalid ip\n");
        return;
    }

    /* QEMU user networking defaults to 10.0.2.15/24 with gateway 10.0.2.2. */
    cmd_ping_target("ping", 0x0A00020Fu, target_ip, 0x0A000202u);
}

static void cmd_pinggw(command_t* cmd) {
    (void)cmd;

    /* QEMU user networking defaults to 10.0.2.15/24 with gateway 10.0.2.2. */
    cmd_ping_target("pinggw", 0x0A00020Fu, 0x0A000202u, 0x0A000202u);
}

static void cmd_pingpublic(command_t* cmd) {
    (void)cmd;

    /* QEMU user networking usually does not forward public ICMP. */
    cmd_ping_target("pingpublic", 0x0A00020Fu, 0x01010101u, 0x0A000202u);
    terminal_puts("pingpublic: note: QEMU user networking may not support public ICMP\n");
    terminal_puts("pingpublic: use pinggw or ping 10.0.2.2 for the supported NAT check\n");
}

static void cmd_netcheck(command_t* cmd) {
    (void)cmd;

    u8 mac[6];

    terminal_puts("netcheck: gateway arp\n");
    if (!arp_resolve(0x0A00020Fu, 0x0A000202u, mac)) {
        terminal_puts("netcheck: gateway arp failed\n");
        return;
    }
    terminal_puts("netcheck: gateway arp ok\n");

    terminal_puts("netcheck: gateway ping\n");
    if (!ipv4_ping(0x0A00020Fu, 0x0A000202u)) {
        terminal_puts("netcheck: gateway ping failed\n");
        return;
    }
    terminal_puts("netcheck: gateway ping ok\n");

    terminal_puts("netcheck: public ping\n");
    if (!ipv4_ping_via_gateway(0x0A00020Fu, 0x01010101u, 0x0A000202u)) {
        terminal_puts("netcheck: public ping failed\n");
        terminal_puts("netcheck: note: QEMU user networking may not support public ICMP\n");
        terminal_puts("netcheck: gateway is ok; TCP hostfwd smokes are the supported user-net test\n");
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

static void cmd_fsread(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("usage: fsread <n>\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    if (!resolve_shell_path_arg(cmd->argv[1], path, sizeof(path))) {
        return;
    }

    unsigned int size = 0;
    const unsigned char* data = vfs_load_file(path, &size);
    if (!data) {
        terminal_puts("fsread: load failed\n");
        return;
    }

    terminal_puts("fsread: ");
    terminal_puts(cmd->argv[1]);
    terminal_puts("  ");
    terminal_put_uint(size);
    terminal_puts(" bytes\nfirst 16: ");

    static const char hex[] = "0123456789ABCDEF";
    unsigned int show = size < 16 ? size : 16;
    for (unsigned int i = 0; i < show; i++) {
        terminal_putc(hex[data[i] >> 4]);
        terminal_putc(hex[data[i] & 0xF]);
        terminal_putc(' ');
    }
    terminal_putc('\n');
}

static void cmd_cat(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("usage: cat <path>\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    if (!resolve_shell_path_arg(cmd->argv[1], path, sizeof(path))) {
        return;
    }

    unsigned int size = 0;
    const unsigned char* data = vfs_load_file(path, &size);
    if (!data) {
        terminal_puts("cat: failed\n");
        return;
    }

    for (unsigned int i = 0; i < size; i++) {
        terminal_putc((char)data[i]);
    }
}

static void cmd_mkdir(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("usage: mkdir <path>\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    if (!resolve_shell_path_arg(cmd->argv[1], path, sizeof(path))) {
        return;
    }

    if (!vfs_mkdir(path)) {
        terminal_puts("mkdir: failed\n");
        return;
    }

    terminal_puts("mkdir: ");
    terminal_puts(cmd->argv[1]);
    terminal_putc('\n');
}

static void cmd_rmdir(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("usage: rmdir <path>\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    if (!resolve_shell_path_arg(cmd->argv[1], path, sizeof(path))) {
        return;
    }

    if (!vfs_rmdir(path)) {
        terminal_puts("rmdir: failed\n");
        return;
    }

    terminal_puts("rmdir: ");
    terminal_puts(cmd->argv[1]);
    terminal_putc('\n');
}

static void cmd_rm(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("usage: rm <path>\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    if (!resolve_shell_path_arg(cmd->argv[1], path, sizeof(path))) {
        return;
    }

    if (!vfs_unlink(path)) {
        terminal_puts("rm: failed\n");
        return;
    }

    terminal_puts("rm: ");
    terminal_puts(cmd->argv[1]);
    terminal_putc('\n');
}

static void cmd_touch(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("usage: touch <path>\n");
        return;
    }

    char path[SHELL_PATH_MAX];
    if (!resolve_shell_path_arg(cmd->argv[1], path, sizeof(path))) {
        return;
    }

    if (!vfs_write_path(path, 0, 0)) {
        terminal_puts("touch: failed\n");
        return;
    }

    terminal_puts("touch: ");
    terminal_puts(cmd->argv[1]);
    terminal_putc('\n');
}

static void cmd_cp(command_t* cmd) {
    if (cmd->argc < 3) {
        terminal_puts("usage: cp <src> <dst>\n");
        return;
    }

    char src[SHELL_PATH_MAX];
    char dst[SHELL_PATH_MAX];
    if (!resolve_shell_path_arg(cmd->argv[1], src, sizeof(src))) {
        return;
    }
    if (!resolve_shell_path_arg(cmd->argv[2], dst, sizeof(dst))) {
        return;
    }

    if (!ext2_copy(src, dst)) {
        terminal_puts("cp: failed\n");
        return;
    }

    terminal_puts("cp: ");
    terminal_puts(cmd->argv[1]);
    terminal_puts(" -> ");
    terminal_puts(cmd->argv[2]);
    terminal_putc('\n');
}

static void cmd_mv(command_t* cmd) {
    if (cmd->argc < 3) {
        terminal_puts("usage: mv <src> <dst>\n");
        return;
    }

    char src[SHELL_PATH_MAX];
    char dst[SHELL_PATH_MAX];
    if (!resolve_shell_path_arg(cmd->argv[1], src, sizeof(src))) {
        return;
    }
    if (!resolve_shell_path_arg(cmd->argv[2], dst, sizeof(dst))) {
        return;
    }

    if (!vfs_rename(src, dst)) {
        terminal_puts("mv: failed\n");
        return;
    }

    terminal_puts("mv: ");
    terminal_puts(cmd->argv[1]);
    terminal_puts(" -> ");
    terminal_puts(cmd->argv[2]);
    terminal_putc('\n');
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

static void cmd_runelf_bg(command_t* cmd) {
    (void)launch_background_job(cmd, cmd->argv[0]);
}

static void cmd_jobs(command_t* cmd) {
    int any = 0;

    (void)cmd;

    for (unsigned int i = 0; i < SHELL_JOB_MAX; i++) {
        if (!s_jobs[i].used) {
            continue;
        }

        any = 1;
        terminal_puts("[");
        terminal_put_uint(s_jobs[i].id);
        terminal_puts("] ");
        terminal_puts(job_state_name(s_jobs[i].proc));
        terminal_puts("  ");
        terminal_puts(s_jobs[i].command);
        terminal_putc('\n');
    }

    if (!any) {
        terminal_puts("jobs: none\n");
    }
}

static void cmd_fg(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("usage: fg <jobid>\n");
        return;
    }

    unsigned int id = 0;
    if (!parse_uint_arg(cmd->argv[1], &id)) {
        terminal_puts("fg: invalid job id\n");
        return;
    }

    shell_job_t* job = job_find(id);
    if (!job || !job->proc) {
        terminal_puts("fg: no such job\n");
        return;
    }

    terminal_puts("fg: ");
    terminal_puts(job->command);
    terminal_putc('\n');

    int detached = 0;
    int status = process_wait_detachable(job->proc, &detached);
    if (detached) {
        terminal_puts("fg: backgrounded ");
        terminal_puts(job->command);
        terminal_putc('\n');
        return;
    }

    terminal_puts("fg: exited ");
    terminal_put_uint((unsigned int)status);
    terminal_putc('\n');
    job_clear(job);
}

static void cmd_kill(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("usage: kill <jobid>\n");
        return;
    }

    unsigned int id = 0;
    if (!parse_uint_arg(cmd->argv[1], &id)) {
        terminal_puts("kill: invalid job id\n");
        return;
    }

    shell_job_t* job = job_find(id);
    if (!job || !job->proc) {
        terminal_puts("kill: no such job\n");
        return;
    }

    if (process_group_signal_deliver(job->proc->pgid, SHELL_SIGTERM)) {
        terminal_puts("kill: signaled ");
        terminal_puts(job->command);
        terminal_putc('\n');
        return;
    }

    process_group_kill(job->proc->pgid, 128 + SHELL_SIGTERM);
    process_destroy(job->proc);
    terminal_puts("kill: ");
    terminal_puts(job->command);
    terminal_putc('\n');
    job_clear(job);
}

static void shelltest_begin(const char* name) {
    terminal_puts("shelltest: ");
    terminal_puts(name);
    terminal_puts(" begin\n");
}

static void shelltest_end(const char* name) {
    terminal_puts("shelltest: ");
    terminal_puts(name);
    terminal_puts(" end\n");
}

static void shelltest_call(const char* name, command_fn_t fn, command_t* cmd) {
    shelltest_begin(name);
    fn(cmd);
    shelltest_end(name);
}

static void shelltest_exec(const char* name, command_t* cmd) {
    shelltest_begin(name);
    commands_execute(cmd);
    shelltest_end(name);
}

static void shelltest_call_nowait(const char* name, command_fn_t fn, command_t* cmd) {
    shelltest_begin(name);
    __asm__ __volatile__("cli");
    fn(cmd);
    shelltest_end(name);
    __asm__ __volatile__("sti");
}

static void cmd_shelltest(command_t* cmd) {
    (void)cmd;

    /*
     * Keep these tables static: the shell task only has a 4 KB kernel
     * stack, and the full shell/selftest matrix is large enough to matter.
     */
    command_t help_cmd = { 1, { "help" } };
    command_t clear_cmd = { 1, { "clear" } };
    command_t echo_cmd = { 4, { "echo", "alpha", "beta", "gamma" } };
    static command_t pipeline_cmd = { 4, { "echo", "pipeline-ok", "|", "cat" } };
    static command_t pipeline3_cmd = { 6, { "echo", "pipeline3-ok", "|", "cat", "|", "cat" } };
    command_t about_cmd = { 1, { "about" } };
    command_t uptime_cmd = { 1, { "uptime" } };
    command_t meminfo_cmd = { 1, { "meminfo" } };
    command_t memmap_cmd = { 1, { "memmap" } };
    command_t netinfo_cmd = { 1, { "netinfo" } };
    command_t netsend_cmd = { 1, { "netsend" } };
    command_t netrecv_cmd = { 1, { "netrecv" } };
    command_t arpgw_cmd = { 1, { "arpgw" } };
    static command_t ping_cmd = { 2, { "ping", "10.0.2.2" } };
    static command_t pinggw_cmd = { 1, { "pinggw" } };
    static command_t cd_demo_cmd = { 2, { "cd", "usr/bin" } };
    static command_t pwd_demo_cmd = { 1, { "pwd" } };
    static command_t ls_demo_cmd = { 1, { "ls" } };
    static command_t fsread_rel_cmd = { 2, { "fsread", "hello.elf" } };
    static command_t cat_rel_cmd = { 2, { "cat", "../../var/tmp/compiler.out" } };
    static command_t touch_rel_cmd = { 2, { "touch", "../../var/tmp/LOCAL.TXT" } };
    static command_t fsread_touch_rel_cmd = { 2, { "fsread", "../../var/tmp/LOCAL.TXT" } };
    static command_t runelf_rel_cmd = { 4, { "runelf", "hello", "alpha", "beta" } };
    static command_t cd_root_cmd = { 2, { "cd", "/" } };
    static command_t pwd_root_cmd = { 1, { "pwd" } };
    static command_t ls_root_cmd = { 1, { "ls" } };
    static command_t ls_glob_cmd = { 2, { "ls", "*.elf" } };
    static command_t ataread_cmd = { 2, { "ataread", "0" } };
    static command_t ls_root_dir_cmd = { 2, { "ls", "/" } };
    static command_t ls_path_cmd = { 2, { "ls", "usr/bin" } };
    static command_t tree_root_cmd = { 1, { "tree" } };
    static command_t tree_path_cmd = { 2, { "tree", "usr/bin" } };
    static command_t fsread_cmd = { 2, { "fsread", "usr/bin/hello.elf" } };
    static command_t fsread_path_cmd = { 2, { "fsread", "usr/bin/hello.elf" } };
    static command_t mkdir_cmd = { 2, { "mkdir", "TESTDIR" } };
    static command_t ls_newdir_cmd = { 2, { "ls", "TESTDIR" } };
    static command_t rmdir_cmd = { 2, { "rmdir", "TESTDIR" } };
    static command_t ls_removed_cmd = { 2, { "ls", "TESTDIR" } };
    static command_t mkdir_nested_parent_cmd = { 2, { "mkdir", "NESTPARENT" } };
    static command_t mkdir_nested_child_cmd = { 2, { "mkdir", "NESTPARENT/CHILD" } };
    static command_t ls_nested_cmd = { 2, { "ls", "NESTPARENT" } };
    static command_t rmdir_nested_child_cmd = { 2, { "rmdir", "NESTPARENT/CHILD" } };
    static command_t rmdir_nested_parent_cmd = { 2, { "rmdir", "NESTPARENT" } };
    static command_t ls_nested_removed_cmd = { 2, { "ls", "NESTPARENT" } };
    static command_t mkdir_var_tmp_cmd = { 2, { "mkdir", "var/tmp" } };
    static command_t mkdir_work_cmd = { 2, { "mkdir", "var/tmp/WORK" } };
    static command_t mkdir_samples_cmd = { 2, { "mkdir", "var/tmp/samples" } };
    static command_t mv_tccmath_cmd = { 3, { "mv", "usr/share/examples/tinycc/tccmath.c", "var/tmp/samples" } };
    static command_t mv_tccagg_cmd = { 3, { "mv", "usr/share/examples/tinycc/tccagg.c", "var/tmp/samples" } };
    static command_t mv_tcctree_cmd = { 3, { "mv", "usr/share/examples/tinycc/tcctree.c", "var/tmp/samples" } };
    static command_t mv_tccmini_cmd = { 3, { "mv", "usr/share/examples/tinycc/tccmini.c", "var/tmp/samples" } };
    static command_t ls_samples_cmd = { 2, { "ls", "var/tmp/samples" } };
    static command_t tccmath_build_cmd = { 6, { "runelf", "usr/bin/tcc.elf", "-nostdlib", "-o", "var/tmp/tccmath.elf", "var/tmp/samples/tccmath.c" } };
    static command_t tccmath_run_cmd = { 2, { "runelf", "var/tmp/tccmath" } };
    static command_t tccagg_build_cmd = { 6, { "runelf", "usr/bin/tcc.elf", "-nostdlib", "-o", "var/tmp/tccagg.elf", "var/tmp/samples/tccagg.c" } };
    static command_t tccagg_run_cmd = { 2, { "runelf", "var/tmp/tccagg" } };
    static command_t tcctree_build_cmd = { 6, { "runelf", "usr/bin/tcc.elf", "-nostdlib", "-o", "var/tmp/tcctree.elf", "var/tmp/samples/tcctree.c" } };
    static command_t tcctree_run_cmd = { 2, { "runelf", "var/tmp/tcctree" } };
    static command_t cp_cmd = { 3, { "cp", "var/tmp/compiler.out", "var/tmp/compiler.copy" } };
    static command_t fsread_copy_cmd = { 2, { "fsread", "var/tmp/compiler.copy" } };
    static command_t mv_cmd = { 3, { "mv", "var/tmp/compiler.copy", "var/tmp/compiler.moved" } };
    static command_t fsread_moved_cmd = { 2, { "fsread", "var/tmp/compiler.moved" } };
    static command_t cp_dir_cmd = { 3, { "cp", "var/tmp/compiler.out", "var/tmp/WORK" } };
    static command_t fsread_dir_copy_cmd = { 2, { "fsread", "var/tmp/WORK/compiler.out" } };
    static command_t rm_dir_cmd = { 2, { "rm", "var/tmp/WORK/compiler.out" } };
    static command_t fsread_dir_removed_cmd = { 2, { "fsread", "var/tmp/WORK/compiler.out" } };
    static command_t mv_dir_cmd = { 3, { "mv", "var/tmp/compiler.moved", "var/tmp/WORK" } };
    static command_t cat_cmd = { 2, { "cat", "var/tmp/compiler.out" } };
    static command_t touch_cmd = { 2, { "touch", "var/tmp/EMPTY.TXT" } };
    static command_t fsread_touch_cmd = { 2, { "fsread", "var/tmp/EMPTY.TXT" } };
    static command_t edit_cmd = { 14, { "edit", "var/tmp/EDIT.TXT", "-c", "c", "-c", "a", "-c", "first-line", "-c", "second-line", "-c", ".", "-c", "wq" } };
    static command_t cat_edit_cmd = { 2, { "cat", "var/tmp/EDIT.TXT" } };
    static command_t runelf_cmd = { 4, { "runelf", "hello", "alpha", "beta" } };
    static command_t runelf_path_cmd = { 4, { "runelf", "usr/bin/hello", "alpha", "beta" } };
    static command_t runelf_nowait_cmd = { 2, { "runelf_nowait", "usr/libexec/tests/ticks" } };
    static command_t compiler_demo_cmd = { 2, { "runelf", "usr/libexec/tests/compiler_demo" } };
    static command_t tinycc_cmd = { 3, { "runelf", "usr/bin/tcc.elf", "-v" } };
    static command_t tccmini_build_cmd = { 6, { "runelf", "usr/bin/tcc.elf", "-nostdlib", "-o", "var/tmp/tccmini.elf", "var/tmp/samples/tccmini.c" } };
    static command_t tccmini_run_cmd = { 2, { "runelf", "var/tmp/tccmini" } };

    terminal_puts("shelltest: start\n");

    shelltest_call("help", cmd_help, &help_cmd);
    shelltest_call("clear", cmd_clear, &clear_cmd);
    shelltest_exec("echo", &echo_cmd);
    shelltest_exec("pipeline", &pipeline_cmd);
    shelltest_exec("pipeline3", &pipeline3_cmd);
    shelltest_exec("about", &about_cmd);
    shelltest_exec("uptime", &uptime_cmd);
    shelltest_call("meminfo", cmd_meminfo, &meminfo_cmd);
    shelltest_call("memmap", cmd_memmap, &memmap_cmd);
    shelltest_call("netinfo", cmd_netinfo, &netinfo_cmd);
    shelltest_call("netsend", cmd_netsend, &netsend_cmd);
    shelltest_call("netrecv", cmd_netrecv, &netrecv_cmd);
    shelltest_call("arpgw", cmd_arpgw, &arpgw_cmd);
    shelltest_call("ping", cmd_ping, &ping_cmd);
    shelltest_call("pinggw", cmd_pinggw, &pinggw_cmd);
    shelltest_call("ataread", cmd_ataread, &ataread_cmd);
    shelltest_exec("ls_abs_root", &ls_root_dir_cmd);
    shelltest_exec("ls_path", &ls_path_cmd);
    shelltest_exec("tree", &tree_root_cmd);
    shelltest_exec("tree_path", &tree_path_cmd);
    shelltest_exec("fsread", &fsread_cmd);
    shelltest_exec("fsread_path", &fsread_path_cmd);
    shelltest_exec("cat", &cat_cmd);
    shelltest_exec("touch", &touch_cmd);
    shelltest_exec("fsread_touch", &fsread_touch_cmd);
    shelltest_exec("mkdir", &mkdir_cmd);
    shelltest_exec("ls_newdir", &ls_newdir_cmd);
    shelltest_exec("rmdir", &rmdir_cmd);
    shelltest_exec("ls_removed", &ls_removed_cmd);
    shelltest_exec("mkdir_nested_parent", &mkdir_nested_parent_cmd);
    shelltest_exec("mkdir_nested_child", &mkdir_nested_child_cmd);
    shelltest_exec("ls_nested", &ls_nested_cmd);
    shelltest_exec("rmdir_nested_child", &rmdir_nested_child_cmd);
    shelltest_exec("rmdir_nested_parent", &rmdir_nested_parent_cmd);
    shelltest_exec("ls_nested_removed", &ls_nested_removed_cmd);
    shelltest_call("compiler_demo", cmd_runelf, &compiler_demo_cmd);
    shelltest_call("tinycc", cmd_runelf, &tinycc_cmd);
    shelltest_exec("mkdir_var_tmp", &mkdir_var_tmp_cmd);
    shelltest_exec("mkdir_work", &mkdir_work_cmd);
    shelltest_exec("mkdir_samples", &mkdir_samples_cmd);
    shelltest_exec("mv_tccmath", &mv_tccmath_cmd);
    shelltest_exec("mv_tccagg", &mv_tccagg_cmd);
    shelltest_exec("mv_tcctree", &mv_tcctree_cmd);
    shelltest_exec("mv_tccmini", &mv_tccmini_cmd);
    shelltest_exec("ls_samples", &ls_samples_cmd);
    shelltest_call("tccmath_build", cmd_runelf, &tccmath_build_cmd);
    shelltest_call("tccmath_run", cmd_runelf, &tccmath_run_cmd);
    shelltest_call("tccagg_build", cmd_runelf, &tccagg_build_cmd);
    shelltest_call("tccagg_run", cmd_runelf, &tccagg_run_cmd);
    shelltest_call("tcctree_build", cmd_runelf, &tcctree_build_cmd);
    shelltest_call("tcctree_run", cmd_runelf, &tcctree_run_cmd);
    shelltest_call("tccmini_build", cmd_runelf, &tccmini_build_cmd);
    shelltest_call("tccmini_run", cmd_runelf, &tccmini_run_cmd);
    shelltest_exec("cat", &cat_cmd);
    shelltest_exec("touch", &touch_cmd);
    shelltest_exec("fsread_touch", &fsread_touch_cmd);
    shelltest_exec("edit", &edit_cmd);
    shelltest_exec("cat_edit", &cat_edit_cmd);
    shelltest_call("cd", cmd_cd, &cd_demo_cmd);
    shelltest_exec("pwd", &pwd_demo_cmd);
    shelltest_exec("ls", &ls_demo_cmd);
    shelltest_exec("fsread_rel", &fsread_rel_cmd);
    shelltest_exec("cat_rel", &cat_rel_cmd);
    shelltest_exec("touch_rel", &touch_rel_cmd);
    shelltest_exec("fsread_touch_rel", &fsread_touch_rel_cmd);
    shelltest_call("runelf_rel", cmd_runelf, &runelf_rel_cmd);
    shelltest_call("runelf", cmd_runelf, &runelf_cmd);
    shelltest_call("cd_root", cmd_cd, &cd_root_cmd);
    shelltest_exec("pwd_root", &pwd_root_cmd);
    shelltest_call("runelf_path", cmd_runelf, &runelf_path_cmd);
    shelltest_exec("ls_root", &ls_root_cmd);
    shelltest_exec("ls_glob", &ls_glob_cmd);
    shelltest_exec("cp", &cp_cmd);
    shelltest_exec("fsread_copy", &fsread_copy_cmd);
    shelltest_exec("mv", &mv_cmd);
    shelltest_exec("fsread_moved", &fsread_moved_cmd);
    shelltest_exec("cp_dir", &cp_dir_cmd);
    shelltest_exec("fsread_dir_copy", &fsread_dir_copy_cmd);
    shelltest_exec("rm_dir", &rm_dir_cmd);
    shelltest_exec("fsread_dir_removed", &fsread_dir_removed_cmd);
    shelltest_exec("mv_dir", &mv_dir_cmd);
    shelltest_call_nowait("runelf_nowait", cmd_runelf_nowait, &runelf_nowait_cmd);

    terminal_puts("shelltest: PASS\n");
}

typedef struct {
    const char* label;
    const char* program;
    int argc;
    char** argv;
    int expected_status;
} selftest_case_t;

static int run_selftest_case(const selftest_case_t* tc) {
    terminal_puts("selftest: ");
    terminal_puts(tc->label);
    terminal_puts(" ... ");

    __asm__ __volatile__("cli");
    process_t* proc = elf_run_named(tc->program, tc->argc, tc->argv);
    if (proc) {
        process_claim_for_wait(proc);
    }
    if (!proc) {
        __asm__ __volatile__("sti");
        terminal_puts("FAIL (launch)\n");
        return 0;
    }

    terminal_puts("selftest: ");
    terminal_puts(tc->label);
    terminal_puts(" launched\n");
    terminal_puts("selftest: ");
    terminal_puts(tc->label);
    terminal_puts(" waiting\n");
    __asm__ __volatile__("sti");
    int status = process_wait(proc);
    terminal_puts("selftest: ");
    terminal_puts(tc->label);
    terminal_puts(" woke\n");
    if (status != tc->expected_status) {
        terminal_puts("FAIL (status=");
        terminal_put_uint((unsigned int)status);
        terminal_puts(", expected=");
        terminal_put_uint((unsigned int)tc->expected_status);
        terminal_puts(")\n");
        return 0;
    }

    terminal_puts("PASS\n");
    return 1;
}

static void cmd_selftest(command_t* cmd) {
    (void)cmd;

    int ok = 1;

    /*
     * Same stack-safety note as cmd_shelltest(): these argv tables stay in
     * static storage so the shell task does not trample its kernel stack
     * while it runs the full ELF regression suite.
     */
    static char* hello_argv[] = { "usr/bin/hello", "alpha", "beta", 0 };
    static char* ticks_argv[] = { "usr/libexec/tests/ticks", "alpha", "beta", 0 };
    static char* args_argv[] = { "usr/libexec/tests/args", "alpha", "beta", 0 };
    static char* runelf_argv[] = { "usr/libexec/tests/runelf_test", "alpha", "beta", "gamma", 0 };
    static char* readline_argv[] = { "usr/libexec/tests/readline", "alpha", "beta", 0 };
    static char* exec_argv[] = { "usr/libexec/tests/exec_test", "alpha", "beta", 0 };
    static char* waitprobe_argv[] = { "usr/libexec/tests/waitprobe", "alpha", "beta", 0 };
    static char* fileread_argv[] = { "usr/libexec/tests/fileread", "alpha", "beta", 0 };
    static char* compiler_demo_argv[] = { "usr/libexec/tests/compiler_demo", "alpha", "beta", 0 };
    static char* heapprobe_argv[] = { "usr/libexec/tests/heapprobe", "alpha", "beta", 0 };
    static char* statprobe_argv[] = { "usr/libexec/tests/statprobe", "alpha", "beta", 0 };
    static char* fileprobe_argv[] = { "usr/libexec/tests/fileprobe", "alpha", "beta", 0 };
    static char* cwdprobe_argv[] = { "usr/libexec/tests/cwdprobe", "alpha", "beta", 0 };
    static char* stdioprobe_argv[] = { "usr/libexec/tests/stdioprobe", "alpha", "beta", 0 };
    static char* dirprobe_argv[] = { "usr/libexec/tests/dirprobe", "alpha", "beta", 0 };
    static char* errnoprobe_argv[] = { "usr/libexec/tests/errnoprobe", "alpha", "beta", 0 };
    static char* badptrprobe_argv[] = { "usr/libexec/tests/badptrprobe", "alpha", "beta", 0 };
    static char* sleep_argv[] = { "usr/libexec/tests/sleep_test", "alpha", "beta", 0 };
    static char* timerfdprobe_argv[] = { "usr/libexec/tests/timerfdprobe", "alpha", "beta", 0 };
    static char* ptrguard_argv[] = { "usr/libexec/tests/ptrguard", "alpha", "beta", 0 };
    static char* preempt_argv[] = { "usr/libexec/tests/preempt_test", "alpha", "beta", 0 };
    static char* crtprobe_argv[] = { "usr/libexec/tests/crtprobe.elf", "alpha", "nested/path", "longish-argument-0123456789abcdef", 0 };
    static char* inputprobe_argv[] = { "usr/libexec/tests/inputprobe", "alpha", "beta", 0 };
    static char* pipeprobe_argv[] = { "usr/libexec/tests/pipeprobe", 0 };
    static char* dupprobe_argv[] = { "usr/libexec/tests/dupprobe", 0 };
    static char* forkprobe_argv[] = { "usr/libexec/tests/forkprobe", 0 };
    static char* execveprobe_argv[] = { "usr/libexec/tests/execveprobe", 0 };
    static char* fault_ud_argv[] = { "usr/libexec/tests/fault", "ud", 0 };
    static char* fault_gp_argv[] = { "usr/libexec/tests/fault", "gp", 0 };
    static char* fault_de_argv[] = { "usr/libexec/tests/fault", "de", 0 };
    static char* fault_br_argv[] = { "usr/libexec/tests/fault", "br", 0 };
    static char* fault_pf_argv[] = { "usr/libexec/tests/fault", "pf", 0 };
    static command_t shelltest_cmd = { 1, { "shelltest" } };

    const selftest_case_t cases[] = {
        { "hello",       "usr/bin/hello",       3, hello_argv,       0 },
        { "ticks",       "usr/libexec/tests/ticks",      1, ticks_argv,       0 },
        { "args",        "usr/libexec/tests/args",       3, args_argv,        0 },
        { "runelf_test", "usr/libexec/tests/runelf_test",4, runelf_argv,      0 },
        { "readline",    "usr/libexec/tests/readline",   1, readline_argv,    0 },
        { "exec_test",   "usr/libexec/tests/exec_test",  1, exec_argv,        0 },
        { "waitprobe",   "usr/libexec/tests/waitprobe",  1, waitprobe_argv,   0 },
        { "fileread",    "usr/libexec/tests/fileread",   1, fileread_argv,    0 },
        { "compiler_demo","usr/libexec/tests/compiler_demo",1, compiler_demo_argv,0 },
        { "heapprobe",   "usr/libexec/tests/heapprobe",  1, heapprobe_argv,   0 },
        { "statprobe",   "usr/libexec/tests/statprobe",  1, statprobe_argv,   0 },
        { "fileprobe",   "usr/libexec/tests/fileprobe",  1, fileprobe_argv,   0 },
        { "cwdprobe",    "usr/libexec/tests/cwdprobe",   1, cwdprobe_argv,    0 },
        { "stdioprobe",  "usr/libexec/tests/stdioprobe", 1, stdioprobe_argv,  0 },
        { "dirprobe",    "usr/libexec/tests/dirprobe",   1, dirprobe_argv,    0 },
        { "errnoprobe",  "usr/libexec/tests/errnoprobe", 1, errnoprobe_argv,  0 },
        { "badptrprobe", "usr/libexec/tests/badptrprobe",1, badptrprobe_argv, 0 },
        { "sleep_test",  "usr/libexec/tests/sleep_test", 1, sleep_argv,       0 },
        { "timerfdprobe","usr/libexec/tests/timerfdprobe",1, timerfdprobe_argv,0 }, /* timerfd wait regression */
        { "ptrguard",    "usr/libexec/tests/ptrguard",   1, ptrguard_argv,    0 }, /* syscall pointer regression */
        { "preempt_test","usr/libexec/tests/preempt_test",1, preempt_argv,     0 }, /* timer-preemption regression */
        { "crtprobe",    "usr/libexec/tests/crtprobe.elf",4, crtprobe_argv,    7 },
        { "inputprobe",  "usr/libexec/tests/inputprobe", 1, inputprobe_argv,   0 }, /* input event queue regression */
        { "pipeprobe",   "usr/libexec/tests/pipeprobe",  1, pipeprobe_argv,    0 },
        { "dupprobe",    "usr/libexec/tests/dupprobe",   1, dupprobe_argv,     0 },
        { "forkprobe",   "usr/libexec/tests/forkprobe",  1, forkprobe_argv,    0 },
        { "execveprobe", "usr/libexec/tests/execveprobe",1, execveprobe_argv,  0 },
        { "fault ud",    "usr/libexec/tests/fault",      2, fault_ud_argv,    6 },
        { "fault gp",    "usr/libexec/tests/fault",      2, fault_gp_argv,   13 },
        { "fault de",    "usr/libexec/tests/fault",      2, fault_de_argv,    0 },
        { "fault br",    "usr/libexec/tests/fault",      2, fault_br_argv,    5 },
        { "fault pf",    "usr/libexec/tests/fault",      2, fault_pf_argv,   14 },
    };

    terminal_puts("selftest: start\n");
    shelltest_begin("overall");
    cmd_shelltest(&shelltest_cmd);
    shelltest_end("overall");

    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!run_selftest_case(&cases[i])) {
            ok = 0;
        }
    }

    if (ok) {
        terminal_puts("selftest: PASS\n");
    } else {
        terminal_puts("selftest: FAIL\n");
    }
}

static command_entry_t commands[] = {
    { "help",          "show shell commands",           cmd_help },
    { "clear",         "clear the screen",              cmd_clear },
    { "meminfo",       "show heap and frame usage",     cmd_meminfo },
    { "memmap",        "show BIOS E820 memory map",     cmd_memmap },
    { "netinfo",       "show PCI NIC status",          cmd_netinfo },
    { "netsend",       "queue a test Ethernet frame",  cmd_netsend },
    { "netrecv",       "poll and dispatch one Ethernet frame", cmd_netrecv },
    { "arpgw",         "resolve the QEMU gateway via ARP", cmd_arpgw },
    { "ping",          "ping an IPv4 address",        cmd_ping },
    { "pinggw",        "ping the QEMU gateway",         cmd_pinggw },
    { "pingpublic",    "try public ICMP (often unsupported by QEMU user net)", cmd_pingpublic },
    { "netcheck",      "check gateway and public connectivity", cmd_netcheck },
    { "cd",            "change the shell working directory", cmd_cd },
    { "ataread",       "dump raw sector bytes",         cmd_ataread },
    { "runelf",        "run an ext2 ELF and wait",      cmd_runelf },
    { "runelf_nowait", "run an ext2 ELF and return",    cmd_runelf_nowait },
    { "runelf_bg",     "run a reattachable background ELF", cmd_runelf_bg },
    { "bg",            "run a reattachable background ELF", cmd_runelf_bg },
    { "jobs",          "list reattachable background jobs", cmd_jobs },
    { "fg",            "reattach and wait for a job",   cmd_fg },
    { "kill",          "terminate a background job",    cmd_kill },
    { "selftest",      "run shipped ELF self-tests",    cmd_selftest },
    { "shelltest",     "run built-in shell command tests", cmd_shelltest },
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

static void close_shell_fd(process_t* shell_proc, int fd) {
    fd_entry_t* ent = process_fd_get(shell_proc, fd);
    if (ent) {
        process_fd_close(ent);
    }
}

static int run_pipeline(command_t* cmd) {
    enum { PIPELINE_MAX = 8 };
    command_t stages[PIPELINE_MAX];
    char paths[PIPELINE_MAX][SHELL_PATH_MAX];
    process_t* procs[PIPELINE_MAX];
    int pipes[PIPELINE_MAX - 1][2];
    int stage_count = 0;
    int arg_start = 0;
    process_t* shell_proc = sched_current();

    if (!shell_proc) return 0;
    for (int i = 0; i < PIPELINE_MAX; i++) procs[i] = 0;
    for (int i = 0; i < PIPELINE_MAX - 1; i++) {
        pipes[i][0] = -1;
        pipes[i][1] = -1;
    }

    for (int i = 0; i <= cmd->argc; i++) {
        if (i == cmd->argc || k_strcmp(cmd->argv[i], "|")) {
            int argc = i - arg_start;
            if (argc <= 0 || stage_count >= PIPELINE_MAX) {
                terminal_puts("pipeline: syntax error\n");
                return 0;
            }
            stages[stage_count].argc = argc;
            for (int j = 0; j < argc && j < MAX_ARGS; j++) {
                stages[stage_count].argv[j] = cmd->argv[arg_start + j];
            }
            if (!resolve_app_command_path(stages[stage_count].argv[0],
                                          paths[stage_count],
                                          sizeof(paths[stage_count]))) {
                terminal_puts("pipeline: command not found: ");
                terminal_puts(stages[stage_count].argv[0]);
                terminal_putc('\n');
                return 0;
            }
            stage_count++;
            arg_start = i + 1;
        }
    }

    for (int i = 0; i < stage_count - 1; i++) {
        if (process_fd_pipe(shell_proc, pipes[i], 0) < 0) {
            terminal_puts("pipeline: pipe failed\n");
            return 0;
        }
    }

    __asm__ __volatile__("cli");
    for (int i = 0; i < stage_count; i++) {
        process_t* proc = elf_run_named(paths[i], stages[i].argc, stages[i].argv);
        if (!proc) {
            __asm__ __volatile__("sti");
            terminal_puts("pipeline: launch failed: ");
            terminal_puts(paths[i]);
            terminal_putc('\n');
            for (int p = 0; p < stage_count - 1; p++) {
                if (pipes[p][0] >= 0) close_shell_fd(shell_proc, pipes[p][0]);
                if (pipes[p][1] >= 0) close_shell_fd(shell_proc, pipes[p][1]);
            }
            return 0;
        }
        k_strncpy(proc->cwd, shell_get_cwd(), sizeof(proc->cwd));
        process_claim_for_wait(proc);

        if (i > 0) {
            (void)process_fd_dup_from(proc, 0, shell_proc, pipes[i - 1][0], 0);
        }
        if (i < stage_count - 1) {
            (void)process_fd_dup_from(proc, 1, shell_proc, pipes[i][1], 0);
        }
        procs[i] = proc;
    }
    __asm__ __volatile__("sti");

    for (int i = 0; i < stage_count - 1; i++) {
        close_shell_fd(shell_proc, pipes[i][0]);
        close_shell_fd(shell_proc, pipes[i][1]);
    }

    for (int i = 0; i < stage_count; i++) {
        if (procs[i]) {
            process_wait(procs[i]);
        }
    }
    return 1;
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

    for (int i = 0; i < cmd->argc; i++) {
        if (k_strcmp(cmd->argv[i], "|")) {
            run_pipeline(cmd);
            return;
        }
    }

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
