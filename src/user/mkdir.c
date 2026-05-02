#include "user_lib.h"

void _start(int argc, char** argv) {
    if (argc < 2) {
        u_puts("usage: mkdir <path>\n");
        sys_exit(1);
    }

    if (sys_mkdir(argv[1], 0) < 0) {
        u_puts("mkdir: failed\n");
        sys_exit(1);
    }

    u_puts("mkdir: ");
    u_puts(argv[1]);
    u_putc('\n');
    sys_exit(0);
}
