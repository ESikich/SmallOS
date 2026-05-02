#include "fcntl.h"
#include "unistd.h"
#include "user_lib.h"

void _start(int argc, char** argv) {
    if (argc < 2) {
        u_puts("usage: touch <path>\n");
        sys_exit(1);
    }

    int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        u_puts("touch: failed\n");
        sys_exit(1);
    }
    close(fd);

    u_puts("touch: ");
    u_puts(argv[1]);
    u_putc('\n');
    sys_exit(0);
}
