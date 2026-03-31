#include "user_lib.h"

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u_puts("ticks program\n");
    u_puts("ticks = ");
    u_put_uint(sys_get_ticks());
    u_puts("\n");

    sys_exit(0);
}