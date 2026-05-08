#include "user_lib.h"

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    char* child_argv[] = {
        "apps/tests/spinwkr",
        "late",
        "pgrp_child.txt",
        "120",
        0
    };

    u_puts("pgrpprobe start\n");
    int rc = sys_exec("apps/tests/spinwkr", 4, child_argv);
    if (rc < 0) {
        u_puts("pgrpprobe child launch failed\n");
        sys_exit(1);
    }

    u_puts("pgrpprobe child launched\n");
    u_puts("pgrpprobe waiting\n");
    for (;;) {
        sys_sleep(20);
    }
}
