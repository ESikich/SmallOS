#include "errno.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "sys/ioctl.h"
#include "termios.h"
#include "unistd.h"
#include "user_syscall.h"

static int failures = 0;

static void check(const char* name, int ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int read_exactish(int fd, char* buf, int len) {
    int n = (int)read(fd, buf, len);
    if (n >= 0 && n < len) buf[n] = '\0';
    return n;
}

int main(void) {
    int fds[2];
    int master;
    int slave;
    int n;
    int pgid;
    int got_pgid = 0;
    int nullfd;
    char buf[16];
    struct termios tio;
    struct winsize ws;

    puts("ttyprobe start");

    check("pty open", sys_pty_open(fds, SYS_FD_FLAG_NONBLOCK) == 0);
    if (failures) {
        printf("ttyprobe FAIL\n");
        return 1;
    }
    master = fds[0];
    slave = fds[1];

    check("isatty slave", isatty(slave) == 1);
    check("tcgetattr default", tcgetattr(slave, &tio) == 0 &&
                                (tio.c_lflag & (ECHO | ICANON | ISIG)) ==
                                    (ECHO | ICANON | ISIG) &&
                                (tio.c_oflag & OPOST) != 0 &&
                                tio.c_cc[VINTR] == 3 &&
                                tio.c_cc[VEOF] == 4 &&
                                tio.c_cc[VMIN] == 1);

    check("write canonical", write(master, "a\n", 2) == 2);
    n = read_exactish(slave, buf, sizeof(buf) - 1);
    check("canonical read newline", n == 2 && strcmp(buf, "a\n") == 0);
    n = read_exactish(master, buf, sizeof(buf) - 1);
    check("echo default", n == 2 && strcmp(buf, "a\n") == 0);

    check("tcgetattr before echo off", tcgetattr(slave, &tio) == 0);
    tio.c_lflag &= ~ECHO;
    check("tcsetattr echo off", tcsetattr(slave, TCSANOW, &tio) == 0);
    check("tcgetattr echo off", tcgetattr(slave, &tio) == 0 &&
                                  (tio.c_lflag & ECHO) == 0);
    check("write echo off", write(master, "b\n", 2) == 2);
    n = read_exactish(slave, buf, sizeof(buf) - 1);
    check("read echo off data", n == 2 && strcmp(buf, "b\n") == 0);
    errno = 0;
    n = read(master, buf, sizeof(buf));
    check("echo off quiet", n < 0 && errno == EAGAIN);

    tio.c_lflag &= ~(ICANON | ECHO);
    check("tcsetattr raw", tcsetattr(slave, TCSANOW, &tio) == 0);
    check("write raw bytes", write(master, "xy", 2) == 2);
    n = read_exactish(slave, buf, sizeof(buf) - 1);
    check("raw read first byte", n == 1 && buf[0] == 'x');
    n = read_exactish(slave, buf, sizeof(buf) - 1);
    check("raw read second byte", n == 1 && buf[0] == 'y');

    tio.c_lflag &= ~ISIG;
    check("tcsetattr isig off", tcsetattr(slave, TCSANOW, &tio) == 0);
    check("write ctrl-c isig off", write(master, "\003", 1) == 1);
    n = read_exactish(slave, buf, sizeof(buf) - 1);
    check("ctrl-c delivered as data", n == 1 && (unsigned char)buf[0] == 3);

    tio.c_lflag = ICANON;
    check("tcsetattr canonical eof", tcsetattr(slave, TCSANOW, &tio) == 0);
    check("write eof", write(master, "\004", 1) == 1);
    n = read(slave, buf, sizeof(buf));
    check("ctrl-d canonical eof", n == 0);

    check("pty set size", sys_pty_set_size(slave, 33, 101) == 0);
    memset(&ws, 0, sizeof(ws));
    check("ioctl winsize fd", ioctl(slave, TIOCGWINSZ, &ws) == 0 &&
                              ws.ws_row == 33 && ws.ws_col == 101);

    pgid = getpid();
    check("ioctl set pgrp", ioctl(slave, TIOCSPGRP, &pgid) == 0);
    check("ioctl get pgrp", ioctl(slave, TIOCGPGRP, &got_pgid) == 0 &&
                            got_pgid == pgid);

    nullfd = open("/dev/null", O_RDONLY);
    check("open dev null", nullfd >= 0);
    if (nullfd >= 0) {
        errno = 0;
        check("tcgetattr enotty", tcgetattr(nullfd, &tio) < 0 && errno == ENOTTY);
        errno = 0;
        check("ioctl enotty", ioctl(nullfd, TIOCGWINSZ, &ws) < 0 && errno == ENOTTY);
        check("isatty false", isatty(nullfd) == 0);
        close(nullfd);
    }

    close(slave);
    close(master);

    printf("ttyprobe %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
