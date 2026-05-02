#include "user_lib.h"
#include "dirent.h"
#include "poll.h"
#include "unistd.h"
#include "sys/socket.h"
#include "arpa/inet.h"

#include "../../third_party/ftp_server/include/ftp_server.h"

#define FTPD_PORT 2121u
#define FTPD_PASV_PORT 30000u

static int start_listener(void) {
    int fd;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(FTPD_PORT);
    addr.sin_addr.s_addr = htonl(0);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void init_config(ftp_config_t* config) {
    memset(config, 0, sizeof(*config));
    ftp_strlcpy(config->bind_addr, "0.0.0.0", sizeof(config->bind_addr));
    config->port = FTPD_PORT;
    ftp_strlcpy(config->root, "/", sizeof(config->root));
    config->ctrl_timeout_ms = CTRL_IDLE_MS;
    config->pasv_port_min = FTPD_PASV_PORT;
    config->pasv_port_max = FTPD_PASV_PORT;
    config->max_sessions = 1;
    config->user_count = 1;
    ftp_strlcpy(config->users[0].username, "ftp",
                sizeof(config->users[0].username));
    ftp_strlcpy(config->users[0].password_hash, "ftp",
                sizeof(config->users[0].password_hash));
    ftp_strlcpy(config->users[0].home, "/",
                sizeof(config->users[0].home));
    config->users[0].perms = FTP_PERM_READ | FTP_PERM_WRITE |
                             FTP_PERM_DELETE | FTP_PERM_MKDIR;
}

void _start(int argc, char** argv) {
    ftp_config_t config;
    int listen_fd;

    (void)argc;
    (void)argv;

    init_config(&config);
    listen_fd = start_listener();
    if (listen_fd < 0) {
        u_puts("ftpd: listen setup failed\n");
        sys_exit(1);
    }

    u_puts("ftpd: listening on 0.0.0.0:");
    u_put_uint(FTPD_PORT);
    u_puts("\n");

    for (;;) {
        int client_fd;

        client_fd = accept(listen_fd, 0, 0);
        if (client_fd < 0) {
            u_puts("ftpd: accept failed\n");
            break;
        }

        (void)ftp_session_serve(client_fd, &config);
    }

    close(listen_fd);
    sys_exit(1);
}
