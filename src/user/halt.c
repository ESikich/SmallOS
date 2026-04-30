#include "user_lib.h"

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u_puts("System halted.\n");
    sys_halt();
    sys_exit(0);
}
