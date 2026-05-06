#include "user_lib.h"
#include "errno.h"
#include "poll.h"
#include "string.h"
#include "unistd.h"
#include "sys/socket.h"
#include "arpa/inet.h"

#define CONNECTPROBE_PORT 45123
#define CONNECTPROBE_HOST "10.0.2.2"

static void fail(int fd, const char* msg) {
    u_puts("connectprobe FAIL: ");
    u_puts(msg);
    u_puts(" errno=");
    u_put_uint((unsigned int)errno);
    u_putc('\n');
    if (fd >= 0) {
        close(fd);
    }
    sys_exit(1);
}

static void wait_event(int fd, short events, const char* what) {
    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    if (poll(&pfd, 1, 8000) != 1 || (pfd.revents & events) == 0) {
        fail(fd, what);
    }
}

void _start(int argc, char** argv) {
    struct sockaddr_in addr;
    struct sockaddr_in peer;
    socklen_t peer_len;
    const char request[] = "connectprobe hello";
    const char expected[] = "connectprobe hello";
    char buf[64];
    int fd;
    int rc;

    (void)argc;
    (void)argv;

    u_puts("connectprobe start\n");

    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
    if (fd < 0) {
        fail(-1, "socket");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONNECTPROBE_PORT);
    addr.sin_addr.s_addr = inet_addr(CONNECTPROBE_HOST);

    rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS && errno != EALREADY) {
        fail(fd, "connect");
    }

    wait_event(fd, POLLOUT, "connect poll");
    u_puts("connectprobe connected\n");

    peer_len = sizeof(peer);
    memset(&peer, 0, sizeof(peer));
    if (getpeername(fd, (struct sockaddr*)&peer, &peer_len) < 0 ||
        peer.sin_port != htons(CONNECTPROBE_PORT) ||
        peer.sin_addr.s_addr != inet_addr(CONNECTPROBE_HOST)) {
        fail(fd, "getpeername");
    }

    rc = send(fd, request, sizeof(request) - 1u, 0);
    if (rc != (int)(sizeof(request) - 1u)) {
        fail(fd, "send");
    }

    wait_event(fd, POLLIN, "reply poll");
    memset(buf, 0, sizeof(buf));
    rc = recv(fd, buf, sizeof(buf) - 1u, 0);
    if (rc != (int)(sizeof(expected) - 1u) ||
        memcmp(buf, expected, sizeof(expected) - 1u) != 0) {
        fail(fd, "recv");
    }

    close(fd);
    u_puts("connectprobe PASS\n");
    sys_exit(0);
}
