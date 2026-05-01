#include "user_lib.h"
#include "unistd.h"
#include "sys/socket.h"
#include "arpa/inet.h"

#define TCP_ECHO_PORT 2323
#define TCP_ECHO_BUF   512

static void write_banner(void) {
    u_puts("tcpecho: listening on 0.0.0.0:");
    u_put_uint(TCP_ECHO_PORT);
    u_puts("\n");
}

void _start(int argc, char** argv) {
    int server_fd;
    struct sockaddr_in addr;

    (void)argc;
    (void)argv;

    server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd < 0) {
        u_puts("tcpecho: socket failed\n");
        sys_exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TCP_ECHO_PORT);
    addr.sin_addr.s_addr = htonl(0);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        u_puts("tcpecho: bind failed\n");
        sys_exit(1);
    }
    if (listen(server_fd, 1) < 0) {
        u_puts("tcpecho: listen failed\n");
        sys_exit(1);
    }

    write_banner();

    for (;;) {
        int client_fd;
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        char buf[TCP_ECHO_BUF];

        memset(&peer, 0, sizeof(peer));
        client_fd = accept(server_fd, (struct sockaddr*)&peer, &peer_len);
        if (client_fd < 0) {
            u_puts("tcpecho: accept failed\n");
            break;
        }

        u_puts("tcpecho: client connected\n");

        for (;;) {
            int n = recv(client_fd, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            if (send(client_fd, buf, (unsigned int)n) < 0) {
                break;
            }
        }

        close(client_fd);
        u_puts("tcpecho: client disconnected\n");
    }

    close(server_fd);
    sys_exit(0);
}
