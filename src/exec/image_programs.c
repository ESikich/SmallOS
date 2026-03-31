#include "terminal.h"


void img_hello_main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    terminal_puts("Hello from loaded image 'hello'\n");
}

void img_args_main(int argc, char** argv) {
    terminal_puts("argc = ");
    terminal_put_uint((unsigned int)argc);
    terminal_putc('\n');

    for (int i = 0; i < argc; i++) {
        terminal_puts("argv[");
        terminal_put_uint((unsigned int)i);
        terminal_puts("] = ");
        terminal_puts(argv[i]);
        terminal_putc('\n');
    }
}