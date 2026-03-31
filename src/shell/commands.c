#include "commands.h"
#include "terminal.h"
#include "system.h"
#include "timer.h"
#include "programs.h"
#include "images.h"
#include "elf_loader.h"
#include "memory.h"
#include "pmm.h"

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
    terminal_puts("  run\n");
    terminal_puts("  runimg\n");
    terminal_puts("  runelf\n");
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
    terminal_puts("SimpleOS v0.1\n");
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

/*
 * cmd_meminfo — print heap and PMM frame allocator state.
 *
 * Example output:
 *   heap:   base 0x100000  top 0x104000  used 16 KB
 *   frames: 4080 free / 4096 total  (16320 KB / 16384 KB)
 *   used:   16 frames (64 KB)
 */
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

static void cmd_run(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("Usage: run <program> [args...]\n");
        return;
    }
    programs_run(cmd->argv[1], cmd->argc - 1, &cmd->argv[1]);
}

static void cmd_runimg(command_t* cmd) {
    if (cmd->argc < 2) {
        terminal_puts("Usage: runimg <image> [args...]\n");
        return;
    }
    images_run(cmd->argv[1], cmd->argc - 1, &cmd->argv[1]);
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
    { "run",     cmd_run },
    { "runimg",  cmd_runimg },
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