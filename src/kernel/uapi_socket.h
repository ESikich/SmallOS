#ifndef UAPI_SOCKET_H
#define UAPI_SOCKET_H

#include "uapi_syscall.h"

typedef unsigned short sa_family_t;
typedef unsigned short in_port_t;
typedef unsigned int   socklen_t;

struct in_addr {
    unsigned int s_addr;
};

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    unsigned char  sin_zero[8];
};

#define AF_INET      2
#define SOCK_STREAM  1
#define IPPROTO_TCP  6

#endif /* UAPI_SOCKET_H */
