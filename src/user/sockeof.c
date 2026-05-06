#include "user_lib.h"
#include "unistd.h"
#include "poll.h"
#include "sys/socket.h"
#include "arpa/inet.h"

#define SOCKEOF_PORT 2463
#define SOCKEOF_PAYLOAD_LEN 3072

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
    char buf[128];
    struct pollfd pfd;
    unsigned int total;
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

    total = 0;
    while (total < SOCKEOF_PAYLOAD_LEN) {
        unsigned int remaining = SOCKEOF_PAYLOAD_LEN - total;
        unsigned int want = remaining < sizeof(buf) ? remaining : sizeof(buf);

        n = read(client_fd, buf, want);
        if (n <= 0) {
            close(client_fd);
            fail(server_fd, "payload read failed");
        }
        for (int i = 0; i < n; i++) {
            char expected = (char)('A' + ((total + (unsigned int)i) % 26u));
            if (buf[i] != expected) {
                close(client_fd);
                fail(server_fd, "payload mismatch");
            }
        }
        total += (unsigned int)n;
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
    if (shutdown(client_fd, SHUT_WR) < 0) {
        close(client_fd);
        fail(server_fd, "shutdown write failed");
    }
    if (write(client_fd, "NOPE", 4) >= 0) {
        close(client_fd);
        fail(server_fd, "post-shutdown write succeeded");
    }

    close(client_fd);
    close(server_fd);
    u_puts("sockeof PASS\n");
    sys_exit(0);
}
