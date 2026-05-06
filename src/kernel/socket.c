#include "socket.h"

#include "klib.h"
#include "uapi_errno.h"
#include "uapi_poll.h"
#include "../drivers/tcp.h"

struct socket {
    socket_kind_t kind;
    socket_state_t state;
    unsigned int refs;
    unsigned int flags;
    unsigned int backlog;
    unsigned short local_port;
    unsigned int conn_id;
};

static socket_t s_sockets[SOCKET_MAX];

static void socket_tcp_use(socket_t* sock) {
    if (!sock || sock->kind != SOCKET_KIND_TCP) return;

    if (sock->state == SOCKET_STATE_CONNECTED) {
        tcp_socket_use_connection(sock->local_port, sock->conn_id);
    } else {
        tcp_socket_use_port(sock->local_port);
    }
}

socket_t* socket_create_tcp(void) {
    for (unsigned int i = 0; i < SOCKET_MAX; i++) {
        socket_t* sock = &s_sockets[i];
        if (sock->refs == 0u) {
            k_memset(sock, 0, sizeof(*sock));
            sock->kind = SOCKET_KIND_TCP;
            sock->state = SOCKET_STATE_OPEN;
            sock->refs = 1u;
            sock->conn_id = TCP_SOCKET_CONN_NONE;
            return sock;
        }
    }

    return 0;
}

void socket_retain(socket_t* sock) {
    if (!sock || sock->refs == 0u) return;
    sock->refs++;
}

void socket_release(socket_t* sock) {
    if (!sock || sock->refs == 0u) return;

    if (sock->refs > 1u) {
        sock->refs--;
        return;
    }

    socket_close(sock);
    k_memset(sock, 0, sizeof(*sock));
}

socket_kind_t socket_kind(socket_t* sock) {
    return sock ? sock->kind : SOCKET_KIND_NONE;
}

socket_state_t socket_state(socket_t* sock) {
    return sock ? sock->state : SOCKET_STATE_CLOSED;
}

unsigned int socket_local_port(socket_t* sock) {
    return sock ? sock->local_port : 0u;
}

unsigned int socket_conn_id(socket_t* sock) {
    return sock ? sock->conn_id : TCP_SOCKET_CONN_NONE;
}

unsigned int socket_peer_ip(socket_t* sock) {
    if (!sock || sock->state != SOCKET_STATE_CONNECTED) return 0u;
    socket_tcp_use(sock);
    return tcp_socket_peer_ip();
}

unsigned int socket_peer_port(socket_t* sock) {
    if (!sock || sock->state != SOCKET_STATE_CONNECTED) return 0u;
    socket_tcp_use(sock);
    return tcp_socket_peer_port();
}

int socket_bind_tcp(socket_t* sock, unsigned int port) {
    if (!sock || sock->kind != SOCKET_KIND_TCP) return -EINVAL;
    if (sock->state != SOCKET_STATE_OPEN) return -EINVAL;
    if (port == 0u || port > 0xFFFFu) return -EINVAL;

    sock->local_port = (unsigned short)port;
    sock->state = SOCKET_STATE_BOUND;
    return 0;
}

int socket_listen_tcp(socket_t* sock, int backlog) {
    int rc;
    unsigned int effective_backlog;

    if (!sock || sock->kind != SOCKET_KIND_TCP) return -EINVAL;
    if (sock->state != SOCKET_STATE_BOUND) return -EINVAL;
    if (sock->local_port == 0u) return -EINVAL;

    effective_backlog = backlog <= 0 ? 1u : (unsigned int)backlog;
    if (effective_backlog > SOCKET_BACKLOG_MAX) {
        effective_backlog = SOCKET_BACKLOG_MAX;
    }

    tcp_socket_use_port(sock->local_port);
    if (tcp_socket_bind(sock->local_port) < 0) return -EACCES;
    rc = tcp_socket_listen(effective_backlog);
    if (rc < 0) return -EIO;

    sock->backlog = effective_backlog;
    sock->state = SOCKET_STATE_LISTENING;
    return 0;
}

int socket_accept_ready(socket_t* sock) {
    if (!sock || sock->state != SOCKET_STATE_LISTENING) return 0;
    socket_tcp_use(sock);
    return tcp_socket_accept_ready();
}

int socket_accept_tcp(socket_t* listener, socket_t* child) {
    unsigned int conn_id;

    if (!listener || !child) return -EINVAL;
    if (listener->kind != SOCKET_KIND_TCP ||
        child->kind != SOCKET_KIND_TCP) {
        return -EINVAL;
    }
    if (listener->state != SOCKET_STATE_LISTENING ||
        child->state != SOCKET_STATE_OPEN) {
        return -EINVAL;
    }

    socket_tcp_use(listener);
    conn_id = tcp_socket_mark_accepted();
    if (conn_id == TCP_SOCKET_CONN_NONE) return -EAGAIN;

    child->local_port = listener->local_port;
    child->conn_id = conn_id;
    child->state = SOCKET_STATE_CONNECTED;
    child->backlog = 0u;
    socket_tcp_use(child);
    return 0;
}

int socket_tcp_connection_established(socket_t* sock) {
    if (!sock || sock->state != SOCKET_STATE_CONNECTED) return 0;
    socket_tcp_use(sock);
    return tcp_socket_connection_established();
}

int socket_tcp_recv_ready(socket_t* sock) {
    if (!sock || sock->state != SOCKET_STATE_CONNECTED) return 0;
    socket_tcp_use(sock);
    return tcp_socket_recv_ready();
}

int socket_tcp_peer_closed(socket_t* sock) {
    if (!sock || sock->state != SOCKET_STATE_CONNECTED) return 0;
    socket_tcp_use(sock);
    return tcp_socket_peer_closed();
}

int socket_tcp_recv(socket_t* sock, void* buf, unsigned int len) {
    if (!sock || sock->state != SOCKET_STATE_CONNECTED) return -EINVAL;
    socket_tcp_use(sock);
    return tcp_socket_recv(buf, len);
}

int socket_tcp_send(socket_t* sock, const void* buf, unsigned int len) {
    if (!sock || sock->state != SOCKET_STATE_CONNECTED) return -EINVAL;
    socket_tcp_use(sock);
    return tcp_socket_send(buf, len);
}

short socket_poll(socket_t* sock, short events) {
    short revents = 0;

    if (!sock || sock->kind != SOCKET_KIND_TCP) return POLLERR;

    if (sock->state == SOCKET_STATE_LISTENING) {
        socket_tcp_use(sock);
        if ((events & POLLIN) && tcp_socket_accept_ready()) {
            revents |= POLLIN;
        }
    } else if (sock->state == SOCKET_STATE_CONNECTED) {
        socket_tcp_use(sock);
        if ((events & POLLIN) && tcp_socket_recv_ready()) {
            revents |= POLLIN;
        }
        if (tcp_socket_peer_closed()) {
            revents |= POLLHUP;
        }
        if ((events & POLLOUT) && tcp_socket_connection_established()) {
            revents |= POLLOUT;
        }
    }

    return revents;
}

void socket_close(socket_t* sock) {
    if (!sock || sock->refs == 0u || sock->kind != SOCKET_KIND_TCP) return;

    if (sock->state == SOCKET_STATE_LISTENING) {
        tcp_socket_close_listener(sock->local_port);
    } else if (sock->state == SOCKET_STATE_CONNECTED) {
        tcp_socket_close_connection(sock->local_port, sock->conn_id);
    }

    sock->state = SOCKET_STATE_CLOSED;
    sock->local_port = 0u;
    sock->conn_id = TCP_SOCKET_CONN_NONE;
    sock->backlog = 0u;
}
