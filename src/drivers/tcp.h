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

void tcp_init(void);
int  tcp_handle_ipv4_frame(const unsigned char* frame, unsigned int len);
void tcp_socket_handle_close(fd_entry_t* ent);
void tcp_socket_use_port(unsigned int port);
int  tcp_socket_bind(unsigned int port);
int  tcp_socket_listen(void);
int  tcp_socket_accept_ready(void);
void tcp_socket_mark_accepted(void);
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

#endif /* TCP_H */
