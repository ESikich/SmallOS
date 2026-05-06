#include "user_lib.h"
#include "unistd.h"
#include "poll.h"
#include "sys/socket.h"
#include "arpa/inet.h"

#define TCP_ECHO_PORT 2323
#define TCP_ECHO_BUF 128
#define TCP_ECHO_MAX_CLIENTS 32

static void write_banner(void) {
    u_puts("tcpecho: listening on 0.0.0.0:");
    u_put_uint(TCP_ECHO_PORT);
    u_puts("\n");
}

void _start(int argc, char** argv) {
    int server_fd;
    struct sockaddr_in addr;
    int clients[TCP_ECHO_MAX_CLIENTS];

    (void)argc;
    (void)argv;

    for (unsigned int i = 0; i < TCP_ECHO_MAX_CLIENTS; i++) {
        clients[i] = -1;
    }

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
    if (listen(server_fd, TCP_ECHO_MAX_CLIENTS) < 0) {
        u_puts("tcpecho: listen failed\n");
        sys_exit(1);
    }

    write_banner();

    for (;;) {
        struct pollfd fds[TCP_ECHO_MAX_CLIENTS + 1];
        int client_index[TCP_ECHO_MAX_CLIENTS + 1];
        nfds_t nfds = 1;
        int ready;
        char buf[TCP_ECHO_BUF];

        fds[0].fd = server_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        client_index[0] = -1;

        for (unsigned int i = 0; i < TCP_ECHO_MAX_CLIENTS; i++) {
            if (clients[i] < 0) {
                continue;
            }
            fds[nfds].fd = clients[i];
            fds[nfds].events = POLLIN | POLLHUP;
            fds[nfds].revents = 0;
            client_index[nfds] = (int)i;
            nfds++;
        }

        ready = poll(fds, nfds, -1);
        if (ready < 0) {
            u_puts("tcpecho: poll failed\n");
            break;
        }

        if (fds[0].revents & POLLIN) {
            int slot = -1;
            int client_fd;

            for (unsigned int i = 0; i < TCP_ECHO_MAX_CLIENTS; i++) {
                if (clients[i] < 0) {
                    slot = (int)i;
                    break;
                }
            }

            client_fd = accept(server_fd, 0, 0);
            if (client_fd < 0) {
                u_puts("tcpecho: accept failed\n");
            } else if (slot < 0) {
                close(client_fd);
                u_puts("tcpecho: client dropped\n");
            } else {
                clients[slot] = client_fd;
                u_puts("tcpecho: fd=");
                u_put_uint((unsigned int)client_fd);
                u_puts("\n");
                u_puts("tcpecho: client connected\n");
            }
        }

        for (nfds_t i = 1; i < nfds; i++) {
            int slot = client_index[i];
            int client_fd;
            int n;
            unsigned int done;

            if (slot < 0 || clients[slot] < 0 || fds[i].revents == 0) {
                continue;
            }

            client_fd = clients[slot];
            n = read(client_fd, buf, sizeof(buf));
            if (n <= 0) {
                close(client_fd);
                clients[slot] = -1;
                u_puts("tcpecho: client disconnected\n");
                continue;
            }

            done = 0;
            while (done < (unsigned int)n) {
                int written = write(client_fd, buf + done, (unsigned int)n - done);
                if (written <= 0) {
                    close(client_fd);
                    clients[slot] = -1;
                    u_puts("tcpecho: write failed\n");
                    break;
                }
                done += (unsigned int)written;
            }
        }
    }

    for (unsigned int i = 0; i < TCP_ECHO_MAX_CLIENTS; i++) {
        if (clients[i] >= 0) {
            close(clients[i]);
        }
    }
    close(server_fd);
    sys_exit(0);
}
