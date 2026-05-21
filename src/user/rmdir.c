#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

void _start(int argc, char** argv) {
    if (argc < 2) {
        fputs("usage: rmdir <path>\n", stderr);
        exit(1);
    }

    if (rmdir(argv[1]) < 0) {
        fputs("rmdir: failed\n", stderr);
        exit(1);
    }

    printf("rmdir: %s\n", argv[1]);
    exit(0);
}
