#ifndef USER_LINUX_IF_PACKET_H
#define USER_LINUX_IF_PACKET_H

#include "../netpacket/packet.h"
#include "../stdint.h"

#define PACKET_HOST      0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3
#define PACKET_OUTGOING  4
#define PACKET_AUXDATA   8

#define TP_STATUS_CSUMNOTREADY 0x00000008u

struct tpacket_auxdata {
    uint32_t tp_status;
    uint32_t tp_len;
    uint32_t tp_snaplen;
    uint16_t tp_mac;
    uint16_t tp_net;
    uint16_t tp_vlan_tci;
    uint16_t tp_padding;
};

#endif /* USER_LINUX_IF_PACKET_H */
