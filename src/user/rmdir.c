#include "user_lib.h"

void _start(int argc, char** argv) {
    if (argc < 2) {
        u_puts("usage: rmdir <path>\n");
        sys_exit(1);
    }

    if (sys_rmdir(argv[1]) < 0) {
        u_puts("rmdir: failed\n");
        sys_exit(1);
    }

    u_puts("rmdir: ");
    u_puts(argv[1]);
    u_putc('\n');
    sys_exit(0);
}
