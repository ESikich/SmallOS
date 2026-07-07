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

#define INADDR_ANY      0u
#define INADDR_LOOPBACK 0x7F000001u

#define AF_UNSPEC    0
#define AF_UNIX      1
#define AF_LOCAL     AF_UNIX
#define AF_INET      2
#define AF_INET6     10
#define AF_NETLINK   16
#define AF_PACKET    17
#define PF_UNSPEC    AF_UNSPEC
#define PF_UNIX      AF_UNIX
#define PF_LOCAL     AF_LOCAL
#define PF_INET      AF_INET
#define PF_INET6     AF_INET6
#define PF_NETLINK   AF_NETLINK
#define PF_PACKET    AF_PACKET
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define SOCK_RAW     3
#define SOCK_RDM     4
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK SYS_FD_FLAG_NONBLOCK
#define SOCK_CLOEXEC  0x00080000u
#define SOCK_TYPE_MASK 0x0000000Fu
#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

#define NETLINK_ROUTE 0

#define MSG_DONTWAIT 0x40u
#define MSG_PEEK     0x02u
#define MSG_NOSIGNAL 0x4000u
#define MSG_TRUNC    0x20u

#define SOL_SOCKET   1
#define SOL_IP       IPPROTO_IP
#define SO_REUSEADDR 2
#define SO_BROADCAST 6
#define SO_KEEPALIVE 9
#define SO_ERROR     4
#define SO_RCVBUF    8
#define SO_SNDBUF    7

#define SOMAXCONN    128

#define SHUT_RD      0
#define SHUT_WR      1
#define SHUT_RDWR    2

#endif /* UAPI_SOCKET_H */
