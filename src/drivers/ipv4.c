#include "ipv4.h"

#include "arp.h"
#include "e1000.h"
#include "../kernel/klib.h"
#include "../kernel/timer.h"
#include "terminal.h"

#define IPV4_ETHERTYPE     0x0800u
#define IPV4_VERSION_IHL   0x45u
#define IPV4_TTL           64u
#define IPV4_PROTO_ICMP    1u
#define IPV4_ECHO_REQUEST  8u
#define IPV4_ECHO_REPLY    0u

#define IPV4_PACKET_SIZE    60u
#define IPV4_PING_ATTEMPTS   3u
#define IPV4_PING_WAIT_TICKS (2u * SMALLOS_TIMER_HZ)

typedef unsigned short u16;

static u16 ipv4_checksum16(const u8* data, u32 len) {
    u32 sum = 0;

    while (len > 1u) {
        sum += (u16)(((u16)data[0] << 8) | (u16)data[1]);
        data += 2;
        len -= 2;
    }

    if (len > 0u) {
        sum += (u16)((u16)data[0] << 8);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return (u16)~sum;
}

static void ipv4_write_u16_be(u8* buf, u32 off, u16 value) {
    buf[off] = (u8)(value >> 8);
    buf[off + 1] = (u8)(value & 0xFFu);
}

static void ipv4_write_u32_be(u8* buf, u32 off, u32 value) {
    buf[off]     = (u8)((value >> 24) & 0xFFu);
    buf[off + 1] = (u8)((value >> 16) & 0xFFu);
    buf[off + 2] = (u8)((value >> 8) & 0xFFu);
    buf[off + 3] = (u8)(value & 0xFFu);
}

static u16 ipv4_read_u16_be(const u8* buf, u32 off) {
    return (u16)(((u16)buf[off] << 8) | (u16)buf[off + 1]);
}

static u32 ipv4_read_u32_be(const u8* buf, u32 off) {
    return ((u32)buf[off] << 24)
         | ((u32)buf[off + 1] << 16)
         | ((u32)buf[off + 2] << 8)
         | (u32)buf[off + 3];
}

void ipv4_print_ip(u32 ip) {
    arp_print_ip(ip);
}

int ipv4_parse_ip(const char* text, u32* out_ip) {
    if (!text || !out_ip) {
        return 0;
    }

    u32 parts[4];
    unsigned int part_index = 0;
    u32 value = 0;
    int saw_digit = 0;

    for (const char* p = text; ; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            value = value * 10u + (u32)(c - '0');
            if (value > 255u) {
                return 0;
            }
            saw_digit = 1;
            continue;
        }

        if (c == '.' || c == '\0') {
            if (!saw_digit || part_index >= 4u) {
                return 0;
            }
            parts[part_index++] = value;
            value = 0;
            saw_digit = 0;
            if (c == '\0') {
                break;
            }
            continue;
        }

        return 0;
    }

    if (part_index != 4u) {
        return 0;
    }

    *out_ip = (parts[0] << 24)
            | (parts[1] << 16)
            | (parts[2] << 8)
            | parts[3];
    return 1;
}

static void ipv4_build_echo_request(u8* frame,
                                    const u8* src_mac,
                                    const u8* dst_mac,
                                    u32 sender_ip,
                                    u32 target_ip,
                                    u16 ident,
                                    u16 seq) {
    static const u8 payload[] = "SmallOS ping";
    u16 total_len = (u16)(20u + 8u + sizeof(payload) - 1u);
    u16 icmp_len = (u16)(8u + sizeof(payload) - 1u);

    k_memset(frame, 0, IPV4_PACKET_SIZE);

    for (unsigned int i = 0; i < 6; i++) {
        frame[i] = dst_mac[i];
        frame[6 + i] = src_mac[i];
    }

    ipv4_write_u16_be(frame, 12, IPV4_ETHERTYPE);

    frame[14] = IPV4_VERSION_IHL;
    frame[15] = 0;
    ipv4_write_u16_be(frame, 16, total_len);
    ipv4_write_u16_be(frame, 18, ident);
    ipv4_write_u16_be(frame, 20, 0x4000u);
    frame[22] = IPV4_TTL;
    frame[23] = IPV4_PROTO_ICMP;
    ipv4_write_u16_be(frame, 24, 0);
    ipv4_write_u32_be(frame, 26, sender_ip);
    ipv4_write_u32_be(frame, 30, target_ip);
    ipv4_write_u16_be(frame, 24, ipv4_checksum16(&frame[14], 20));

    frame[34] = IPV4_ECHO_REQUEST;
    frame[35] = 0;
    ipv4_write_u16_be(frame, 36, 0);
    ipv4_write_u16_be(frame, 38, ident);
    ipv4_write_u16_be(frame, 40, seq);
    k_memcpy(&frame[42], payload, sizeof(payload) - 1u);
    ipv4_write_u16_be(frame, 36, ipv4_checksum16(&frame[34], icmp_len));
}

