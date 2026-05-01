#include "user_lib.h"

/*
 * fileread — end-to-end test for SYS_OPEN / SYS_FREAD / SYS_CLOSE.
 *
 * Opens a nested file from the FAT16 partition
 * (apps/demo/hello.elf), reads the first 16 bytes, dumps them as hex,
 * reads the rest to confirm EOF behaviour, then closes the fd.  Also
 * tests double-close and bad-fd error returns.
 *
 * Expected output for a well-formed ELF:
 *   first 16 bytes: 7F 45 4C 46 ...   (ELF magic)
 *   fd close: ok
 *
 * Usage:
 *   runelf apps/tests/fileread
 */

static void put_hex_byte(unsigned char b) {
    static const char hex[] = "0123456789ABCDEF";
    u_putc(hex[b >> 4]);
    u_putc(hex[b & 0xF]);
}

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u_puts("fileread test\n");
    u_puts("-------------\n");

    /* --- open --- */
    int fd = sys_open("apps/demo/hello.elf");
    if (fd < 0) {
        u_puts("open failed\n");
        sys_exit(1);
    }
    u_puts("opened fd=");
    u_put_uint((uint32_t)fd);
    u_putc('\n');

    /* --- read first 16 bytes --- */
    char buf[16];
    int n = sys_fread(fd, buf, 16);
    if (n < 0) {
        u_puts("fread error\n");
        sys_close(fd);
        sys_exit(1);
    }
    u_puts("first ");
    u_put_uint((uint32_t)n);
    u_puts(" bytes:");
    for (int i = 0; i < n; i++) {
        u_putc(' ');
        put_hex_byte((unsigned char)buf[i]);
    }
    u_putc('\n');

    /* --- drain the rest to confirm EOF --- */
    char drain[256];
    uint32_t total = (uint32_t)n;
    int got;
    while ((got = sys_fread(fd, drain, sizeof(drain))) > 0) {
        total += (uint32_t)got;
    }
    if (got < 0) {
        u_puts("fread error mid-file\n");
        sys_close(fd);
        sys_exit(1);
    }
    u_puts("total bytes read: ");
    u_put_uint(total);
    u_putc('\n');

    /* --- close --- */
    int r = sys_close(fd);
    u_puts("close: ");
    u_puts(r == 0 ? "ok" : "error");
    u_putc('\n');

    /* --- double-close should return -1 --- */
    r = sys_close(fd);
    u_puts("double-close: ");
    u_puts(r < 0 ? "ok (got -1)" : "ERROR (should have failed)");
    u_putc('\n');

    /* --- bad fd --- */
    r = sys_fread(99, buf, 1);
    u_puts("fread bad fd: ");
    u_puts(r < 0 ? "ok (got -1)" : "ERROR (should have failed)");
    u_putc('\n');

    sys_exit(0);
}
