#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

void _start(int argc, char** argv) {
    if (argc < 3) {
        fputs("usage: mv <src> <dst>\n", stderr);
        exit(1);
    }

    if (rename(argv[1], argv[2]) < 0) {
        fputs("mv: failed\n", stderr);
        exit(1);
    }

    printf("mv: %s -> %s\n", argv[1], argv[2]);
    exit(0);
}
