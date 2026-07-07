#include "socket.h"

#include "klib.h"
#include "uapi_errno.h"
#include "uapi_poll.h"
#include "uapi_socket.h"
#include "wait.h"
#include "../drivers/net.h"
#include "../drivers/tcp.h"

#define SOCKET_FLAG_READ_SHUTDOWN  0x00000001u
#define SOCKET_FLAG_WRITE_SHUTDOWN 0x00000002u
#define SOCKET_RAW_ICMP_MAX_PACKET 1600u
#define SOCKET_UDP_MAX_PACKET      576u
#define SOCKET_UDP_QUEUE_MAX       8u
#define SOCKET_UDP_EPHEMERAL_FIRST 49152u
#define SOCKET_UDP_EPHEMERAL_LAST  65535u

struct socket {
    socket_kind_t kind;
    socket_state_t state;
    unsigned int refs;
    unsigned int flags;
    unsigned int backlog;
    unsigned short local_port;
    unsigned int remote_ip;
    unsigned short remote_port;
    unsigned int conn_id;
    wait_queue_t read_waiters;
    wait_queue_t write_waiters;
    wait_queue_t accept_waiters;
};

typedef struct udp_packet {
    unsigned int used;
    unsigned int src_ip;
    unsigned int dst_ip;
    unsigned short src_port;
    unsigned short dst_port;
    unsigned int len;
    unsigned char payload[SOCKET_UDP_MAX_PACKET];
} udp_packet_t;

static socket_t s_sockets[SOCKET_MAX];
static unsigned char s_raw_icmp_packet[SOCKET_RAW_ICMP_MAX_PACKET];
static unsigned int s_raw_icmp_len;
static unsigned int s_raw_icmp_src_ip;
static unsigned int s_raw_icmp_valid;
static udp_packet_t s_udp_packets[SOCKET_UDP_QUEUE_MAX];
static unsigned int s_next_udp_ephemeral = SOCKET_UDP_EPHEMERAL_FIRST;

static void socket_tcp_use(socket_t* sock) {
    if (!sock || sock->kind != SOCKET_KIND_TCP) return;

    if (sock->state == SOCKET_STATE_CONNECTED ||
        sock->state == SOCKET_STATE_CONNECTING) {
        tcp_socket_use_connection(sock->local_port, sock->conn_id);
    } else {
        tcp_socket_use_port(sock->local_port);
    }
}

socket_t* socket_create_kind(socket_kind_t kind) {
    if (kind != SOCKET_KIND_TCP &&
        kind != SOCKET_KIND_UDP &&
        kind != SOCKET_KIND_RAW_ICMP) {
        return 0;
    }
    for (unsigned int i = 0; i < SOCKET_MAX; i++) {
        socket_t* sock = &s_sockets[i];
        if (sock->refs == 0u) {
            k_memset(sock, 0, sizeof(*sock));
            sock->kind = kind;
            sock->state = SOCKET_STATE_OPEN;
            sock->refs = 1u;
            sock->conn_id = TCP_SOCKET_CONN_NONE;
            wait_queue_init(&sock->read_waiters);
            wait_queue_init(&sock->write_waiters);
            wait_queue_init(&sock->accept_waiters);
            return sock;
        }
    }

    return 0;
}