static int ipv4_parse_echo_reply(const u8* frame,
                                 u32 len,
                                 u32 sender_ip,
                                 u32 target_ip,
                                 u16 ident,
                                 u16 seq) {
    u16 ether_type;
    u8 ihl;
    u32 header_len;
    u16 total_len;
    u16 header_sum;
    u16 icmp_sum;

    if (len < 42u) {
        return 0;
    }

    ether_type = ipv4_read_u16_be(frame, 12);
    if (ether_type != IPV4_ETHERTYPE) {
        return 0;
    }

    ihl = (u8)(frame[14] & 0x0Fu);
    header_len = (u32)ihl * 4u;
    if (ihl < 5u || len < 14u + header_len + 8u) {
        return 0;
    }

    total_len = ipv4_read_u16_be(frame, 16);
    if (total_len + 14u > len) {
        total_len = (u16)(len - 14u);
    }

    if (frame[23] != IPV4_PROTO_ICMP) {
        return 0;
    }
    if (ipv4_read_u32_be(frame, 26) != target_ip) {
        return 0;
    }
    if (ipv4_read_u32_be(frame, 30) != sender_ip) {
        return 0;
    }

    if (frame[14 + header_len] != IPV4_ECHO_REPLY) {
        return 0;
    }

    header_sum = ipv4_checksum16(&frame[14], header_len);
    if (header_sum != 0u) {
        return 0;
    }

    icmp_sum = ipv4_checksum16(&frame[14 + header_len], (u32)(total_len - header_len));
    if (icmp_sum != 0u) {
        return 0;
    }

    if (ipv4_read_u16_be(frame, 14 + header_len + 4u) != ident) {
        return 0;
    }
    if (ipv4_read_u16_be(frame, 14 + header_len + 6u) != seq) {
        return 0;
    }

    return 1;
}

int ipv4_ping_via_gateway(u32 sender_ip, u32 target_ip, u32 gateway_ip) {
    u8 target_mac[6];
    u8 frame[1600];
    u32 len = 0;
    u16 ident = 0x4A50u; /* "JP" for ping bookkeeping */
    u16 seq = 1u;

    if (!arp_resolve(sender_ip, gateway_ip, target_mac)) {
        terminal_puts("ping: arp failed\n");
        return 0;
    }

    for (unsigned int attempt = 0; attempt < IPV4_PING_ATTEMPTS; attempt++) {
        unsigned int deadline = timer_get_ticks() + IPV4_PING_WAIT_TICKS;

        terminal_puts("ping: attempt ");
        terminal_put_uint(attempt + 1u);
        terminal_puts("/");
        terminal_put_uint(IPV4_PING_ATTEMPTS);
        terminal_putc('\n');

        ipv4_build_echo_request(frame, e1000_mac(), target_mac, sender_ip, target_ip, ident, seq);

        if (!e1000_send(frame, IPV4_PACKET_SIZE)) {
            terminal_puts("ping: send failed\n");
            return 0;
        }

        while ((int)(timer_get_ticks() - deadline) < 0) {
            if (!e1000_recv(frame, sizeof(frame), &len)) {
                __asm__ __volatile__("hlt");
                continue;
            }

            if (ipv4_parse_echo_reply(frame, len, sender_ip, target_ip, ident, seq)) {
                terminal_puts("ping: ");
                ipv4_print_ip(target_ip);
                terminal_puts(" reply\n");
                terminal_puts("ping: attempt ");
                terminal_put_uint(attempt + 1u);
                terminal_puts("/");
                terminal_put_uint(IPV4_PING_ATTEMPTS);
                terminal_puts(" ok\n");
                return 1;
            }
        }

        terminal_puts("ping: attempt ");
        terminal_put_uint(attempt + 1u);
        terminal_puts("/");
        terminal_put_uint(IPV4_PING_ATTEMPTS);
        terminal_puts(" timeout\n");
    }

    terminal_puts("ping: timeout\n");
    return 0;
}

int ipv4_ping(u32 sender_ip, u32 target_ip) {
    return ipv4_ping_via_gateway(sender_ip, target_ip, target_ip);
}
