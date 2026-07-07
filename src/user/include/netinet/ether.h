#ifndef USER_NETINET_ETHER_H
#define USER_NETINET_ETHER_H

#include "if_ether.h"

struct ether_addr* ether_aton_r(const char* ascii, struct ether_addr* addr);

#endif /* USER_NETINET_ETHER_H */
