#ifndef SOCKET_H
#define SOCKET_H

#include "types.h"

typedef struct process process_t;
typedef enum {
    SOCKET_KIND_NONE = 0,
    SOCKET_KIND_TCP  = 1
} socket_kind_t;

typedef enum {
    SOCKET_STATE_CLOSED    = 0,
    SOCKET_STATE_OPEN      = 1,
    SOCKET_STATE_BOUND     = 2,
    SOCKET_STATE_LISTENING = 3,
    SOCKET_STATE_CONNECTED = 4
} socket_state_t;

typedef struct socket socket_t;

#define SOCKET_MAX         256u
#define SOCKET_BACKLOG_MAX 32u

socket_t*      socket_create_tcp(void);
void           socket_retain(socket_t* sock);
void           socket_release(socket_t* sock);
socket_kind_t  socket_kind(socket_t* sock);
socket_state_t socket_state(socket_t* sock);
unsigned int   socket_local_port(socket_t* sock);
unsigned int   socket_conn_id(socket_t* sock);
unsigned int   socket_peer_ip(socket_t* sock);
unsigned int   socket_peer_port(socket_t* sock);

int            socket_bind_tcp(socket_t* sock, unsigned int port);
int            socket_listen_tcp(socket_t* sock, int backlog);
int            socket_accept_ready(socket_t* sock);
int            socket_accept_tcp(socket_t* listener, socket_t* child);
int            socket_tcp_connection_established(socket_t* sock);
int            socket_tcp_recv_ready(socket_t* sock);
int            socket_tcp_peer_closed(socket_t* sock);
int            socket_tcp_recv(socket_t* sock, void* buf, unsigned int len);
int            socket_tcp_send_ready(socket_t* sock);
int            socket_tcp_send(socket_t* sock, const void* buf, unsigned int len);
short          socket_poll(socket_t* sock, short events);
int            socket_wait(socket_t* sock, process_t* proc, short events);
void           socket_wait_clear_process(process_t* proc);
void           socket_wake_tcp_listener(unsigned int port);
void           socket_wake_tcp_connection(unsigned int port,
                                          unsigned int conn_id,
                                          short events);
void           socket_close(socket_t* sock);

#endif /* SOCKET_H */
