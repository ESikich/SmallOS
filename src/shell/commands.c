#include "commands.h"
#include "terminal.h"
#include "system.h"
#include "timer.h"
#include "elf_loader.h"
#include "memory.h"
#include "pmm.h"
#include "ata.h"
#include "fat16.h"

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static void cmd_help(command_t* cmd) {
    (void)cmd;
    terminal_puts("Commands:\n");
    terminal_puts("  help\n");
    terminal_puts("  clear\n");
    terminal_puts("  echo\n");
    terminal_puts("  about\n");
    terminal_puts("  halt\n");
    terminal_puts("  reboot\n");
    terminal_puts("  uptime\n");
    terminal_puts("  meminfo\n");
    terminal_puts("  ataread <lba>      dump first 32 bytes of a sector\n");
    terminal_puts("  fsls               list FAT16 root directory\n");
    terminal_puts("  fsread <n>         dump first 16 bytes of a FAT16 file\n");
    terminal_puts("  runelf <n> [args]\n");
}

static void cmd_clear(command_t* cmd) {
    (void)cmd;
    terminal_clear();
}

static void cmd_echo(command_t* cmd) {
    for (int i = 1; i < cmd->argc; i++) {
        terminal_puts(cmd->argv[i]);
        if (i != cmd->argc - 1) terminal_putc(' ');
    }
    terminal_putc('\n');
}

static void cmd_about(command_t* cmd) {
    (void)cmd;
    terminal_puts("SmallOS v0.1\n");
}

static void cmd_halt(command_t* cmd) {
    (void)cmd;
    terminal_puts("System halted.\n");
    system_halt();
}

static void cmd_reboot(command_t* cmd) {
    (void)cmd;
    terminal_puts("Rebooting...\n");
    system_reboot();
}

static void cmd_uptime(command_t* cmd) {
    (void)cmd;
    terminal_puts("Ticks: ");
    terminal_put_uint(timer_get_ticks());
    terminal_putc('\n');
    terminal_puts("Seconds: ");
    terminal_put_uint(timer_get_seconds());
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
        terminal_puts("sig: ");
        terminal_put_hex(sector[510]);
        terminal_putc(' ');
        terminal_put_hex(sector[511]);
        terminal_puts(" (expect 0x55 0xAA)\n");
        terminal_puts("fat16_lba patch: ");
        unsigned int patched_lba = sector[504]
                                 | ((unsigned int)sector[505] << 8)
                                 | ((unsigned int)sector[506] << 16)
                                 | ((unsigned int)sector[507] << 24);
        terminal_put_uint(patched_lba);
        terminal_putc('\n');
    }
}

/*
 * fsls — list FAT16 root directory
 */
static void cmd_fsls(command_t* cmd) {
    (void)cmd;
    fat16_ls();
}

/*
 * fsread <n> — load a file from FAT16 and dump its first 16 bytes.
 * Expected: 7F 45 4C 46 for any ELF file.
 */
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
    if (!elf_run_named(cmd->argv[1], cmd->argc - 1, &cmd->argv[1])) {
        terminal_puts("runelf: failed\n");
    }
}

static command_entry_t commands[] = {
    { "help",    cmd_help },
    { "clear",   cmd_clear },
    { "echo",    cmd_echo },
    { "about",   cmd_about },
    { "halt",    cmd_halt },
    { "reboot",  cmd_reboot },
    { "uptime",  cmd_uptime },
    { "meminfo", cmd_meminfo },
    { "ataread", cmd_ataread },
    { "fsls",    cmd_fsls },
    { "fsread",  cmd_fsread },
    { "runelf",  cmd_runelf },
};

#define COMMAND_COUNT (sizeof(commands) / sizeof(commands[0]))

void commands_execute(command_t* cmd) {
    if (cmd->argc == 0) return;

    for (unsigned int i = 0; i < COMMAND_COUNT; i++) {
        if (str_eq(cmd->argv[0], commands[i].name)) {
            commands[i].fn(cmd);
            return;
        }
    }

    terminal_puts("Unknown command: ");
    terminal_puts(cmd->argv[0]);
    terminal_putc('\n');
}