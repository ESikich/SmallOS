#ifndef TCP_H
#define TCP_H

#include "../kernel/process.h"

/*
 * Minimal kernel TCP bring-up.
 *
 * The first cut is a single passive listener that accepts one connection
 * at a time and echoes received payload back to the peer.  It runs as a
 * background kernel task so the rest of the system can keep booting and
 * the existing test suite stays unchanged.
 */

void tcp_init(void);
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
void tcp_socket_wake_waiter(void);
unsigned int tcp_socket_peer_ip(void);
unsigned int tcp_socket_peer_port(void);
unsigned int tcp_socket_local_port(void);

#endif /* TCP_H */
