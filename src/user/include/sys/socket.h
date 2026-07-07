#ifndef USER_SYS_SOCKET_H
#define USER_SYS_SOCKET_H

#include "uapi_socket.h"
#include "../stddef.h"
#include "uio.h"

struct msghdr {
    void* msg_name;
    socklen_t msg_namelen;
    struct iovec* msg_iov;
    size_t msg_iovlen;
    void* msg_control;
    size_t msg_controllen;
    int msg_flags;
};

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr* addr, socklen_t addrlen);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr* addr, socklen_t* addrlen);
int accept4(int fd, struct sockaddr* addr, socklen_t* addrlen, int flags);
int connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
int send(int fd, const void* buf, size_t len, int flags);
int sendto(int fd, const void* buf, size_t len, int flags,
           const struct sockaddr* dest_addr, socklen_t addrlen);
ssize_t sendmsg(int fd, const struct msghdr* msg, int flags);
int recv(int fd, void* buf, size_t len, int flags);
int recvfrom(int fd, void* buf, size_t len, int flags,
             struct sockaddr* src_addr, socklen_t* addrlen);
ssize_t recvmsg(int fd, struct msghdr* msg, int flags);
int setsockopt(int fd, int level, int optname, const void* optval, unsigned int optlen);
int getsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen);
int getsockname(int fd, struct sockaddr* addr, socklen_t* addrlen);
int getpeername(int fd, struct sockaddr* addr, socklen_t* addrlen);
int shutdown(int fd, int how);

#endif /* USER_SYS_SOCKET_H */
