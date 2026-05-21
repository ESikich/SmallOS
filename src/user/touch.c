#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

void _start(int argc, char** argv) {
    if (argc < 2) {
        fputs("usage: touch <path>\n", stderr);
        exit(1);
    }

    int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        fputs("touch: failed\n", stderr);
        exit(1);
    }
    close(fd);

    printf("touch: %s\n", argv[1]);
    exit(0);
}
