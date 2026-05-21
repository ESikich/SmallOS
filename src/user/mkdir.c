#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"

void _start(int argc, char** argv) {
    if (argc < 2) {
        fputs("usage: mkdir <path>\n", stderr);
        exit(1);
    }

    if (mkdir(argv[1], 0) < 0) {
        fputs("mkdir: failed\n", stderr);
        exit(1);
    }

    printf("mkdir: %s\n", argv[1]);
    exit(0);
}
