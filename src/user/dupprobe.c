#include "unistd.h"
#include "fcntl.h"
#include "stdio.h"

int main(void) {
    int fds[2];
    int d;
    char ch;
    unsigned char bytes[2];
    int file_fd;
    int file_dup;

    if (pipe(fds) < 0) {
        puts("dupprobe pipe: FAIL");
        return 1;
    }
    d = dup(fds[1]);
    if (d < 0) {
        puts("dupprobe dup: FAIL");
        return 1;
    }
    close(fds[1]);
    if (write(d, "z", 1) != 1 || read(fds[0], &ch, 1) != 1 || ch != 'z') {
        puts("dupprobe dup pipe: FAIL");
        return 1;
    }
    if (dup2(d, 7) != 7 || write(7, "q", 1) != 1 ||
        read(fds[0], &ch, 1) != 1 || ch != 'q') {
        puts("dupprobe dup2: FAIL");
        return 1;
    }
    if (fcntl(7, F_SETFD, FD_CLOEXEC) < 0 ||
        (fcntl(7, F_GETFD) & FD_CLOEXEC) == 0) {
        puts("dupprobe cloexec: FAIL");
        return 1;
    }
    close(7);
    close(d);
    close(fds[0]);

    file_fd = open("usr/bin/hello.elf", O_RDONLY);
    if (file_fd < 0) {
        puts("dupprobe file open: FAIL");
        return 1;
    }
    file_dup = dup(file_fd);
    if (file_dup < 0) {
        puts("dupprobe file dup: FAIL");
        return 1;
    }
    if (read(file_fd, bytes, 1) != 1 ||
        read(file_dup, bytes + 1, 1) != 1 ||
        bytes[0] != 0x7fu || bytes[1] != 'E') {
        puts("dupprobe file offset: FAIL");
        return 1;
    }
    close(file_fd);
    close(file_dup);

    puts("dupprobe: PASS");
    return 0;
}
