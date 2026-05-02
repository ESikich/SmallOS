#include "user_lib.h"

static void put_hex_byte(unsigned char b) {
    static const char hex[] = "0123456789ABCDEF";
    u_putc(hex[b >> 4]);
    u_putc(hex[b & 0xF]);
}

void _start(int argc, char** argv) {
    if (argc < 2) {
        u_puts("usage: fsread <n>\n");
        sys_exit(1);
    }

    uint32_t size = 0;
    int is_dir = 0;
    if (u_stat(argv[1], &size, &is_dir) < 0 || is_dir) {
        u_puts("fat16: not found: ");
        u_puts(argv[1]);
        u_putc('\n');
        u_puts("fsread: load failed\n");
        sys_exit(1);
    }

    int fd = sys_open(argv[1]);
    if (fd < 0) {
        u_puts("fsread: load failed\n");
        sys_exit(1);
    }

    unsigned char buf[16];
    int n = sys_fread(fd, (char*)buf, sizeof(buf));
    sys_close(fd);
    if (n < 0) {
        u_puts("fsread: load failed\n");
        sys_exit(1);
    }

    u_puts("fsread: ");
    u_puts(argv[1]);
    u_puts("  ");
    u_put_uint(size);
    u_puts(" bytes\nfirst 16: ");
    for (int i = 0; i < n; i++) {
        put_hex_byte(buf[i]);
        u_putc(' ');
    }
    u_putc('\n');
    sys_exit(0);
}
