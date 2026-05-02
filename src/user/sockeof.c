#include "user_lib.h"
#include "unistd.h"
#include "poll.h"
#include "sys/socket.h"
#include "arpa/inet.h"

#define SOCKEOF_PORT 2463

static void fail(int fd, const char* msg) {
    u_puts("sockeof: ");
    u_puts(msg);
    u_puts("\n");
    if (fd >= 0) {
        close(fd);
    }
    sys_exit(1);
}

void _start(int argc, char** argv) {
    int server_fd;
    int client_fd;
    struct sockaddr_in addr;
    char buf[32];
    struct pollfd pfd;
    int n;

    (void)argc;
    (void)argv;

    server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd < 0) {
        fail(-1, "socket failed");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SOCKEOF_PORT);
    addr.sin_addr.s_addr = htonl(0);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fail(server_fd, "bind failed");
    }
    if (listen(server_fd, 1) < 0) {
        fail(server_fd, "listen failed");
    }

    u_puts("sockeof: listening on 0.0.0.0:");
    u_put_uint(SOCKEOF_PORT);
    u_puts("\n");

    client_fd = accept(server_fd, 0, 0);
    if (client_fd < 0) {
        fail(server_fd, "accept failed");
    }

    pfd.fd = client_fd;
    pfd.events = POLLIN | POLLHUP;
    pfd.revents = 0;
    if (poll(&pfd, 1, 5000) != 1 || (pfd.revents & POLLIN) == 0) {
        close(client_fd);
        fail(server_fd, "payload poll failed");
    }

    n = read(client_fd, buf, sizeof(buf));
    if (n != 7 || memcmp(buf, "payload", 7) != 0) {
        close(client_fd);
        fail(server_fd, "payload read failed");
    }

    pfd.revents = 0;
    if (poll(&pfd, 1, 5000) != 1 || (pfd.revents & POLLHUP) == 0) {
        close(client_fd);
        fail(server_fd, "eof poll failed");
    }

    n = read(client_fd, buf, sizeof(buf));
    if (n != 0) {
        close(client_fd);
        fail(server_fd, "eof read failed");
    }

    if (write(client_fd, "PASS\n", 5) != 5) {
        close(client_fd);
        fail(server_fd, "response write failed");
    }

    close(client_fd);
    close(server_fd);
    u_puts("sockeof PASS\n");
    sys_exit(0);
}
