#include "unistd.h"
#include "fcntl.h"
#include "sys/wait.h"
#include "stdio.h"

static int value = 11;

int main(void) {
    int status = 0;
    pid_t pid;
    int fd;
    unsigned char byte;

    fd = open("usr/bin/hello.elf", O_RDONLY);
    if (fd < 0 || read(fd, &byte, 1) != 1 || byte != 0x7fu) {
        puts("forkprobe file setup: FAIL");
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        puts("forkprobe fork: FAIL");
        return 1;
    }
    if (pid == 0) {
        value = 42;
        if (read(fd, &byte, 1) != 1 || byte != 'E') {
            return 3;
        }
        return value == 42 ? 8 : 2;
    }

    if (value != 11) {
        puts("forkprobe memory: FAIL");
        return 1;
    }
    if (waitpid(pid, &status, 0) != pid || status != (8 << 8)) {
        puts("forkprobe wait: FAIL");
        return 1;
    }
    if (read(fd, &byte, 1) != 1 || byte != 'L') {
        puts("forkprobe fd offset: FAIL");
        return 1;
    }
    close(fd);
    puts("forkprobe: PASS");
    return 0;
}
