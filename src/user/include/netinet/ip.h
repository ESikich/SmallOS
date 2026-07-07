#ifndef USER_NETINET_IP_H
#define USER_NETINET_IP_H

#include "../stdint.h"

struct iphdr {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    unsigned int ihl:4;
    unsigned int version:4;
#else
    unsigned int version:4;
    unsigned int ihl:4;
#endif
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t check;
    uint32_t saddr;
    uint32_t daddr;
};

#define IP_TTL 2
#define IP_MULTICAST_TTL 33
#define IPVERSION 4
#define IPDEFTTL 64

#endif /* USER_NETINET_IP_H */
