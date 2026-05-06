#include "user_lib.h"
#include "poll.h"
#include "sys/epoll.h"
#include "sys/timerfd.h"

static void fail(const char* msg) {
    u_puts("timerfdprobe FAIL: ");
    u_puts(msg);
    u_putc('\n');
    sys_exit(1);
}

static void arm_ticks(int fd, unsigned int ticks) {
    struct itimerspec spec;
    unsigned int ns_per_tick = SMALLOS_NS_PER_SECOND / SMALLOS_TIMER_HZ;

    memset(&spec, 0, sizeof(spec));
    spec.it_value.tv_nsec = (long)(ticks * ns_per_tick);
    if (sys_timerfd_settime(fd, 0, &spec, 0) < 0) {
        fail("settime");
    }
}

static void consume_timer(int fd) {
    unsigned long long expirations = 0;
    int rc = sys_fread(fd, (char*)&expirations, sizeof(expirations));

    if (rc != (int)sizeof(expirations) || expirations == 0ull) {
        fail("read expiration");
    }
}

void _start(int argc, char** argv) {
    struct pollfd pfd;
    struct epoll_event ev;
    struct epoll_event out;
    int fd;
    int epfd;

    (void)argc;
    (void)argv;

    u_puts("timerfdprobe start\n");

    fd = sys_timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd < 0) {
        fail("create poll timer");
    }

    arm_ticks(fd, 3);
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (sys_poll(&pfd, 1u, -1) != 1 || (pfd.revents & POLLIN) == 0) {
        fail("poll wait");
    }
    consume_timer(fd);
    sys_close(fd);
    u_puts("timerfdprobe poll ok\n");

    fd = sys_timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd < 0) {
        fail("create epoll timer");
    }
    epfd = sys_epoll_create(0);
    if (epfd < 0) {
        fail("epoll create");
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u32 = 0x54494D45u;
    if (sys_epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        fail("epoll add");
    }

    arm_ticks(fd, 3);
    memset(&out, 0, sizeof(out));
    if (sys_epoll_wait(epfd, &out, 1, -1) != 1 ||
        (out.events & EPOLLIN) == 0 ||
        out.data.u32 != 0x54494D45u) {
        fail("epoll wait");
    }
    consume_timer(fd);
    sys_close(epfd);
    sys_close(fd);
    u_puts("timerfdprobe epoll ok\n");

    u_puts("timerfdprobe PASS\n");
    sys_exit(0);
}
