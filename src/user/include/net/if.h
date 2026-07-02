#ifndef USER_NET_IF_H
#define USER_NET_IF_H

#define IFNAMSIZ 16

struct ifreq {
    char ifr_name[IFNAMSIZ];
};

unsigned int if_nametoindex(const char* ifname);
char* if_indextoname(unsigned int ifindex, char* ifname);

#endif /* USER_NET_IF_H */
