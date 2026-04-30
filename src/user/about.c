#include "user_lib.h"

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u_puts("SmallOS v0.1\n");
    sys_exit(0);
}
