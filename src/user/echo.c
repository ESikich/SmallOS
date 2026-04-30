#include "user_lib.h"

void _start(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        u_puts(argv[i]);
        if (i != argc - 1) {
            u_putc(' ');
        }
    }
    u_putc('\n');
    sys_exit(0);
}
