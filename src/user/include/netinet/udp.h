#ifndef USER_NETINET_UDP_H
#define USER_NETINET_UDP_H

#include "../stdint.h"

struct udphdr {
    uint16_t source;
    uint16_t dest;
    uint16_t len;
    uint16_t check;
};

#endif /* USER_NETINET_UDP_H */
