#include "commands.h"
#include "terminal.h"
#include "elf_loader.h"
#include "memory.h"
#include "pmm.h"
#include "ata.h"
#include "fat16.h"
#include "process.h"
#include "klib.h"

static void print_command_list(void);
static void print_program_list(void);
static void run_elf_command(command_t* cmd, const char* program);

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
    process_t* proc = elf_run_named(program, cmd->argc, cmd->argv);
    if (!proc) {
        terminal_puts(program);
        terminal_puts(": failed\n");
        return;
    }

    process_wait(proc);
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
    (void)cmd;
    fat16_ls();
}

static void cmd_fsread(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("usage: fsread <n>\n");
        return;
    }

    unsigned int size = 0;
    const unsigned char* data = fat16_load(cmd->argv[1], &size);
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

static void cmd_runelf(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("Usage: runelf <n>\n");
        return;
    }

    process_t* proc = elf_run_named(cmd->argv[1], cmd->argc - 1, &cmd->argv[1]);
    if (!proc) {
        terminal_puts("runelf: failed\n");
        return;
    }

    process_wait(proc);
}

static void cmd_runelf_nowait(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("Usage: runelf_nowait <n>\n");
        return;
    }

    if (!elf_run_named(cmd->argv[1], cmd->argc - 1, &cmd->argv[1])) {
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

    command_t help_cmd = { 1, { "help" } };
    command_t clear_cmd = { 1, { "clear" } };
    command_t echo_cmd = { 4, { "echo", "alpha", "beta", "gamma" } };
    command_t about_cmd = { 1, { "about" } };
    command_t uptime_cmd = { 1, { "uptime" } };
    command_t meminfo_cmd = { 1, { "meminfo" } };
    command_t ataread_cmd = { 2, { "ataread", "0" } };
    command_t fsls_cmd = { 1, { "fsls" } };
    command_t fsread_cmd = { 2, { "fsread", "hello.elf" } };
    command_t runelf_cmd = { 2, { "runelf", "hello" } };
    command_t runelf_nowait_cmd = { 2, { "runelf_nowait", "ticks" } };
    command_t compiler_demo_cmd = { 2, { "runelf", "compiler_demo" } };

    terminal_puts("shelltest: start\n");

    shelltest_call("help", cmd_help, &help_cmd);
    shelltest_call("clear", cmd_clear, &clear_cmd);
    shelltest_call("echo", cmd_echo, &echo_cmd);
    shelltest_call("about", cmd_about, &about_cmd);
    shelltest_call("uptime", cmd_uptime, &uptime_cmd);
    shelltest_call("meminfo", cmd_meminfo, &meminfo_cmd);
    shelltest_call("ataread", cmd_ataread, &ataread_cmd);
    shelltest_call("fsls", cmd_fsls, &fsls_cmd);
    shelltest_call("fsread", cmd_fsread, &fsread_cmd);
    shelltest_call("runelf", cmd_runelf, &runelf_cmd);
    shelltest_call("runelf_nowait", cmd_runelf_nowait, &runelf_nowait_cmd);
    shelltest_call("compiler_demo", cmd_runelf, &compiler_demo_cmd);

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

    process_t* proc = elf_run_named(tc->program, tc->argc, tc->argv);
    if (!proc) {
        terminal_puts("FAIL (launch)\n");
        return 0;
    }

    int status = process_wait(proc);
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

    char* hello_argv[] = { "hello", "alpha", "beta", 0 };
    char* ticks_argv[] = { "ticks.elf", 0 };
    char* args_argv[] = { "args", "alpha", "beta", 0 };
    char* runelf_argv[] = { "runelf_test.elf", "alpha", "beta", "gamma", 0 };
    char* readline_argv[] = { "readline.elf", 0 };
    char* exec_argv[] = { "exec_test.elf", 0 };
    char* fileread_argv[] = { "fileread.elf", 0 };
    char* compiler_demo_argv[] = { "compiler_demo.elf", 0 };
    char* sleep_argv[] = { "sleep_test.elf", 0 };
    char* ptrguard_argv[] = { "ptrguard.elf", 0 };
    char* preempt_argv[] = { "preempt_test.elf", 0 };
    char* fault_ud_argv[] = { "fault.elf", "ud", 0 };
    char* fault_gp_argv[] = { "fault.elf", "gp", 0 };
    char* fault_de_argv[] = { "fault.elf", "de", 0 };
    char* fault_br_argv[] = { "fault.elf", "br", 0 };
    char* fault_pf_argv[] = { "fault.elf", "pf", 0 };
    command_t shelltest_cmd = { 1, { "shelltest" } };

    const selftest_case_t cases[] = {
        { "hello",       "hello.elf",       3, hello_argv,       0 },
        { "ticks",       "ticks.elf",       1, ticks_argv,       0 },
        { "args",        "args.elf",        3, args_argv,        0 },
        { "runelf_test", "runelf_test.elf", 4, runelf_argv,      0 },
        { "readline",    "readline.elf",    1, readline_argv,    0 },
        { "exec_test",   "exec_test.elf",   1, exec_argv,        0 },
        { "fileread",    "fileread.elf",    1, fileread_argv,    0 },
        { "compiler_demo","compiler_demo.elf",1, compiler_demo_argv,0 },
        { "sleep_test",  "sleep_test.elf",  1, sleep_argv,       0 },
        { "ptrguard",    "ptrguard.elf",    1, ptrguard_argv,    0 }, /* syscall pointer regression */
        { "preempt_test","preempt_test.elf",1, preempt_argv,     0 }, /* timer-preemption regression */
        { "fault ud",    "fault.elf",       2, fault_ud_argv,    6 },
        { "fault gp",    "fault.elf",       2, fault_gp_argv,   13 },
        { "fault de",    "fault.elf",       2, fault_de_argv,    0 },
        { "fault br",    "fault.elf",       2, fault_br_argv,    5 },
        { "fault pf",    "fault.elf",       2, fault_pf_argv,   14 },
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
    { "ataread",       "dump raw sector bytes",         cmd_ataread },
    { "fsls",          "list FAT16 root directory",     cmd_fsls },
    { "fsread",        "dump FAT16 file bytes",         cmd_fsread },
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
    { "hello",       "print argc/argv and tick count" },
    { "ticks",       "print the current tick count" },
    { "args",        "print argc and argv" },
    { "runelf_test", "verify ELF loading, syscalls, and stack setup" },
    { "readline",    "interactive SYS_READ demo" },
    { "exec_test",   "exercise SYS_EXEC semantics" },
    { "fileread",    "exercise SYS_OPEN / SYS_FREAD / SYS_CLOSE" },
    { "compiler_demo", "exercise SYS_WRITEFILE and readback" },
    { "sleep_test",  "exercise SYS_SLEEP semantics" },
    { "ptrguard",    "exercise syscall pointer validation" },
    { "preempt_test","prove timer-driven preemption" },
    { "fault",       "fault probe (ud/gp/de/br/pf)" },
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
