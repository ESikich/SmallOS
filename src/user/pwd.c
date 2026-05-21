#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    char cwd[128];
    if (getcwd(cwd, sizeof(cwd)) == 0) {
        fputs("pwd: failed\n", stderr);
        exit(1);
    }

    printf("pwd: %s\n", cwd);
    exit(0);
}
