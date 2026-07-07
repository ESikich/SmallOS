#ifndef UAPI_NET_H
#define UAPI_NET_H

#include "uapi_socket.h"

#define IFNAMSIZ 16
#define ETH_ALEN 6

#define IFF_UP          0x0001u
#define IFF_BROADCAST   0x0002u
#define IFF_DEBUG       0x0004u
#define IFF_LOOPBACK    0x0008u
#define IFF_POINTOPOINT 0x0010u
#define IFF_NOTRAILERS  0x0020u
#define IFF_RUNNING     0x0040u
#define IFF_NOARP       0x0080u
#define IFF_PROMISC     0x0100u
#define IFF_ALLMULTI    0x0200u
#define IFF_MASTER      0x0400u
#define IFF_SLAVE       0x0800u
#define IFF_MULTICAST   0x1000u

#define ARPHRD_NETROM 0u
#define ARPHRD_ETHER 1u
#define ARPHRD_EETHER 2u
#define ARPHRD_AX25 3u
#define ARPHRD_PRONET 4u
#define ARPHRD_CHAOS 5u
#define ARPHRD_IEEE802 6u
#define ARPHRD_ARCNET 7u
#define ARPHRD_APPLETLK 8u
#define ARPHRD_DLCI 15u
#define ARPHRD_ATM 19u
#define ARPHRD_METRICOM 23u
#define ARPHRD_IEEE1394 24u
#define ARPHRD_EUI64 27u
#define ARPHRD_INFINIBAND 32u
#define ARPHRD_SLIP 256u
#define ARPHRD_CSLIP 257u
#define ARPHRD_SLIP6 258u
#define ARPHRD_CSLIP6 259u
#define ARPHRD_RSRVD 260u
#define ARPHRD_ADAPT 264u
#define ARPHRD_ROSE 270u
#define ARPHRD_X25 271u
#define ARPHRD_HWX25 272u
#define ARPHRD_PPP 512u
#define ARPHRD_CISCO 513u
#define ARPHRD_HDLC ARPHRD_CISCO
#define ARPHRD_LAPB 516u
#define ARPHRD_DDCMP 517u
#define ARPHRD_RAWHDLC 518u
#define ARPHRD_TUNNEL 768u
#define ARPHRD_TUNNEL6 769u
#define ARPHRD_FRAD 770u
#define ARPHRD_SKIP 771u
#define ARPHRD_LOOPBACK 772u
#define ARPHRD_LOCALTLK 773u
#define ARPHRD_FDDI 774u
#define ARPHRD_BIF 775u
#define ARPHRD_SIT 776u
#define ARPHRD_IPDDP 777u
#define ARPHRD_IPGRE 778u
#define ARPHRD_PIMREG 779u
#define ARPHRD_HIPPI 780u
#define ARPHRD_ASH 781u
#define ARPHRD_ECONET 782u
#define ARPHRD_IRDA 783u
#define ARPHRD_VOID 0xFFFFu
#define ARPHRD_NONE 0xFFFEu


#define SIOCADDRT       0x890Bu
#define SIOCDELRT       0x890Cu
#define SIOCGIFNAME     0x8910u
#define SIOCSIFLINK     0x8911u
#define SIOCGIFCONF     0x8912u
#define SIOCGIFFLAGS    0x8913u
#define SIOCSIFFLAGS    0x8914u
#define SIOCGIFADDR     0x8915u
#define SIOCSIFADDR     0x8916u
#define SIOCGIFDSTADDR  0x8917u
#define SIOCSIFDSTADDR  0x8918u
#define SIOCGIFBRDADDR  0x8919u
#define SIOCSIFBRDADDR  0x891Au
#define SIOCGIFNETMASK  0x891Bu
#define SIOCSIFNETMASK  0x891Cu
#define SIOCGIFMETRIC   0x891Du
#define SIOCSIFMETRIC   0x891Eu
#define SIOCGIFMEM      0x891Fu
#define SIOCSIFMEM      0x8920u
#define SIOCGIFMTU      0x8921u
#define SIOCSIFMTU      0x8922u
#define SIOCSIFNAME     0x8923u
#define SIOCGIFHWADDR   0x8927u
#define SIOCSIFHWADDR   0x8924u
#define SIOCDARP        0x8953u
#define SIOCGARP        0x8954u
#define SIOCSARP        0x8955u
#define SIOCGIFTXQLEN   0x8942u
#define SIOCSIFTXQLEN   0x8943u
#define SIOCGIFINDEX    0x8933u
#define SIOGIFINDEX     SIOCGIFINDEX
#define SIOCSIFHWBROADCAST 0x8937u

