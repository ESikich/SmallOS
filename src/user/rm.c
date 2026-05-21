#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

void _start(int argc, char** argv) {
    if (argc < 2) {
        fputs("usage: rm <path>\n", stderr);
        exit(1);
    }

    if (unlink(argv[1]) < 0) {
        fputs("rm: failed\n", stderr);
        exit(1);
    }

    printf("rm: %s\n", argv[1]);
    exit(0);
}
