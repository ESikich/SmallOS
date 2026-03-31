#include "programs.h"
#include "terminal.h"

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static void prog_hello(int argc, char** argv) {
    (void)argc;
    (void)argv;
    terminal_puts("Hello from program 'hello'\n");
}

static void prog_args(int argc, char** argv) {
    terminal_puts("argc = ");

    char buf[16];
    int n = argc;
    int i = 0;

    if (n == 0) {
        terminal_putc('0');
    } else {
        while (n > 0) {
            buf[i++] = '0' + (n % 10);
            n /= 10;
        }
        while (i > 0) {
            terminal_putc(buf[--i]);
        }
    }

    terminal_putc('\n');

    for (int j = 0; j < argc; j++) {
        terminal_puts("argv[");
        if (j >= 10) {
            terminal_putc('0' + (j / 10));
        }
        terminal_putc('0' + (j % 10));
        terminal_puts("] = ");
        terminal_puts(argv[j]);
        terminal_putc('\n');
    }
}

static program_entry_desc_t builtins[] = {
    { "hello", prog_hello },
    { "args",  prog_args  },
};

#define PROGRAM_COUNT (sizeof(builtins) / sizeof(builtins[0]))

void programs_run(const char* name, int argc, char** argv) {
    for (unsigned int i = 0; i < PROGRAM_COUNT; i++) {
        if (str_eq(name, builtins[i].name)) {
            builtins[i].entry(argc, argv);
            return;
        }
    }

    terminal_puts("Program not found: ");
    terminal_puts(name);
    terminal_putc('\n');
}