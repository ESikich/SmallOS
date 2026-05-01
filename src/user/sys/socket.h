#ifndef USER_SYS_SOCKET_H
#define USER_SYS_SOCKET_H

#include "uapi_socket.h"

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr* addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr* addr, socklen_t* addrlen);
int connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
int send(int fd, const void* buf, unsigned int len);
int recv(int fd, void* buf, unsigned int len);

#endif /* USER_SYS_SOCKET_H */
