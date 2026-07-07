#ifndef USER_NET_ETHERNET_H
#define USER_NET_ETHERNET_H

#define ETH_ALEN 6
#define ETH_TLEN 2
#define ETH_HLEN 14
#define ETH_ZLEN 60
#define ETH_DATA_LEN 1500
#define ETH_FRAME_LEN 1514

#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806

struct ether_addr {
    unsigned char ether_addr_octet[ETH_ALEN];
};

#endif /* USER_NET_ETHERNET_H */
