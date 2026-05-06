#ifndef TCP_H
#define TCP_H

#include "../kernel/process.h"

/*
 * Minimal kernel TCP service.
 *
 * A background kernel task drains NIC receive work, dispatches TCP frames,
 * manages a tiny set of passive stream slots, handles retransmit/idle timers,
 * and wakes user-space socket waiters.  It is intentionally narrow but now
 * backs normal guest services such as tcpecho, sockeof, and ftpd.
 */

int  tcp_init(void);
int  tcp_handle_ipv4_frame(const unsigned char* frame, unsigned int len);
void tcp_socket_close_listener(unsigned int port);
void tcp_socket_close_connection(unsigned int port, unsigned int conn_id);
void tcp_socket_use_port(unsigned int port);
void tcp_socket_use_connection(unsigned int port, unsigned int conn_id);
int  tcp_socket_bind(unsigned int port);
int  tcp_socket_listen(unsigned int backlog);
int  tcp_socket_accept_ready(void);
unsigned int tcp_socket_mark_accepted(void);
int  tcp_socket_connection_established(void);
int  tcp_socket_recv_ready(void);
int  tcp_socket_peer_closed(void);
int  tcp_socket_recv(void* buf, unsigned int len);
int  tcp_socket_send(const void* buf, unsigned int len);
unsigned int tcp_socket_poll_events(void);
void tcp_socket_set_waiter(process_t* proc);
void tcp_socket_clear_waiter(process_t* proc);
void tcp_socket_wake_waiter(void);
unsigned int tcp_socket_peer_ip(void);
unsigned int tcp_socket_peer_port(void);
unsigned int tcp_socket_local_port(void);

#define TCP_SOCKET_CONN_NONE 0xFFFFFFFFu

#endif /* TCP_H */
