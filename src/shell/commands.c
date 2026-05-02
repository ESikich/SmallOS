#include "commands.h"
#include "shell.h"
#include "terminal.h"
#include "elf_loader.h"
#include "memory.h"
#include "pmm.h"
#include "ata.h"
#include "e1000.h"
#include "arp.h"
#include "ipv4.h"
#include "fat16.h"
#include "process.h"
#include "klib.h"

static void print_command_list(void);
static void print_program_list(void);
static void run_elf_command(command_t* cmd, const char* program);
static int resolve_shell_path_arg(const char* input, char* out, unsigned int out_size);
static int path_has_wildcards(const char* path);
static int split_wildcard_path(const char* path, char* dir_out, unsigned int dir_out_size, const char** pattern_out);
static void cmd_ls_path(const char* input);
static void terminal_put_cwd(void);

static void cmd_help(command_t* cmd) {
    (void)cmd;
    terminal_puts("Commands:\n");
    print_command_list();

    terminal_puts("\nPrograms:\n");
    print_program_list();
}

static void cmd_clear(command_t* cmd) {
    (void)cmd;
    terminal_clear();
}

static void run_elf_command(command_t* cmd, const char* program) {
    __asm__ __volatile__("cli");
    process_t* proc = elf_run_named(program, cmd->argc, cmd->argv);
    if (proc) {
        process_claim_for_wait(proc);
    }
    __asm__ __volatile__("sti");

    if (!proc) {
        terminal_puts(program);
        terminal_puts(": failed\n");
        return;
    }

    process_wait(proc);
}

static int resolve_shell_path_arg(const char* input, char* out, unsigned int out_size) {
    if (!shell_resolve_path(input, out, out_size)) {
        terminal_puts("fat16: invalid path: ");
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

static void cmd_about(command_t* cmd) {
    run_elf_command(cmd, "about");
}

static void cmd_halt(command_t* cmd) {
    run_elf_command(cmd, "halt");
}

static void cmd_reboot(command_t* cmd) {
    run_elf_command(cmd, "reboot");
}

static void cmd_uptime(command_t* cmd) {
    run_elf_command(cmd, "uptime");
}

static void cmd_echo(command_t* cmd) {
    run_elf_command(cmd, "echo");
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

    unsigned int heap_base = 0x100000u;
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
}

static void cmd_netinfo(command_t* cmd) {
    (void)cmd;

    terminal_puts("netinfo: ");
    e1000_print_info();
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

    u8 frame[1600];
    u32 len = 0;

    if (!e1000_recv(frame, sizeof(frame), &len)) {
        terminal_puts("netrecv: no packet\n");
        return;
    }

    terminal_puts("netrecv: ");
    terminal_put_uint(len);
    terminal_puts(" bytes\nfirst 32: ");

    static const char hex[] = "0123456789ABCDEF";
    u32 show = len < 32u ? len : 32u;
    for (u32 i = 0; i < show; i++) {
        terminal_putc(hex[frame[i] >> 4]);
        terminal_putc(hex[frame[i] & 0xF]);
        terminal_putc(' ');
        if (i == 15u) {
            terminal_puts("\n  ");
        }
    }
    terminal_putc('\n');
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

    /* Public ICMP probe to separate "gateway works" from "internet works". */
    cmd_ping_target("pingpublic", 0x0A00020Fu, 0x01010101u, 0x0A000202u);
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
        terminal_puts("netcheck: note: some hosts block ICMP beyond the gateway\n");
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
            FAT16_PARTITION_ENTRY_INDEX = 1,
        };
        unsigned int entry_off = MBR_PARTITION_TABLE_OFFSET +
                                 FAT16_PARTITION_ENTRY_INDEX * MBR_PARTITION_ENTRY_SIZE;
        terminal_puts("sig: ");
        terminal_put_hex(sector[510]);
        terminal_putc(' ');
        terminal_put_hex(sector[511]);
        terminal_puts(" (expect 0x55 0xAA)\n");
        terminal_puts("fat16 partition lba: ");
        unsigned int partition_lba = sector[entry_off + MBR_PARTITION_LBA_OFFSET]
                                   | ((unsigned int)sector[entry_off + MBR_PARTITION_LBA_OFFSET + 1] << 8)
                                   | ((unsigned int)sector[entry_off + MBR_PARTITION_LBA_OFFSET + 2] << 16)
                                   | ((unsigned int)sector[entry_off + MBR_PARTITION_LBA_OFFSET + 3] << 24);
        terminal_put_uint(partition_lba);
        terminal_putc('\n');
    }
}

