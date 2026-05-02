#include "net.h"

#include "arp.h"
#include "e1000.h"
#include "ipv4.h"
#include "tcp.h"
#include "../kernel/types.h"

#define NET_ETHERTYPE_ARP  0x0806u
#define NET_ETHERTYPE_IPV4 0x0800u
#define NET_IPV4_PROTO_ICMP 1u
#define NET_IPV4_PROTO_TCP  6u
#define NET_MAX_FRAME 1600u

static u8 s_net_frame[NET_MAX_FRAME];

static u16 net_read_u16_be(const u8* buf, u32 off) {
    return (u16)(((u16)buf[off] << 8) | (u16)buf[off + 1]);
}

int net_poll_once(void) {
    u32 len = 0;
    u16 ether_type;

    if (!e1000_recv(s_net_frame, sizeof(s_net_frame), &len)) {
        return 0;
    }

    if (len < 14u) {
        return 1;
    }

    ether_type = net_read_u16_be(s_net_frame, 12);
    if (ether_type == NET_ETHERTYPE_ARP) {
        (void)arp_handle_frame(s_net_frame, len);
        return 1;
    }

    if (ether_type == NET_ETHERTYPE_IPV4 && len >= 34u) {
        if (s_net_frame[23] == NET_IPV4_PROTO_ICMP) {
            (void)ipv4_handle_frame(s_net_frame, len);
        } else if (s_net_frame[23] == NET_IPV4_PROTO_TCP) {
            (void)tcp_handle_ipv4_frame(s_net_frame, len);
        }
        return 1;
    }

    return 1;
}

void net_poll_drain(void) {
    while (net_poll_once()) {
    }
}
