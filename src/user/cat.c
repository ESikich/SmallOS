#include "user_lib.h"

void _start(int argc, char** argv) {
    if (argc < 2) {
        u_puts("usage: cat <path>\n");
        sys_exit(1);
    }

    int fd = sys_open(argv[1]);
    if (fd < 0) {
        u_puts("cat: failed\n");
        sys_exit(1);
    }

    char buf[4096];
    for (;;) {
        int n = sys_fread(fd, buf, sizeof(buf));
        if (n < 0) {
            sys_close(fd);
            u_puts("cat: failed\n");
            sys_exit(1);
        }
        if (n == 0) {
            break;
        }
        if (u_writefd(1, buf, (uint32_t)n) < 0) {
            sys_close(fd);
            u_puts("cat: failed\n");
            sys_exit(1);
        }
    }

    sys_close(fd);
    sys_exit(0);
}
