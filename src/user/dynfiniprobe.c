#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"

int main(void) {
    char buf[32];
    int fd;
    int n;

    fd = open("dynfini.out", O_RDONLY);
    if (fd < 0) {
        puts("dynfiniprobe open: FAIL");
        return 1;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) {
        puts("dynfiniprobe read: FAIL");
        return 1;
    }
    buf[n] = 0;
    if (strcmp(buf, "dynfini fini\n") != 0) {
        puts("dynfiniprobe content: FAIL");
        return 1;
    }
    unlink("dynfini.out");
    puts("dynfiniprobe: PASS");
    return 0;
}
