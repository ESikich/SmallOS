#include "commands.h"
#include "shell.h"
#include "terminal.h"
#include "elf_loader.h"
#include "vfs.h"
#include "process.h"
#include "klib.h"

static void print_command_list(void);
static int run_app_command(command_t* cmd, const char* program);
static int resolve_app_command_path(const char* name, char* out, unsigned int out_size);
static int resolve_shell_path_arg(const char* input, char* out, unsigned int out_size);
static int command_name_has_path_sep(const char* name);
static int path_has_dot(const char* path);
static int path_copy3(char* out, unsigned int out_size, const char* a, const char* b, const char* c);
static void terminal_put_cwd(void);

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
    { "mousetest",     "print mouse events for 5 seconds", 0 },
    { "usbinfo",       "show USB controller power diagnostics", 0 },
    { "usbdiag",       "run USB port and descriptor diagnostics", 0 },
    { "usbports",      "dump passive USB port status", 0 },
    { "usbpeek",       "read OHCI address-0 device descriptor", 0 },
    { "usbpower",      "try OHCI root-hub port power", 0 },
    { "usbmouse",      "probe OHCI boot mouse",        0 },
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
