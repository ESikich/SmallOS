#include "user_lib.h"

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u_puts("Rebooting...\n");
    sys_reboot();
    sys_exit(0);
}