static void cmd_fsls(command_t* cmd) {
    if (cmd->argc < 2) {
        fat16_ls();
        return;
    }

    cmd_ls_path(cmd->argv[1]);
}

static void cmd_ls(command_t* cmd) {
    if (cmd->argc < 2) {
        const char* cwd = shell_get_cwd();
        if (!cwd || cwd[0] == '\0') {
            fat16_ls();
            return;
        }

        fat16_ls_path(cwd);
        return;
    }

    cmd_ls_path(cmd->argv[1]);
}

static int path_has_wildcards(const char* path) {
    if (!path) {
        return 0;
    }

    for (const char* p = path; *p; p++) {
        if (*p == '*' || *p == '?') {
            return 1;
        }
    }

    return 0;
}

static int split_wildcard_path(const char* path, char* dir_out, unsigned int dir_out_size, const char** pattern_out) {
    if (!path || !dir_out || !pattern_out || dir_out_size == 0) {
        return 0;
    }

    const char* last_sep = 0;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            last_sep = p;
        }
    }

    if (!last_sep) {
        if (path[0] != '\0' && 1u > dir_out_size) {
            return 0;
        }
        dir_out[0] = '\0';
        *pattern_out = path;
        return 1;
    }

    unsigned int dir_len = (unsigned int)(last_sep - path);
    if (dir_len + 1u > dir_out_size) {
        return 0;
    }

    for (unsigned int i = 0; i < dir_len; i++) {
        dir_out[i] = path[i];
    }
    dir_out[dir_len] = '\0';
    *pattern_out = last_sep + 1;
    return 1;
}

