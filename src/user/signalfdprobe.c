#include "user_lib.h"
#include "poll.h"
#include "signal.h"
#include "unistd.h"
#include "sys/signalfd.h"

static void fail(int fd, const char* msg) {
    u_puts("signalfdprobe FAIL: ");
    u_puts(msg);
    u_putc('\n');
    if (fd >= 0) {
        close(fd);
    }
    sys_exit(1);
}

void _start(int argc, char** argv) {
    sigset_t mask;
    struct pollfd pfd;
    struct signalfd_siginfo info;
    int fd;
    int rc;

    (void)argc;
    (void)argv;

    u_puts("signalfdprobe start\n");

    if (sigemptyset(&mask) < 0 || sigaddset(&mask, SIGINT) < 0) {
        fail(-1, "mask");
    }

    fd = signalfd(-1, &mask, 0);
    if (fd < 0) {
        fail(-1, "signalfd");
    }

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    u_puts("signalfdprobe waiting\n");
    if (poll(&pfd, 1, 10000) != 1 || (pfd.revents & POLLIN) == 0) {
        fail(fd, "poll");
    }

    rc = read(fd, &info, sizeof(info));
    if (rc != (int)sizeof(info) || info.ssi_signo != SIGINT) {
        fail(fd, "read");
    }

    close(fd);
    u_puts("signalfdprobe signal ");
    u_put_uint(info.ssi_signo);
    u_putc('\n');
    u_puts("signalfdprobe PASS\n");
    sys_exit(0);
}