#define RTF_UP      0x0001u
#define RTF_GATEWAY 0x0002u
#define RTF_HOST    0x0004u
#define RTF_REINSTATE 0x0008u
#define RTF_DYNAMIC 0x0010u
#define RTF_MODIFIED 0x0020u
#define RTF_MSS     0x0040u
#define RTF_WINDOW  0x0080u
#define RTF_IRTT    0x0100u
#define RTF_REJECT  0x0200u

#define ATF_COM         0x02u
#define ATF_PERM        0x04u
#define ATF_PUBL        0x08u
#define ATF_USETRAILERS 0x10u
#define ATF_NETMASK     0x20u

struct ifmap {
    unsigned long mem_start;
    unsigned long mem_end;
    unsigned short base_addr;
    unsigned char irq;
    unsigned char dma;
    unsigned char port;
};

struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        struct sockaddr ifru_netmask;
        struct sockaddr ifru_hwaddr;
        short ifru_flags;
        int ifru_ivalue;
        int ifru_mtu;
        struct ifmap ifru_map;
        char ifru_slave[IFNAMSIZ];
        char ifru_newname[IFNAMSIZ];
        void* ifru_data;
    } ifr_ifru;
};

#define ifr_addr      ifr_ifru.ifru_addr
#define ifr_dstaddr   ifr_ifru.ifru_dstaddr
#define ifr_broadaddr ifr_ifru.ifru_broadaddr
#define ifr_netmask   ifr_ifru.ifru_netmask
#define ifr_hwaddr    ifr_ifru.ifru_hwaddr
#define ifr_flags     ifr_ifru.ifru_flags
#define ifr_metric    ifr_ifru.ifru_ivalue
#define ifr_mtu       ifr_ifru.ifru_mtu
#define ifr_map       ifr_ifru.ifru_map
#define ifr_slave     ifr_ifru.ifru_slave
#define ifr_data      ifr_ifru.ifru_data
#define ifr_ifindex   ifr_ifru.ifru_ivalue
#define ifr_bandwidth ifr_ifru.ifru_ivalue
#define ifr_qlen      ifr_ifru.ifru_ivalue
#define ifr_newname   ifr_ifru.ifru_newname

struct ifconf {
    int ifc_len;
    union {
        char* ifcu_buf;
        struct ifreq* ifcu_req;
    } ifc_ifcu;
};

#define ifc_buf ifc_ifcu.ifcu_buf
#define ifc_req ifc_ifcu.ifcu_req

struct rtentry {
    unsigned long rt_pad1;
    struct sockaddr rt_dst;
    struct sockaddr rt_gateway;
    struct sockaddr rt_genmask;
    unsigned short rt_flags;
    short rt_pad2;
    unsigned long rt_pad3;
    void* rt_pad4;
    short rt_metric;
    char* rt_dev;
    unsigned long rt_mtu;
    unsigned long rt_window;
    unsigned short rt_irtt;
};

#define rt_mss rt_mtu

struct arpreq {
    struct sockaddr arp_pa;
    struct sockaddr arp_ha;
    int arp_flags;
    struct sockaddr arp_netmask;
    char arp_dev[IFNAMSIZ];
};

#endif /* UAPI_NET_H */
