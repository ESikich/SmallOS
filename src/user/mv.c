#include "user_lib.h"

void _start(int argc, char** argv) {
    if (argc < 3) {
        u_puts("usage: mv <src> <dst>\n");
        sys_exit(1);
    }

    if (u_rename(argv[1], argv[2]) < 0) {
        u_puts("mv: failed\n");
        sys_exit(1);
    }

    u_puts("mv: ");
    u_puts(argv[1]);
    u_puts(" -> ");
    u_puts(argv[2]);
    u_putc('\n');
    sys_exit(0);
}
