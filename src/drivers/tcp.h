#ifndef TCP_H
#define TCP_H

/*
 * Minimal kernel TCP service.
 *
 * A background kernel task drains NIC receive work, dispatches TCP frames,
 * manages passive listeners plus a global 4-tuple connection table, handles
 * retransmit/idle timers, and wakes user-space socket waiters.  It is
 * intentionally narrow but now backs normal guest services such as tcpecho,
 * sockeof, and ftpd.
 */

typedef struct {
    unsigned int max_listeners;
    unsigned int listeners;
    unsigned int max_connections;
    unsigned int max_connections_per_listener;
    unsigned int connections;
    unsigned int syn_recv_connections;
    unsigned int established_connections;
    unsigned int fin_wait_connections;
    unsigned int accepted_connections;
    unsigned int pending_connections;
    unsigned int max_backlog;
    unsigned int rx_rings;
    unsigned int tx_rings;
    unsigned int rx_bytes;
    unsigned int tx_bytes;
    unsigned int rx_buffer_bytes;
    unsigned int tx_buffer_bytes;
    unsigned int max_rx_buffer_bytes;
    unsigned int max_tx_buffer_bytes;
} tcp_stats_t;

int  tcp_init(void);
int  tcp_handle_ipv4_frame(const unsigned char* frame, unsigned int len);
void tcp_get_stats(tcp_stats_t* out);
void tcp_socket_close_listener(unsigned int port);
void tcp_socket_close_connection(unsigned int port, unsigned int conn_id);
void tcp_socket_use_port(unsigned int port);
void tcp_socket_use_connection(unsigned int port, unsigned int conn_id);
int  tcp_socket_bind(unsigned int port);
int  tcp_socket_listen(unsigned int backlog);
int  tcp_socket_connect(unsigned int local_port,
                        unsigned int remote_ip,
                        unsigned int remote_port,
                        unsigned int* out_local_port,
                        unsigned int* out_conn_id);
int  tcp_socket_accept_ready(void);
unsigned int tcp_socket_mark_accepted(void);
int  tcp_socket_connection_established(void);
int  tcp_socket_connect_pending(void);
int  tcp_socket_recv_ready(void);
int  tcp_socket_peer_closed(void);
int  tcp_socket_recv(void* buf, unsigned int len);
int  tcp_socket_send_ready(void);
int  tcp_socket_send(const void* buf, unsigned int len);
int  tcp_socket_shutdown(int how);
unsigned int tcp_socket_poll_events(void);
unsigned int tcp_socket_peer_ip(void);
unsigned int tcp_socket_peer_port(void);
unsigned int tcp_socket_local_ip(void);
unsigned int tcp_socket_local_port(void);

#define TCP_SOCKET_CONN_NONE 0xFFFFFFFFu

#endif /* TCP_H */
