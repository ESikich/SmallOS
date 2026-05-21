#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"
#include "unistd.h"

static void put_hex_byte(unsigned char b) {
    static const char hex[] = "0123456789ABCDEF";
    putchar(hex[b >> 4]);
    putchar(hex[b & 0xF]);
}

void _start(int argc, char** argv) {
    if (argc < 2) {
        fputs("usage: fsread <path>\n", stderr);
        exit(1);
    }

    struct stat st;
    if (stat(argv[1], &st) < 0 || S_ISDIR(st.st_mode)) {
        printf("ext2: not found: %s\n", argv[1]);
        fputs("fsread: load failed\n", stderr);
        exit(1);
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        fputs("fsread: load failed\n", stderr);
        exit(1);
    }

    unsigned char buf[16];
    int n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0) {
        fputs("fsread: load failed\n", stderr);
        exit(1);
    }

    printf("fsread: %s  %u bytes\nfirst 16: ", argv[1], (unsigned int)st.st_size);
    for (int i = 0; i < n; i++) {
        put_hex_byte(buf[i]);
        putchar(' ');
    }
    putchar('\n');
    exit(0);
}
