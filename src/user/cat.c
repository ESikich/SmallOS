#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

void _start(int argc, char** argv) {
    int fd = STDIN_FILENO;

    if (argc >= 2) {
        fd = open(argv[1], O_RDONLY);
    }
    if (fd < 0) {
        fputs("cat: failed\n", stderr);
        exit(1);
    }

    char buf[4096];
    for (;;) {
        int n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            close(fd);
            fputs("cat: failed\n", stderr);
            exit(1);
        }
        if (n == 0) {
            break;
        }
        int off = 0;
        while (off < n) {
            int written = write(STDOUT_FILENO, buf + off, (unsigned int)(n - off));
            if (written <= 0) {
                close(fd);
                fputs("cat: failed\n", stderr);
                exit(1);
            }
            off += written;
        }
    }

    if (fd != STDIN_FILENO) close(fd);
    exit(0);
}
