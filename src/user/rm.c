#include "user_lib.h"

void _start(int argc, char** argv) {
    if (argc < 2) {
        u_puts("usage: rm <path>\n");
        sys_exit(1);
    }

    if (u_unlink(argv[1]) < 0) {
        u_puts("rm: failed\n");
        sys_exit(1);
    }

    u_puts("rm: ");
    u_puts(argv[1]);
    u_putc('\n');
    sys_exit(0);
}
