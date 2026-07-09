#include "errno.h"
#include "fcntl.h"
#include "stdio.h"
#include "string.h"
#include "sys/ioctl.h"
#include "sys/wait.h"
#include "termios.h"
#include "unistd.h"
#include "pty.h"
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

static int buffer_contains(const char* haystack, const char* needle) {
    return strstr(haystack, needle) != 0;
}

static int login_tty_exec_probe(void) {
    int master = -1;
    int slave = -1;
    int syncpipe[2] = { -1, -1 };
    int child;
    int status = -1;
    int total = 0;
    int loops;
    char sync = 0;
    char buf[128];
    char name[32];
    struct winsize ws;

    memset(&ws, 0, sizeof(ws));
    ws.ws_row = 24;
    ws.ws_col = 80;
    memset(buf, 0, sizeof(buf));
    memset(name, 0, sizeof(name));

    if (openpty(&master, &slave, name, 0, &ws) < 0) return 0;
    if (pipe(syncpipe) < 0) {
        close(slave);
        close(master);
        return 0;
    }

    child = fork();
    if (child == 0) {
        char* argv[] = { "shell", "-c", "echo pty-exec-ok", 0 };
        char* envp[] = { 0 };

        close(master);
        close(syncpipe[0]);
        if (login_tty(slave) < 0) _exit(100);
        if (write(syncpipe[1], "x", 1) != 1) _exit(101);
        close(syncpipe[1]);
        for (int fd = 3; fd < 64; fd++) close(fd);
        execve("/bin/shell", argv, envp);
        _exit(111);
    }

    close(slave);
    close(syncpipe[1]);
    if (child < 0) {
        close(syncpipe[0]);
        close(master);
        return 0;
    }

    if (read(syncpipe[0], &sync, 1) != 1 || sync != 'x') {
        close(syncpipe[0]);
        close(master);
        waitpid(child, &status, 0);
        return 0;
    }
    close(syncpipe[0]);

    fcntl(master, F_SETFL, O_NONBLOCK);
    for (loops = 0; loops < 200; loops++) {
        int n = (int)read(master, buf + total, (int)sizeof(buf) - 1 - total);
        if (n > 0) {
            total += n;
            buf[total] = '\0';
            if (total >= (int)sizeof(buf) - 1) break;
        } else if (n < 0 && errno != EAGAIN) {
            break;
        }

        if (waitpid(child, &status, WNOHANG) == child) {
            break;
        }
        usleep(10000);
    }

    while (total < (int)sizeof(buf) - 1) {
        int n = (int)read(master, buf + total, (int)sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
    }

    close(master);
    if (status < 0) waitpid(child, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
           buffer_contains(buf, "pty-exec-ok");
}

int main(void) {
    int fds[2];
    int master;
    int slave;
    int n;
    int pgid;
    int got_pgid = 0;
    int nullfd;
    int ofds[2] = { -1, -1 };
    int child;
    int status = -1;
    char buf[16];
    char name[32];
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
                                (tio.c_oflag & (OPOST | ONLCR)) ==
                                    (OPOST | ONLCR) &&
                                tio.c_cc[VINTR] == 3 &&
                                tio.c_cc[VEOF] == 4 &&
                                tio.c_cc[VMIN] == 1);

    check("write canonical", write(master, "a\n", 2) == 2);
    n = read_exactish(slave, buf, sizeof(buf) - 1);
    check("canonical read newline", n == 2 && strcmp(buf, "a\n") == 0);
    n = read_exactish(master, buf, sizeof(buf) - 1);
    check("echo default", n == 3 && strcmp(buf, "a\r\n") == 0);

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

    check("tcsetattr output post", tcgetattr(slave, &tio) == 0);
    tio.c_oflag |= OPOST | ONLCR;
    check("tcsetattr onlcr", tcsetattr(slave, TCSANOW, &tio) == 0);
    check("slave write newline", write(slave, "c\n", 2) == 2);
    n = read_exactish(master, buf, sizeof(buf) - 1);
    check("slave output onlcr", n == 3 && strcmp(buf, "c\r\n") == 0);
    check("slave write crlf", write(slave, "d\r\n", 3) == 3);
    n = read_exactish(master, buf, sizeof(buf) - 1);
    check("slave output crlf not doubled", n == 3 && strcmp(buf, "d\r\n") == 0);

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
    ws.ws_row = 44;
    ws.ws_col = 120;
    check("ioctl setwinsize fd", ioctl(slave, TIOCSWINSZ, &ws) == 0);
    memset(&ws, 0, sizeof(ws));
    check("ioctl setwinsize readback", ioctl(slave, TIOCGWINSZ, &ws) == 0 &&
                                       ws.ws_row == 44 && ws.ws_col == 120);

    memset(name, 0, sizeof(name));
    check("openpty wrapper", openpty(&ofds[0], &ofds[1], name, 0, &ws) == 0);
    if (ofds[0] >= 0 && ofds[1] >= 0) {
        check("openpty name", strncmp(name, "/dev/pts/", 9) == 0);
        check("ttyname slave", ttyname(ofds[1]) &&
                               strncmp(ttyname(ofds[1]), "/dev/pts/", 9) == 0);
        check("ptsname master", ptsname(ofds[0]) &&
                                strncmp(ptsname(ofds[0]), "/dev/pts/", 9) == 0);
        memset(name, 0, sizeof(name));
        check("ttyname_r slave", ttyname_r(ofds[1], name, sizeof(name)) == 0 &&
                                 strncmp(name, "/dev/pts/", 9) == 0);
        memset(name, 0, sizeof(name));
        check("ptsname_r master", ptsname_r(ofds[0], name, sizeof(name)) == 0 &&
                                  strncmp(name, "/dev/pts/", 9) == 0);
        check("grantpt wrapper", grantpt(ofds[0]) == 0);
        check("unlockpt wrapper", unlockpt(ofds[0]) == 0);
        child = fork();
        if (child == 0) {
            int rc = login_tty(ofds[1]);
            _exit(rc == 0 && isatty(0) && isatty(1) && isatty(2) ? 0 : 7);
        }
        check("fork login_tty child", child > 0);
        if (child > 0) {
            check("wait login_tty child", waitpid(child, &status, 0) == child &&
                                           WIFEXITED(status) &&
                                           WEXITSTATUS(status) == 0);
        }
        check("login_tty exec shell", login_tty_exec_probe());
        close(ofds[1]);
        close(ofds[0]);
    }

    pgid = getpgrp();
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
        check("ttyname_r enotty", ttyname_r(nullfd, name, sizeof(name)) == ENOTTY);
        check("isatty false", isatty(nullfd) == 0);
        close(nullfd);
    }

    close(slave);
    close(master);

    printf("ttyprobe %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