socket_t* socket_create_tcp(void) {
    return socket_create_kind(SOCKET_KIND_TCP);
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

void socket_get_stats(socket_stats_t* out) {
    if (!out) return;

    k_memset(out, 0, sizeof(*out));
    out->max_sockets = SOCKET_MAX;

    for (unsigned int i = 0; i < SOCKET_MAX; i++) {
        socket_t* sock = &s_sockets[i];
        if (sock->refs == 0u) continue;

        out->used_sockets++;
        if (sock->kind == SOCKET_KIND_TCP) {
            out->tcp_sockets++;
        }

        switch (sock->state) {
        case SOCKET_STATE_OPEN:
            out->open_sockets++;
            break;
        case SOCKET_STATE_BOUND:
            out->bound_sockets++;
            break;
        case SOCKET_STATE_LISTENING:
            out->listening_sockets++;
            break;
        case SOCKET_STATE_CONNECTING:
            out->connected_sockets++;
            break;
        case SOCKET_STATE_CONNECTED:
            out->connected_sockets++;
            break;
        case SOCKET_STATE_CLOSED:
        default:
            break;
        }
    }
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

unsigned int socket_local_ip(socket_t* sock) {
    if (!sock) return 0u;
    if (sock->kind == SOCKET_KIND_UDP) return net_ipv4_local_ip();
    if (sock->kind != SOCKET_KIND_TCP) return 0u;
    socket_tcp_use(sock);
    return tcp_socket_local_ip();
}

unsigned int socket_conn_id(socket_t* sock) {
    return sock ? sock->conn_id : TCP_SOCKET_CONN_NONE;
}

unsigned int socket_peer_ip(socket_t* sock) {
    if (!sock ||
        (sock->state != SOCKET_STATE_CONNECTED &&
         sock->state != SOCKET_STATE_CONNECTING)) return 0u;
    if (sock->kind == SOCKET_KIND_UDP) return sock->remote_ip;
    if (sock->kind != SOCKET_KIND_TCP) return 0u;
    socket_tcp_use(sock);
    return tcp_socket_peer_ip();
}

unsigned int socket_peer_port(socket_t* sock) {
    if (!sock ||
        (sock->state != SOCKET_STATE_CONNECTED &&
         sock->state != SOCKET_STATE_CONNECTING)) return 0u;
    if (sock->kind == SOCKET_KIND_UDP) return sock->remote_port;
    if (sock->kind != SOCKET_KIND_TCP) return 0u;
    socket_tcp_use(sock);
    return tcp_socket_peer_port();
}

static int socket_udp_port_in_use(unsigned int port, socket_t* except) {
    if (port == 0u || port > 0xFFFFu) return 1;
    for (unsigned int i = 0; i < SOCKET_MAX; i++) {
        socket_t* sock = &s_sockets[i];
        if (sock == except || sock->refs == 0u || sock->kind != SOCKET_KIND_UDP) {
            continue;
        }
        if (sock->local_port == (unsigned short)port &&
            (sock->state == SOCKET_STATE_BOUND ||
             sock->state == SOCKET_STATE_CONNECTED)) {
            return 1;
        }
    }
    return 0;
}

static unsigned int socket_udp_alloc_port(socket_t* sock) {
    unsigned int start = s_next_udp_ephemeral;
    unsigned int port = start;

    for (;;) {
        if (!socket_udp_port_in_use(port, sock)) {
            s_next_udp_ephemeral = port + 1u;
            if (s_next_udp_ephemeral > SOCKET_UDP_EPHEMERAL_LAST) {
                s_next_udp_ephemeral = SOCKET_UDP_EPHEMERAL_FIRST;
            }
            return port;
        }
        port++;
        if (port > SOCKET_UDP_EPHEMERAL_LAST) port = SOCKET_UDP_EPHEMERAL_FIRST;
        if (port == start) return 0u;
    }
}

int socket_bind_udp(socket_t* sock, unsigned int port) {
    if (!sock || sock->kind != SOCKET_KIND_UDP) return -EINVAL;
    if (sock->state != SOCKET_STATE_OPEN) return -EINVAL;
    if (port == 0u) {
        port = socket_udp_alloc_port(sock);
        if (port == 0u) return -EADDRINUSE;
    }
    if (port > 0xFFFFu) return -EINVAL;
    if (socket_udp_port_in_use(port, sock)) return -EADDRINUSE;

    sock->local_port = (unsigned short)port;
    sock->state = SOCKET_STATE_BOUND;
    return 0;
}

int socket_udp_ensure_bound(socket_t* sock) {
    if (!sock || sock->kind != SOCKET_KIND_UDP) return -EINVAL;
    if (sock->state == SOCKET_STATE_BOUND || sock->state == SOCKET_STATE_CONNECTED) {
        if (sock->local_port != 0u) return 0;
    }
    if (sock->state != SOCKET_STATE_OPEN) return -EINVAL;
    return socket_bind_udp(sock, 0u);
}

int socket_connect_udp(socket_t* sock,
                       unsigned int remote_ip,
                       unsigned int remote_port) {
    int rc;

    if (!sock || sock->kind != SOCKET_KIND_UDP) return -EINVAL;
    if (remote_ip == 0u || remote_port == 0u || remote_port > 0xFFFFu) return -EINVAL;
    if (sock->state != SOCKET_STATE_OPEN &&
        sock->state != SOCKET_STATE_BOUND &&
        sock->state != SOCKET_STATE_CONNECTED) {
        return -EINVAL;
    }
    rc = socket_udp_ensure_bound(sock);
    if (rc < 0) return rc;
    sock->remote_ip = remote_ip;
    sock->remote_port = (unsigned short)remote_port;
    sock->state = SOCKET_STATE_CONNECTED;
    return 0;
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

int socket_connect_tcp(socket_t* sock,
                       unsigned int remote_ip,
                       unsigned int remote_port) {
    unsigned int local_port;
    unsigned int conn_id;
    int rc;

    if (!sock || sock->kind != SOCKET_KIND_TCP) return -EINVAL;
    if (sock->state == SOCKET_STATE_CONNECTED) return -EISCONN;
    if (sock->state == SOCKET_STATE_CONNECTING) {
        socket_tcp_use(sock);
        if (tcp_socket_connection_established()) {
            sock->state = SOCKET_STATE_CONNECTED;
            return -EISCONN;
        }
        if (tcp_socket_connect_pending()) return -EALREADY;
        sock->state = SOCKET_STATE_CLOSED;
        return -ECONNRESET;
    }
    if (sock->state != SOCKET_STATE_OPEN && sock->state != SOCKET_STATE_BOUND) {
        return -EINVAL;
    }

    rc = tcp_socket_connect(sock->local_port,
                            remote_ip,
                            remote_port,
                            &local_port,
                            &conn_id);
    if (rc < 0) return rc;

    sock->local_port = (unsigned short)local_port;
    sock->conn_id = conn_id;
    sock->state = SOCKET_STATE_CONNECTING;
    socket_tcp_use(sock);
    if (tcp_socket_connection_established()) {
        sock->state = SOCKET_STATE_CONNECTED;
    }
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
    if (!sock ||
        (sock->state != SOCKET_STATE_CONNECTED &&
         sock->state != SOCKET_STATE_CONNECTING)) {
        return 0;
    }
    socket_tcp_use(sock);
    if (tcp_socket_connection_established()) {
        if (sock->state == SOCKET_STATE_CONNECTING) {
            sock->state = SOCKET_STATE_CONNECTED;
        }
        return 1;
    }
    return 0;
}

int socket_tcp_connect_pending(socket_t* sock) {
    if (!sock || sock->state != SOCKET_STATE_CONNECTING) return 0;
    socket_tcp_use(sock);
    if (tcp_socket_connection_established()) {
        sock->state = SOCKET_STATE_CONNECTED;
        return 0;
    }
    return tcp_socket_connect_pending();
}

int socket_tcp_recv_ready(socket_t* sock) {
    if (!sock ||
        (sock->state != SOCKET_STATE_CONNECTED &&
         sock->state != SOCKET_STATE_CONNECTING)) return 0;
    if ((sock->flags & SOCKET_FLAG_READ_SHUTDOWN) != 0u) return 1;
    net_poll_drain();
    socket_tcp_use(sock);
    return tcp_socket_recv_ready();
}

int socket_tcp_peer_closed(socket_t* sock) {
    if (!sock ||
        (sock->state != SOCKET_STATE_CONNECTED &&
         sock->state != SOCKET_STATE_CONNECTING)) return 0;
    socket_tcp_use(sock);
    return tcp_socket_peer_closed();
}

int socket_tcp_recv(socket_t* sock, void* buf, unsigned int len) {
    if (!sock ||
        (sock->state != SOCKET_STATE_CONNECTED &&
         sock->state != SOCKET_STATE_CONNECTING)) return -EINVAL;
    if (!socket_tcp_connection_established(sock)) return -EAGAIN;
    if ((sock->flags & SOCKET_FLAG_READ_SHUTDOWN) != 0u) return 0;
    socket_tcp_use(sock);
    return tcp_socket_recv(buf, len);
}

int socket_tcp_send_ready(socket_t* sock) {
    if (!sock ||
        (sock->state != SOCKET_STATE_CONNECTED &&
         sock->state != SOCKET_STATE_CONNECTING)) return 0;
    if ((sock->flags & SOCKET_FLAG_WRITE_SHUTDOWN) != 0u) return 0;
    socket_tcp_use(sock);
    return tcp_socket_send_ready();
}

int socket_tcp_send(socket_t* sock, const void* buf, unsigned int len) {
    if (!sock ||
        (sock->state != SOCKET_STATE_CONNECTED &&
         sock->state != SOCKET_STATE_CONNECTING)) return -EINVAL;
    if (!socket_tcp_connection_established(sock)) return -EAGAIN;
    if ((sock->flags & SOCKET_FLAG_WRITE_SHUTDOWN) != 0u) return -EPIPE;
    socket_tcp_use(sock);
    return tcp_socket_send(buf, len);
}

int socket_shutdown_tcp(socket_t* sock, int how) {
    int rc = 0;

    if (!sock || sock->kind != SOCKET_KIND_TCP) return -EINVAL;
    if (sock->state != SOCKET_STATE_CONNECTED) return -EINVAL;
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) return -EINVAL;

    socket_tcp_use(sock);
    if (how == SHUT_RD || how == SHUT_RDWR) {
        rc = tcp_socket_shutdown(SHUT_RD);
        if (rc < 0) return rc;
        sock->flags |= SOCKET_FLAG_READ_SHUTDOWN;
        wait_queue_wake_all(&sock->read_waiters);
    }

    if (rc >= 0 && (how == SHUT_WR || how == SHUT_RDWR)) {
        rc = tcp_socket_shutdown(SHUT_WR);
        if (rc < 0) return rc;
        sock->flags |= SOCKET_FLAG_WRITE_SHUTDOWN;
        wait_queue_wake_all(&sock->write_waiters);
    }

    return rc;
}

int socket_raw_icmp_recv_ready(socket_t* sock) {
    if (!sock || sock->kind != SOCKET_KIND_RAW_ICMP) return 0;
    net_poll_drain();
    return s_raw_icmp_valid != 0u;
}

int socket_raw_icmp_recv(socket_t* sock,
                         void* buf,
                         unsigned int len,
                         unsigned int* out_src_ip) {
    unsigned int copy_len;

    if (!sock || sock->kind != SOCKET_KIND_RAW_ICMP || !buf) return -EINVAL;
    if (!socket_raw_icmp_recv_ready(sock)) return -EAGAIN;
    copy_len = s_raw_icmp_len;
    if (copy_len > len) copy_len = len;
    k_memcpy(buf, s_raw_icmp_packet, copy_len);
    if (out_src_ip) *out_src_ip = s_raw_icmp_src_ip;
    s_raw_icmp_valid = 0u;
    s_raw_icmp_len = 0u;
    return (int)copy_len;
}

int socket_raw_icmp_deliver(const void* packet,
                            unsigned int len,
                            unsigned int src_ip) {
    unsigned int copy_len;

    if (!packet || len == 0u) return 0;
    copy_len = len;
    if (copy_len > sizeof(s_raw_icmp_packet)) copy_len = sizeof(s_raw_icmp_packet);
    k_memcpy(s_raw_icmp_packet, packet, copy_len);
    s_raw_icmp_len = copy_len;
    s_raw_icmp_src_ip = src_ip;
    s_raw_icmp_valid = 1u;
    for (unsigned int i = 0; i < SOCKET_MAX; i++) {
        socket_t* sock = &s_sockets[i];
        if (sock->refs != 0u && sock->kind == SOCKET_KIND_RAW_ICMP) {
            wait_queue_wake_all(&sock->read_waiters);
        }
    }
    return 1;
}

static int socket_udp_packet_matches(socket_t* sock, udp_packet_t* packet) {
    if (!sock || !packet || !packet->used || sock->kind != SOCKET_KIND_UDP) return 0;
    if (sock->local_port == 0u || packet->dst_port != sock->local_port) return 0;
    if (sock->state == SOCKET_STATE_CONNECTED) {
        if (sock->remote_ip != 0u && packet->src_ip != sock->remote_ip) return 0;
        if (sock->remote_port != 0u && packet->src_port != sock->remote_port) return 0;
    }
    return 1;
}

int socket_udp_recv_ready(socket_t* sock) {
    if (!sock || sock->kind != SOCKET_KIND_UDP) return 0;
    net_poll_drain();
    for (unsigned int i = 0; i < SOCKET_UDP_QUEUE_MAX; i++) {
        if (socket_udp_packet_matches(sock, &s_udp_packets[i])) return 1;
    }
    return 0;
}

int socket_udp_recv(socket_t* sock,
                    void* buf,
                    unsigned int len,
                    unsigned int* out_src_ip,
                    unsigned int* out_src_port) {
    if (!sock || sock->kind != SOCKET_KIND_UDP || !buf) return -EINVAL;
    if (!socket_udp_recv_ready(sock)) return -EAGAIN;
    for (unsigned int i = 0; i < SOCKET_UDP_QUEUE_MAX; i++) {
        udp_packet_t* packet = &s_udp_packets[i];
        if (!socket_udp_packet_matches(sock, packet)) continue;
        unsigned int copy_len = packet->len;
        if (copy_len > len) copy_len = len;
        k_memcpy(buf, packet->payload, copy_len);
        if (out_src_ip) *out_src_ip = packet->src_ip;
        if (out_src_port) *out_src_port = packet->src_port;
        k_memset(packet, 0, sizeof(*packet));
        return (int)copy_len;
    }
    return -EAGAIN;
}

int socket_udp_deliver(unsigned int src_ip,
                       unsigned int src_port,
                       unsigned int dst_ip,
                       unsigned int dst_port,
                       const void* payload,
                       unsigned int len) {
    udp_packet_t* slot = 0;
    int any_listener = 0;

    if (!payload || len == 0u || len > SOCKET_UDP_MAX_PACKET ||
        src_port == 0u || src_port > 0xFFFFu ||
        dst_port == 0u || dst_port > 0xFFFFu) {
        return 0;
    }

    for (unsigned int i = 0; i < SOCKET_MAX; i++) {
        socket_t* sock = &s_sockets[i];
        if (sock->refs == 0u || sock->kind != SOCKET_KIND_UDP) continue;
        if (sock->local_port != (unsigned short)dst_port) continue;
        if (sock->state != SOCKET_STATE_BOUND &&
            sock->state != SOCKET_STATE_CONNECTED) continue;
        if (sock->state == SOCKET_STATE_CONNECTED &&
            ((sock->remote_ip != 0u && sock->remote_ip != src_ip) ||
             (sock->remote_port != 0u && sock->remote_port != (unsigned short)src_port))) {
            continue;
        }
        any_listener = 1;
        break;
    }
    if (!any_listener) return 0;

    for (unsigned int i = 0; i < SOCKET_UDP_QUEUE_MAX; i++) {
        if (!s_udp_packets[i].used) {
            slot = &s_udp_packets[i];
            break;
        }
    }
    if (!slot) return 0;

    k_memset(slot, 0, sizeof(*slot));
    slot->used = 1u;
    slot->src_ip = src_ip;
    slot->dst_ip = dst_ip;
    slot->src_port = (unsigned short)src_port;
    slot->dst_port = (unsigned short)dst_port;
    slot->len = len;
    k_memcpy(slot->payload, payload, len);

    for (unsigned int i = 0; i < SOCKET_MAX; i++) {
        socket_t* sock = &s_sockets[i];
        if (sock->refs != 0u && socket_udp_packet_matches(sock, slot)) {
            wait_queue_wake_all(&sock->read_waiters);
        }
    }
    return 1;
}

short socket_poll(socket_t* sock, short events) {
    short revents = 0;

    if (!sock || sock->kind == SOCKET_KIND_NONE) return POLLERR;
    if (sock->kind == SOCKET_KIND_UDP) {
        if ((events & POLLIN) && socket_udp_recv_ready(sock)) {
            revents |= POLLIN;
        }
        if ((events & POLLOUT) != 0) {
            revents |= POLLOUT;
        }
        return revents;
    }
    if (sock->kind == SOCKET_KIND_RAW_ICMP) {
        if ((events & POLLIN) && socket_raw_icmp_recv_ready(sock)) {
            revents |= POLLIN;
        }
        return revents;
    }
    if (sock->kind != SOCKET_KIND_TCP) return 0;
    net_poll_drain();

    if (sock->state == SOCKET_STATE_LISTENING) {
        socket_tcp_use(sock);
        if ((events & POLLIN) && tcp_socket_accept_ready()) {
            revents |= POLLIN;
        }
    } else if (sock->state == SOCKET_STATE_CONNECTING) {
        socket_tcp_use(sock);
        if (tcp_socket_connection_established()) {
            sock->state = SOCKET_STATE_CONNECTED;
            if ((events & POLLOUT) != 0) {
                revents |= POLLOUT;
            }
        } else if (!tcp_socket_connect_pending()) {
            revents |= POLLERR | POLLHUP;
        }
    } else if (sock->state == SOCKET_STATE_CONNECTED) {
        int established;
        int peer_closed;

        socket_tcp_use(sock);
        if ((events & POLLIN) &&
            (((sock->flags & SOCKET_FLAG_READ_SHUTDOWN) != 0u) ||
             tcp_socket_recv_ready())) {
            revents |= POLLIN;
        }
        established = tcp_socket_connection_established();
        peer_closed = tcp_socket_peer_closed();
        if (peer_closed || !established) {
            revents |= POLLHUP;
        }
        if ((events & POLLOUT) &&
            ((sock->flags & SOCKET_FLAG_WRITE_SHUTDOWN) == 0u) &&
            established &&
            tcp_socket_send_ready()) {
            revents |= POLLOUT;
        }
    }

    return revents;
}

int socket_wait(socket_t* sock, process_t* proc, short events) {
    int rc = 0;

    if (!sock || !proc) return -EINVAL;
    if (sock->kind == SOCKET_KIND_UDP) {
        if ((events & POLLIN) != 0) {
            rc = wait_queue_add(&sock->read_waiters, proc);
        }
        if (rc >= 0 && (events & POLLOUT) != 0) {
            rc = wait_queue_add(&sock->write_waiters, proc);
        }
        if (rc < 0) wait_queue_remove_proc(proc);
        return rc;
    }
    if (sock->kind == SOCKET_KIND_RAW_ICMP) {
        if ((events & POLLIN) != 0) {
            rc = wait_queue_add(&sock->read_waiters, proc);
        }
        if (rc < 0) wait_queue_remove_proc(proc);
        return rc;
    }
    if (sock->kind != SOCKET_KIND_TCP) return -EINVAL;

    if (sock->state == SOCKET_STATE_LISTENING) {
        if ((events & POLLIN) != 0) {
            rc = wait_queue_add(&sock->accept_waiters, proc);
        }
    } else if (sock->state == SOCKET_STATE_CONNECTED ||
               sock->state == SOCKET_STATE_CONNECTING) {
        if ((events & POLLIN) != 0) {
            rc = wait_queue_add(&sock->read_waiters, proc);
        }
        if (rc >= 0 && (events & POLLOUT) != 0) {
            rc = wait_queue_add(&sock->write_waiters, proc);
        }
    }

    if (rc < 0) {
        wait_queue_remove_proc(proc);
    }
    return rc;
}

void socket_wait_clear_process(process_t* proc) {
    wait_queue_remove_proc(proc);
}

void socket_wake_tcp_listener(unsigned int port) {
    if (port == 0u) return;

    for (unsigned int i = 0; i < SOCKET_MAX; i++) {
        socket_t* sock = &s_sockets[i];
        if (sock->refs == 0u) continue;
        if (sock->kind != SOCKET_KIND_TCP) continue;
        if (sock->state != SOCKET_STATE_LISTENING) continue;
        if (sock->local_port != (unsigned short)port) continue;

        wait_queue_wake_all(&sock->accept_waiters);
    }
}

void socket_wake_tcp_connection(unsigned int port,
                                unsigned int conn_id,
                                short events) {
    if (port == 0u || conn_id == TCP_SOCKET_CONN_NONE) return;

    for (unsigned int i = 0; i < SOCKET_MAX; i++) {
        socket_t* sock = &s_sockets[i];
        if (sock->refs == 0u) continue;
        if (sock->kind != SOCKET_KIND_TCP) continue;
        if (sock->state != SOCKET_STATE_CONNECTED &&
            sock->state != SOCKET_STATE_CONNECTING) continue;
        if (sock->local_port != (unsigned short)port) continue;
        if (sock->conn_id != conn_id) continue;

        if ((events & (POLLIN | POLLHUP | POLLERR)) != 0) {
            wait_queue_wake_all(&sock->read_waiters);
        }
        if ((events & (POLLOUT | POLLHUP | POLLERR)) != 0) {
            wait_queue_wake_all(&sock->write_waiters);
        }
    }
}

void socket_close(socket_t* sock) {
    if (!sock || sock->refs == 0u) return;

    wait_queue_wake_all(&sock->accept_waiters);
    wait_queue_wake_all(&sock->read_waiters);
    wait_queue_wake_all(&sock->write_waiters);

    if (sock->kind != SOCKET_KIND_TCP) {
        sock->state = SOCKET_STATE_CLOSED;
        sock->local_port = 0u;
        sock->remote_ip = 0u;
        sock->remote_port = 0u;
        sock->conn_id = TCP_SOCKET_CONN_NONE;
        sock->backlog = 0u;
        return;
    }

    if (sock->state == SOCKET_STATE_LISTENING) {
        tcp_socket_close_listener(sock->local_port);
    } else if (sock->state == SOCKET_STATE_CONNECTED ||
               sock->state == SOCKET_STATE_CONNECTING) {
        tcp_socket_close_connection(sock->local_port, sock->conn_id);
    }

    sock->state = SOCKET_STATE_CLOSED;
    sock->local_port = 0u;
    sock->conn_id = TCP_SOCKET_CONN_NONE;
    sock->backlog = 0u;
}
