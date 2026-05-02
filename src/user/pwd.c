#include "user_lib.h"

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    char cwd[128];
    if (u_getcwd(cwd, sizeof(cwd)) < 0) {
        u_puts("pwd: failed\n");
        sys_exit(1);
    }

    u_puts("pwd: ");
    u_puts(cwd);
    u_putc('\n');
    sys_exit(0);
}