static void cmd_ls_path(const char* input) {
    char path[SHELL_PATH_MAX];
    if (!resolve_shell_path_arg(input, path, sizeof(path))) {
        return;
    }

    if (!path_has_wildcards(path)) {
        fat16_ls_path(path);
        return;
    }

    char dir[SHELL_PATH_MAX];
    const char* pattern = 0;
    if (!split_wildcard_path(path, dir, sizeof(dir), &pattern)) {
        terminal_puts("ls: failed\n");
        return;
    }

    fat16_ls_path_filtered(dir, pattern);
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
    const unsigned char* data = fat16_load(path, &size);
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
    const unsigned char* data = fat16_load(path, &size);
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

    if (!fat16_mkdir(path)) {
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

    if (!fat16_rmdir(path)) {
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

    if (!fat16_rm(path)) {
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

    if (!fat16_write_path(path, 0, 0)) {
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

    if (!fat16_copy(src, dst)) {
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

    if (!fat16_move(src, dst)) {
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

    if (!elf_run_named(path, cmd->argc - 1, &cmd->argv[1])) {
        terminal_puts("runelf_nowait: failed\n");
    }
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

static void cmd_shelltest(command_t* cmd) {
    (void)cmd;

    /*
     * Keep these tables static: the shell task only has a 4 KB kernel
     * stack, and the full shell/selftest matrix is large enough to matter.
     */
    command_t help_cmd = { 1, { "help" } };
    command_t clear_cmd = { 1, { "clear" } };
    command_t echo_cmd = { 4, { "echo", "alpha", "beta", "gamma" } };
    command_t about_cmd = { 1, { "about" } };
    command_t uptime_cmd = { 1, { "uptime" } };
    command_t meminfo_cmd = { 1, { "meminfo" } };
    command_t netinfo_cmd = { 1, { "netinfo" } };
    command_t netsend_cmd = { 1, { "netsend" } };
    command_t netrecv_cmd = { 1, { "netrecv" } };
    command_t arpgw_cmd = { 1, { "arpgw" } };
    static command_t ping_cmd = { 2, { "ping", "10.0.2.2" } };
    static command_t pinggw_cmd = { 1, { "pinggw" } };
    static command_t pingpublic_cmd = { 1, { "pingpublic" } };
    static command_t netcheck_cmd = { 1, { "netcheck" } };
    static command_t cd_demo_cmd = { 2, { "cd", "apps/demo" } };
    static command_t pwd_demo_cmd = { 1, { "pwd" } };
    static command_t ls_demo_cmd = { 1, { "ls" } };
    static command_t fsread_rel_cmd = { 2, { "fsread", "hello.elf" } };
    static command_t cat_rel_cmd = { 2, { "cat", "../../compiler.out" } };
    static command_t touch_rel_cmd = { 2, { "touch", "LOCAL.TXT" } };
    static command_t fsread_touch_rel_cmd = { 2, { "fsread", "LOCAL.TXT" } };
    static command_t runelf_rel_cmd = { 4, { "runelf", "hello", "alpha", "beta" } };
    static command_t cd_root_cmd = { 2, { "cd", "/" } };
    static command_t pwd_root_cmd = { 1, { "pwd" } };
    static command_t ls_root_cmd = { 1, { "ls" } };
    static command_t ls_glob_cmd = { 2, { "ls", "*.elf" } };
    static command_t ataread_cmd = { 2, { "ataread", "0" } };
    static command_t fsls_root_cmd = { 1, { "fsls" } };
    static command_t fsls_path_cmd = { 2, { "fsls", "apps/demo" } };
    static command_t fsread_cmd = { 2, { "fsread", "apps/demo/hello.elf" } };
    static command_t fsread_path_cmd = { 2, { "fsread", "apps/demo/hello.elf" } };
    static command_t mkdir_cmd = { 2, { "mkdir", "TESTDIR" } };
    static command_t fsls_newdir_cmd = { 2, { "fsls", "TESTDIR" } };
    static command_t rmdir_cmd = { 2, { "rmdir", "TESTDIR" } };
    static command_t fsls_removed_cmd = { 2, { "fsls", "TESTDIR" } };
    static command_t mkdir_nested_parent_cmd = { 2, { "mkdir", "NESTPARENT" } };
    static command_t mkdir_nested_child_cmd = { 2, { "mkdir", "NESTPARENT/CHILD" } };
    static command_t fsls_nested_cmd = { 2, { "fsls", "NESTPARENT" } };
    static command_t rmdir_nested_child_cmd = { 2, { "rmdir", "NESTPARENT/CHILD" } };
    static command_t rmdir_nested_parent_cmd = { 2, { "rmdir", "NESTPARENT" } };
    static command_t fsls_nested_removed_cmd = { 2, { "fsls", "NESTPARENT" } };
    static command_t mkdir_samples_cmd = { 2, { "mkdir", "samples" } };
    static command_t mv_tccmath_cmd = { 3, { "mv", "tccmath.c", "samples" } };
    static command_t mv_tccagg_cmd = { 3, { "mv", "tccagg.c", "samples" } };
    static command_t mv_tcctree_cmd = { 3, { "mv", "tcctree.c", "samples" } };
    static command_t mv_tccmini_cmd = { 3, { "mv", "tccmini.c", "samples" } };
    static command_t fsls_samples_cmd = { 2, { "fsls", "samples" } };
    static command_t tccmath_build_cmd = { 6, { "runelf", "tools/tcc.elf", "-nostdlib", "-o", "tccmath.elf", "samples/tccmath.c" } };
    static command_t tccmath_run_cmd = { 2, { "runelf", "tccmath" } };
    static command_t tccagg_build_cmd = { 6, { "runelf", "tools/tcc.elf", "-nostdlib", "-o", "tccagg.elf", "samples/tccagg.c" } };
    static command_t tccagg_run_cmd = { 2, { "runelf", "tccagg" } };
    static command_t tcctree_build_cmd = { 6, { "runelf", "tools/tcc.elf", "-nostdlib", "-o", "tcctree.elf", "samples/tcctree.c" } };
    static command_t tcctree_run_cmd = { 2, { "runelf", "tcctree" } };
    static command_t cp_cmd = { 3, { "cp", "compiler.out", "compiler.copy" } };
    static command_t fsread_copy_cmd = { 2, { "fsread", "compiler.copy" } };
    static command_t mv_cmd = { 3, { "mv", "compiler.copy", "compiler.moved" } };
    static command_t fsread_moved_cmd = { 2, { "fsread", "compiler.moved" } };
    static command_t cp_dir_cmd = { 3, { "cp", "compiler.out", "apps/demo" } };
    static command_t fsread_dir_copy_cmd = { 2, { "fsread", "apps/demo/compiler.out" } };
    static command_t rm_dir_cmd = { 2, { "rm", "apps/demo/compiler.out" } };
    static command_t fsread_dir_removed_cmd = { 2, { "fsread", "apps/demo/compiler.out" } };
    static command_t mv_dir_cmd = { 3, { "mv", "compiler.moved", "apps/demo" } };
    static command_t cat_cmd = { 2, { "cat", "compiler.out" } };
    static command_t touch_cmd = { 2, { "touch", "EMPTY.TXT" } };
    static command_t fsread_touch_cmd = { 2, { "fsread", "EMPTY.TXT" } };
    static command_t runelf_cmd = { 4, { "runelf", "hello", "alpha", "beta" } };
    static command_t runelf_path_cmd = { 4, { "runelf", "apps/demo/hello", "alpha", "beta" } };
    static command_t runelf_nowait_cmd = { 2, { "runelf_nowait", "apps/tests/ticks" } };
    static command_t compiler_demo_cmd = { 2, { "runelf", "apps/tests/compiler_demo" } };
    static command_t tinycc_cmd = { 3, { "runelf", "tools/tcc.elf", "-v" } };
    static command_t tccmini_build_cmd = { 6, { "runelf", "tools/tcc.elf", "-nostdlib", "-o", "tccmini.elf", "samples/tccmini.c" } };
    static command_t tccmini_run_cmd = { 2, { "runelf", "tccmini" } };

    terminal_puts("shelltest: start\n");

    shelltest_call("help", cmd_help, &help_cmd);
    shelltest_call("clear", cmd_clear, &clear_cmd);
    shelltest_call("echo", cmd_echo, &echo_cmd);
    shelltest_call("about", cmd_about, &about_cmd);
    shelltest_call("uptime", cmd_uptime, &uptime_cmd);
    shelltest_call("meminfo", cmd_meminfo, &meminfo_cmd);
    shelltest_call("netinfo", cmd_netinfo, &netinfo_cmd);
    shelltest_call("netsend", cmd_netsend, &netsend_cmd);
    shelltest_call("netrecv", cmd_netrecv, &netrecv_cmd);
    shelltest_call("arpgw", cmd_arpgw, &arpgw_cmd);
    shelltest_call("ping", cmd_ping, &ping_cmd);
    shelltest_call("pinggw", cmd_pinggw, &pinggw_cmd);
    shelltest_call("pingpublic", cmd_pingpublic, &pingpublic_cmd);
    shelltest_call("netcheck", cmd_netcheck, &netcheck_cmd);
    shelltest_call("ataread", cmd_ataread, &ataread_cmd);
    shelltest_call("fsls", cmd_fsls, &fsls_root_cmd);
    shelltest_call("fsls_path", cmd_fsls, &fsls_path_cmd);
    shelltest_call("fsread", cmd_fsread, &fsread_cmd);
    shelltest_call("fsread_path", cmd_fsread, &fsread_path_cmd);
    shelltest_call("cat", cmd_cat, &cat_cmd);
    shelltest_call("touch", cmd_touch, &touch_cmd);
    shelltest_call("fsread_touch", cmd_fsread, &fsread_touch_cmd);
    shelltest_call("mkdir", cmd_mkdir, &mkdir_cmd);
    shelltest_call("fsls_newdir", cmd_fsls, &fsls_newdir_cmd);
    shelltest_call("rmdir", cmd_rmdir, &rmdir_cmd);
    shelltest_call("fsls_removed", cmd_fsls, &fsls_removed_cmd);
    shelltest_call("mkdir_nested_parent", cmd_mkdir, &mkdir_nested_parent_cmd);
    shelltest_call("mkdir_nested_child", cmd_mkdir, &mkdir_nested_child_cmd);
    shelltest_call("fsls_nested", cmd_fsls, &fsls_nested_cmd);
    shelltest_call("rmdir_nested_child", cmd_rmdir, &rmdir_nested_child_cmd);
    shelltest_call("rmdir_nested_parent", cmd_rmdir, &rmdir_nested_parent_cmd);
    shelltest_call("fsls_nested_removed", cmd_fsls, &fsls_nested_removed_cmd);
    shelltest_call("compiler_demo", cmd_runelf, &compiler_demo_cmd);
    shelltest_call("tinycc", cmd_runelf, &tinycc_cmd);
    shelltest_call("mkdir_samples", cmd_mkdir, &mkdir_samples_cmd);
    shelltest_call("mv_tccmath", cmd_mv, &mv_tccmath_cmd);
    shelltest_call("mv_tccagg", cmd_mv, &mv_tccagg_cmd);
    shelltest_call("mv_tcctree", cmd_mv, &mv_tcctree_cmd);
    shelltest_call("mv_tccmini", cmd_mv, &mv_tccmini_cmd);
    shelltest_call("fsls_samples", cmd_fsls, &fsls_samples_cmd);
    shelltest_call("tccmath_build", cmd_runelf, &tccmath_build_cmd);
    shelltest_call("tccmath_run", cmd_runelf, &tccmath_run_cmd);
    shelltest_call("tccagg_build", cmd_runelf, &tccagg_build_cmd);
    shelltest_call("tccagg_run", cmd_runelf, &tccagg_run_cmd);
    shelltest_call("tcctree_build", cmd_runelf, &tcctree_build_cmd);
    shelltest_call("tcctree_run", cmd_runelf, &tcctree_run_cmd);
    shelltest_call("tccmini_build", cmd_runelf, &tccmini_build_cmd);
    shelltest_call("tccmini_run", cmd_runelf, &tccmini_run_cmd);
    shelltest_call("cat", cmd_cat, &cat_cmd);
    shelltest_call("touch", cmd_touch, &touch_cmd);
    shelltest_call("fsread_touch", cmd_fsread, &fsread_touch_cmd);
    shelltest_call("cd", cmd_cd, &cd_demo_cmd);
    shelltest_call("pwd", cmd_pwd, &pwd_demo_cmd);
    shelltest_call("ls", cmd_ls, &ls_demo_cmd);
    shelltest_call("fsread_rel", cmd_fsread, &fsread_rel_cmd);
    shelltest_call("cat_rel", cmd_cat, &cat_rel_cmd);
    shelltest_call("touch_rel", cmd_touch, &touch_rel_cmd);
    shelltest_call("fsread_touch_rel", cmd_fsread, &fsread_touch_rel_cmd);
    shelltest_call("runelf_rel", cmd_runelf, &runelf_rel_cmd);
    shelltest_call("runelf", cmd_runelf, &runelf_cmd);
    shelltest_call("cd_root", cmd_cd, &cd_root_cmd);
    shelltest_call("pwd_root", cmd_pwd, &pwd_root_cmd);
    shelltest_call("runelf_path", cmd_runelf, &runelf_path_cmd);
    shelltest_call("ls_root", cmd_ls, &ls_root_cmd);
    shelltest_call("ls_glob", cmd_ls, &ls_glob_cmd);
    shelltest_call("cp", cmd_cp, &cp_cmd);
    shelltest_call("fsread_copy", cmd_fsread, &fsread_copy_cmd);
    shelltest_call("mv", cmd_mv, &mv_cmd);
    shelltest_call("fsread_moved", cmd_fsread, &fsread_moved_cmd);
    shelltest_call("cp_dir", cmd_cp, &cp_dir_cmd);
    shelltest_call("fsread_dir_copy", cmd_fsread, &fsread_dir_copy_cmd);
    shelltest_call("rm_dir", cmd_rm, &rm_dir_cmd);
    shelltest_call("fsread_dir_removed", cmd_fsread, &fsread_dir_removed_cmd);
    shelltest_call("mv_dir", cmd_mv, &mv_dir_cmd);
    shelltest_call("runelf_nowait", cmd_runelf_nowait, &runelf_nowait_cmd);

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
    static char* hello_argv[] = { "apps/demo/hello", "alpha", "beta", 0 };
    static char* ticks_argv[] = { "apps/tests/ticks", "alpha", "beta", 0 };
    static char* args_argv[] = { "apps/tests/args", "alpha", "beta", 0 };
    static char* runelf_argv[] = { "apps/tests/runelf_test", "alpha", "beta", "gamma", 0 };
    static char* readline_argv[] = { "apps/tests/readline", "alpha", "beta", 0 };
    static char* exec_argv[] = { "apps/tests/exec_test", "alpha", "beta", 0 };
    static char* fileread_argv[] = { "apps/tests/fileread", "alpha", "beta", 0 };
    static char* compiler_demo_argv[] = { "apps/tests/compiler_demo", "alpha", "beta", 0 };
    static char* heapprobe_argv[] = { "apps/tests/heapprobe", "alpha", "beta", 0 };
    static char* statprobe_argv[] = { "apps/tests/statprobe", "alpha", "beta", 0 };
    static char* fileprobe_argv[] = { "apps/tests/fileprobe", "alpha", "beta", 0 };
    static char* sleep_argv[] = { "apps/tests/sleep_test", "alpha", "beta", 0 };
    static char* ptrguard_argv[] = { "apps/tests/ptrguard", "alpha", "beta", 0 };
    static char* preempt_argv[] = { "apps/tests/preempt_test", "alpha", "beta", 0 };
    static char* fault_ud_argv[] = { "apps/tests/fault", "ud", 0 };
    static char* fault_gp_argv[] = { "apps/tests/fault", "gp", 0 };
    static char* fault_de_argv[] = { "apps/tests/fault", "de", 0 };
    static char* fault_br_argv[] = { "apps/tests/fault", "br", 0 };
    static char* fault_pf_argv[] = { "apps/tests/fault", "pf", 0 };
    static command_t shelltest_cmd = { 1, { "shelltest" } };

    const selftest_case_t cases[] = {
        { "hello",       "apps/demo/hello",       3, hello_argv,       0 },
        { "ticks",       "apps/tests/ticks",      1, ticks_argv,       0 },
        { "args",        "apps/tests/args",       3, args_argv,        0 },
        { "runelf_test", "apps/tests/runelf_test",4, runelf_argv,      0 },
        { "readline",    "apps/tests/readline",   1, readline_argv,    0 },
        { "exec_test",   "apps/tests/exec_test",  1, exec_argv,        0 },
        { "fileread",    "apps/tests/fileread",   1, fileread_argv,    0 },
        { "compiler_demo","apps/tests/compiler_demo",1, compiler_demo_argv,0 },
        { "heapprobe",   "apps/tests/heapprobe",  1, heapprobe_argv,   0 },
        { "statprobe",   "apps/tests/statprobe",  1, statprobe_argv,   0 },
        { "fileprobe",   "apps/tests/fileprobe",  1, fileprobe_argv,   0 },
        { "sleep_test",  "apps/tests/sleep_test", 1, sleep_argv,       0 },
        { "ptrguard",    "apps/tests/ptrguard",   1, ptrguard_argv,    0 }, /* syscall pointer regression */
        { "preempt_test","apps/tests/preempt_test",1, preempt_argv,     0 }, /* timer-preemption regression */
        { "fault ud",    "apps/tests/fault",      2, fault_ud_argv,    6 },
        { "fault gp",    "apps/tests/fault",      2, fault_gp_argv,   13 },
        { "fault de",    "apps/tests/fault",      2, fault_de_argv,    0 },
        { "fault br",    "apps/tests/fault",      2, fault_br_argv,    5 },
        { "fault pf",    "apps/tests/fault",      2, fault_pf_argv,   14 },
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
    { "help",          "show commands and program list", cmd_help },
    { "clear",         "clear the screen",              cmd_clear },
    { "echo",          "print arguments via ELF",       cmd_echo },
    { "about",         "show the OS version via ELF",   cmd_about },
    { "halt",          "halt the machine via ELF",      cmd_halt },
    { "reboot",        "reboot the machine via ELF",    cmd_reboot },
    { "uptime",        "show tick and second counts via ELF", cmd_uptime },
    { "meminfo",       "show heap and frame usage",     cmd_meminfo },
    { "netinfo",       "show PCI NIC status",          cmd_netinfo },
    { "netsend",       "queue a test Ethernet frame",  cmd_netsend },
    { "netrecv",       "poll and dump one Ethernet frame", cmd_netrecv },
    { "arpgw",         "resolve the QEMU gateway via ARP", cmd_arpgw },
    { "ping",          "ping an IPv4 address",        cmd_ping },
    { "pinggw",        "ping the QEMU gateway",         cmd_pinggw },
    { "pingpublic",    "ping 1.1.1.1 to probe internet reachability", cmd_pingpublic },
    { "netcheck",      "check gateway and public connectivity", cmd_netcheck },
    { "cd",            "change the shell working directory", cmd_cd },
    { "pwd",           "print the shell working directory", cmd_pwd },
    { "ls",            "list a FAT16 directory",       cmd_ls },
    { "ataread",       "dump raw sector bytes",         cmd_ataread },
    { "fsls",          "list FAT16 root directory",     cmd_fsls },
    { "fsread",        "dump FAT16 file bytes",         cmd_fsread },
    { "cat",           "print a FAT16 file",            cmd_cat },
    { "mkdir",         "create a FAT16 directory",     cmd_mkdir },
    { "rmdir",         "remove a FAT16 directory",     cmd_rmdir },
    { "rm",            "remove a FAT16 file",          cmd_rm },
    { "touch",         "create or truncate a FAT16 file", cmd_touch },
    { "cp",            "copy a FAT16 file",            cmd_cp },
    { "mv",            "move or rename a FAT16 entry", cmd_mv },
    { "runelf",        "run a FAT16 ELF and wait",      cmd_runelf },
    { "runelf_nowait", "run a FAT16 ELF and return",    cmd_runelf_nowait },
    { "selftest",      "run shipped ELF self-tests",    cmd_selftest },
    { "shelltest",     "run built-in shell command tests", cmd_shelltest },
};

#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

typedef struct {
    const char* name;
    const char* help;
} program_entry_t;

static program_entry_t programs[] = {
    { "echo",         "print arguments" },
    { "about",        "show the OS version" },
    { "uptime",       "show tick and second counts" },
    { "halt",         "halt the machine" },
    { "reboot",       "reboot the machine" },
    { "netinfo",      "show PCI NIC status" },
    { "netsend",      "queue a test Ethernet frame" },
    { "netrecv",      "poll and dump one Ethernet frame" },
    { "arpgw",        "resolve the QEMU gateway via ARP" },
    { "ping",         "ping an IPv4 address" },
    { "pinggw",       "ping the QEMU gateway" },
    { "pingpublic",   "ping 1.1.1.1 to probe internet reachability" },
    { "netcheck",     "check gateway and public connectivity" },
    { "apps/demo/hello",       "print argc/argv and tick count" },
    { "apps/tests/ticks",      "print the current tick count" },
    { "apps/tests/args",       "print argc and argv" },
    { "apps/tests/runelf_test","verify ELF loading, syscalls, and stack setup" },
    { "apps/tests/readline",   "interactive SYS_READ demo" },
    { "apps/tests/exec_test",  "exercise SYS_EXEC semantics" },
    { "apps/tests/fileread",   "exercise SYS_OPEN / SYS_FREAD / SYS_CLOSE" },
    { "apps/tests/compiler_demo", "exercise SYS_WRITEFILE / SYS_WRITEFILE_PATH and readback" },
    { "apps/tests/heapprobe",  "exercise malloc/free/realloc/calloc" },
    { "apps/tests/statprobe",  "exercise SYS_STAT and path probing" },
    { "apps/tests/fileprobe",  "exercise small file wrapper helpers" },
    { "apps/tests/sleep_test", "exercise SYS_SLEEP semantics" },
    { "apps/tests/ptrguard",   "exercise syscall pointer validation" },
    { "apps/tests/preempt_test","prove timer-driven preemption" },
    { "apps/tests/fault",      "fault probe (ud/gp/de/br/pf)" },
};

#define PROGRAM_COUNT (sizeof(programs) / sizeof(programs[0]))

static void print_command_list(void) {
    for (unsigned int i = 0; i < COMMAND_COUNT; i++) {
        terminal_puts("  ");
        terminal_puts(commands[i].name);

        unsigned int name_len = 0;
        while (commands[i].name[name_len]) name_len++;
        if (name_len < 16) {
            for (unsigned int pad = name_len; pad < 16; pad++) {
                terminal_putc(' ');
            }
        } else {
            terminal_putc(' ');
        }

        terminal_puts(commands[i].help);
        terminal_putc('\n');
    }
}

static void print_program_list(void) {
    for (unsigned int i = 0; i < PROGRAM_COUNT; i++) {
        terminal_puts("  ");
        terminal_puts(programs[i].name);

        unsigned int name_len = 0;
        while (programs[i].name[name_len]) name_len++;
        if (name_len < 16) {
            for (unsigned int pad = name_len; pad < 16; pad++) {
                terminal_putc(' ');
            }
        } else {
            terminal_putc(' ');
        }

        terminal_puts(programs[i].help);
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

    terminal_puts("Unknown command: ");
    terminal_puts(cmd->argv[0]);
    terminal_putc('\n');
}
