#include "net.h"

#include "arp.h"
#include "dhcp.h"
#include "ipv4.h"
#include "nic.h"
#include "ntp.h"
#include "tcp.h"
#include "../kernel/socket.h"
#include "../kernel/types.h"
#include "terminal.h"

typedef unsigned short u16;

#define NET_ETHERTYPE_ARP  0x0806u
#define NET_ETHERTYPE_IPV4 0x0800u
#define NET_IPV4_PROTO_ICMP 1u
#define NET_IPV4_PROTO_TCP  6u
#define NET_IPV4_PROTO_UDP  17u
#define NET_MAX_FRAME 1600u

static u8 s_net_frame[NET_MAX_FRAME];
static net_ipv4_config_t s_ipv4_config;
static volatile int s_net_poll_locked = 0;

static u16 net_read_u16_be(const u8* buf, u32 off) {
    return (u16)(((u16)buf[off] << 8) | (u16)buf[off + 1]);
}

static u32 net_read_u32_be(const u8* buf, u32 off) {
    return ((u32)buf[off] << 24)
         | ((u32)buf[off + 1] << 16)
         | ((u32)buf[off + 2] << 8)
         | (u32)buf[off + 3];
}

int net_poll_once(void) {
    u32 len = 0;
    u16 ether_type;

    if (!__sync_bool_compare_and_swap(&s_net_poll_locked, 0, 1)) {
        return 0;
    }
    if (!nic_recv(s_net_frame, sizeof(s_net_frame), &len)) {
        __sync_lock_release(&s_net_poll_locked);
        return 0;
    }

    if (len < 14u) {
        __sync_lock_release(&s_net_poll_locked);
        return 1;
    }

    ether_type = net_read_u16_be(s_net_frame, 12);
    if (ether_type == NET_ETHERTYPE_ARP) {
        (void)arp_handle_frame(s_net_frame, len);
    } else if (ether_type == NET_ETHERTYPE_IPV4 && len >= 34u) {
        if (s_net_frame[23] == NET_IPV4_PROTO_ICMP) {
            u32 ihl = (u32)(s_net_frame[14] & 0x0Fu) * 4u;
            u32 total_len = net_read_u16_be(s_net_frame, 16);
            if (ihl >= 20u && total_len >= ihl + 8u && 14u + total_len <= len) {
                u32 src_ip = ((u32)s_net_frame[26] << 24)
                           | ((u32)s_net_frame[27] << 16)
                           | ((u32)s_net_frame[28] << 8)
                           | (u32)s_net_frame[29];
                (void)socket_raw_icmp_deliver(&s_net_frame[14], total_len, src_ip);
            }
            (void)ipv4_handle_frame(s_net_frame, len);
        } else if (s_net_frame[23] == NET_IPV4_PROTO_TCP) {
            (void)tcp_handle_ipv4_frame(s_net_frame, len);
        } else if (s_net_frame[23] == NET_IPV4_PROTO_UDP) {
            u32 ihl = (u32)(s_net_frame[14] & 0x0Fu) * 4u;
            u32 total_len = net_read_u16_be(s_net_frame, 16);
            int delivered = 0;
            if (ihl >= 20u && total_len >= ihl + 8u && 14u + total_len <= len) {
                u32 udp_off = 14u + ihl;
                u16 src_port = net_read_u16_be(s_net_frame, udp_off);
                u16 dst_port = net_read_u16_be(s_net_frame, udp_off + 2u);
                u16 udp_len = net_read_u16_be(s_net_frame, udp_off + 4u);
                if (udp_len >= 8u && udp_off + udp_len <= len) {
                    delivered = socket_udp_deliver(net_read_u32_be(s_net_frame, 26u),
                                                   src_port,
                                                   net_read_u32_be(s_net_frame, 30u),
                                                   dst_port,
                                                   &s_net_frame[udp_off + 8u],
                                                   (u32)udp_len - 8u);
                }
            }
            if (!delivered && !dhcp_handle_ipv4_frame(s_net_frame, len)) {
                (void)ntp_handle_ipv4_frame(s_net_frame, len);
            }
        }
    }

    __sync_lock_release(&s_net_poll_locked);
    return 1;
}

unsigned int net_poll_drain_budget(unsigned int max_frames) {
    unsigned int frames = 0;

    while (frames < max_frames && net_poll_once()) {
        frames++;
    }

    return frames;
}

void net_poll_drain(void) {
    while (net_poll_drain_budget(32u) == 32u) {
    }
}

void net_ipv4_clear_config(void) {
    s_ipv4_config.configured = 0;
    s_ipv4_config.ip = 0;
    s_ipv4_config.netmask = 0;
    s_ipv4_config.gateway = 0;
    s_ipv4_config.dns = 0;
    s_ipv4_config.dhcp_server = 0;
    s_ipv4_config.lease_seconds = 0;
}

void net_ipv4_configure(u32 ip,
                        u32 netmask,
                        u32 gateway,
                        u32 dns,
                        u32 dhcp_server,
                        u32 lease_seconds) {
    s_ipv4_config.configured = ip != 0u;
    s_ipv4_config.ip = ip;
    s_ipv4_config.netmask = netmask;
    s_ipv4_config.gateway = gateway;
    s_ipv4_config.dns = dns;
    s_ipv4_config.dhcp_server = dhcp_server;
    s_ipv4_config.lease_seconds = lease_seconds;
}

const net_ipv4_config_t* net_ipv4_config(void) {
    return &s_ipv4_config;
}

int net_ipv4_is_configured(void) {
    return s_ipv4_config.configured;
}

u32 net_ipv4_local_ip(void) {
    return s_ipv4_config.ip;
}

u32 net_ipv4_netmask(void) {
    return s_ipv4_config.netmask;
}

u32 net_ipv4_gateway(void) {
    return s_ipv4_config.gateway;
}

u32 net_ipv4_dns(void) {
    return s_ipv4_config.dns;
}

void net_ipv4_print_config(void) {
    if (!s_ipv4_config.configured) {
        terminal_puts("net: IPv4 unconfigured\n");
        return;
    }

    terminal_puts("net: ip=");
    ipv4_print_ip(s_ipv4_config.ip);
    terminal_puts(" mask=");
    ipv4_print_ip(s_ipv4_config.netmask);
    terminal_puts(" gw=");
    ipv4_print_ip(s_ipv4_config.gateway);
    if (s_ipv4_config.dns != 0u) {
        terminal_puts(" dns=");
        ipv4_print_ip(s_ipv4_config.dns);
    }
    if (s_ipv4_config.lease_seconds != 0u) {
        terminal_puts(" lease=");
        terminal_put_uint(s_ipv4_config.lease_seconds);
        terminal_puts("s");
    }
    terminal_putc('\n');
}
