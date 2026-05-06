#ifndef UAPI_EPOLL_H
#define UAPI_EPOLL_H

#include "uapi_poll.h"

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLIN  POLLIN
#define EPOLLPRI POLLPRI
#define EPOLLOUT POLLOUT
#define EPOLLERR POLLERR
#define EPOLLHUP POLLHUP
#define EPOLLET  0x80000000u

#define EPOLL_CLOEXEC 0x00080000u

typedef union epoll_data {
    void* ptr;
    int fd;
    unsigned int u32;
    unsigned long long u64;
} epoll_data_t;

struct epoll_event {
    unsigned int events;
    epoll_data_t data;
};

#endif /* UAPI_EPOLL_H */
