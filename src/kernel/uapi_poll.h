#ifndef UAPI_POLL_H
#define UAPI_POLL_H

typedef unsigned int nfds_t;

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010

#endif /* UAPI_POLL_H */
