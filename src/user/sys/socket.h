#ifndef USER_SYS_SOCKET_H
#define USER_SYS_SOCKET_H

#include "uapi_socket.h"
#include "../stddef.h"

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr* addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr* addr, socklen_t* addrlen);
int accept4(int fd, struct sockaddr* addr, socklen_t* addrlen, int flags);
int connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
int send(int fd, const void* buf, size_t len, int flags);
int recv(int fd, void* buf, size_t len, int flags);
int setsockopt(int fd, int level, int optname, const void* optval, unsigned int optlen);
int getsockname(int fd, struct sockaddr* addr, socklen_t* addrlen);
int getpeername(int fd, struct sockaddr* addr, socklen_t* addrlen);
int shutdown(int fd, int how);

#endif /* USER_SYS_SOCKET_H */
