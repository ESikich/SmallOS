#include "ntp.h"

#include "arp.h"
#include "e1000.h"
#include "net.h"
#include "../kernel/klib.h"
#include "../kernel/timer.h"

#define NTP_ETHERTYPE_IPV4 0x0800u
#define NTP_IPV4_VERSION_IHL 0x45u
#define NTP_IPV4_TTL 64u
#define NTP_IPV4_PROTO_UDP 17u
#define NTP_LOCAL_IP 0x0A00020Fu
#define NTP_GATEWAY_IP 0x0A000202u
#define NTP_LOCAL_PORT 49123u
#define NTP_SERVER_PORT 123u
#define NTP_PACKET_LEN 48u
#define NTP_UNIX_EPOCH_DELTA 2208988800u
#define NTP_TIMEOUT_TICKS (5u * SMALLOS_TIMER_HZ)

static int s_waiting;
static int s_got_reply;
static u32 s_server_ip;
static u32 s_unix_time;

static void write_u16_be(u8* buf, u32 off, u16 value) {
    buf[off] = (u8)(value >> 8);
    buf[off + 1] = (u8)(value & 0xFFu);
}

static void write_u32_be(u8* buf, u32 off, u32 value) {
    buf[off] = (u8)((value >> 24) & 0xFFu);
    buf[off + 1] = (u8)((value >> 16) & 0xFFu);
    buf[off + 2] = (u8)((value >> 8) & 0xFFu);
    buf[off + 3] = (u8)(value & 0xFFu);
}

static u16 read_u16_be(const u8* buf, u32 off) {
    return (u16)(((u16)buf[off] << 8) | (u16)buf[off + 1]);
}

static u32 read_u32_be(const u8* buf, u32 off) {
    return ((u32)buf[off] << 24)
         | ((u32)buf[off + 1] << 16)
         | ((u32)buf[off + 2] << 8)
         | (u32)buf[off + 3];
}

static u16 checksum16(const u8* data, u32 len) {
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

static void build_request(u8* frame, const u8* dst_mac, u32 server_ip) {
    const u8* src_mac = e1000_mac();
    u32 ip_off = 14u;
    u32 udp_off = ip_off + 20u;
    u32 ntp_off = udp_off + 8u;
    u16 total_len = (u16)(20u + 8u + NTP_PACKET_LEN);

    k_memset(frame, 0, 14u + 20u + 8u + NTP_PACKET_LEN);
    for (unsigned int i = 0; i < 6; i++) {
        frame[i] = dst_mac[i];
        frame[6u + i] = src_mac[i];
    }
    write_u16_be(frame, 12u, NTP_ETHERTYPE_IPV4);

    frame[ip_off] = NTP_IPV4_VERSION_IHL;
    write_u16_be(frame, ip_off + 2u, total_len);
    write_u16_be(frame, ip_off + 4u, 0x4E54u);
    write_u16_be(frame, ip_off + 6u, 0x4000u);
    frame[ip_off + 8u] = NTP_IPV4_TTL;
    frame[ip_off + 9u] = NTP_IPV4_PROTO_UDP;
    write_u32_be(frame, ip_off + 12u, NTP_LOCAL_IP);
    write_u32_be(frame, ip_off + 16u, server_ip);
    write_u16_be(frame, ip_off + 10u, checksum16(&frame[ip_off], 20u));

    write_u16_be(frame, udp_off, NTP_LOCAL_PORT);
    write_u16_be(frame, udp_off + 2u, NTP_SERVER_PORT);
    write_u16_be(frame, udp_off + 4u, (u16)(8u + NTP_PACKET_LEN));
    write_u16_be(frame, udp_off + 6u, 0u);

    frame[ntp_off] = 0x1Bu;
}

int ntp_handle_ipv4_frame(const u8* frame, u32 len) {
    u32 ip_off = 14u;
    u32 header_len;
    u32 udp_off;
    u32 ntp_off;
    u16 udp_len;
    u32 ntp_seconds;

    if (!s_waiting || !frame || len < 14u + 20u + 8u + NTP_PACKET_LEN) {
        return 0;
    }
    if (read_u16_be(frame, 12u) != NTP_ETHERTYPE_IPV4) return 0;
    if ((frame[ip_off] >> 4) != 4u || (frame[ip_off] & 0x0Fu) < 5u) return 0;
    header_len = (u32)(frame[ip_off] & 0x0Fu) * 4u;
    if (len < ip_off + header_len + 8u + NTP_PACKET_LEN) return 0;
    if (frame[ip_off + 9u] != NTP_IPV4_PROTO_UDP) return 0;
    if (read_u32_be(frame, ip_off + 12u) != s_server_ip) return 0;
    if (read_u32_be(frame, ip_off + 16u) != NTP_LOCAL_IP) return 0;

    udp_off = ip_off + header_len;
    if (read_u16_be(frame, udp_off) != NTP_SERVER_PORT) return 0;
    if (read_u16_be(frame, udp_off + 2u) != NTP_LOCAL_PORT) return 0;
    udp_len = read_u16_be(frame, udp_off + 4u);
    if (udp_len < 8u + NTP_PACKET_LEN || len < udp_off + udp_len) return 0;

    ntp_off = udp_off + 8u;
    ntp_seconds = read_u32_be(frame, ntp_off + 40u);
    if (ntp_seconds < NTP_UNIX_EPOCH_DELTA) return 0;
    s_unix_time = ntp_seconds - NTP_UNIX_EPOCH_DELTA;
    s_got_reply = 1;
    return 1;
}

int ntp_sync(u32 server_ip, u32* out_unix_time) {
    u8 gateway_mac[6];
    u8 frame[14u + 20u + 8u + NTP_PACKET_LEN];
    unsigned int deadline;

    if (server_ip == 0u) return 0;
    if (!arp_resolve(NTP_LOCAL_IP, NTP_GATEWAY_IP, gateway_mac)) {
        return 0;
    }

    build_request(frame, gateway_mac, server_ip);
    s_waiting = 1;
    s_got_reply = 0;
    s_server_ip = server_ip;
    s_unix_time = 0;

    if (!e1000_send(frame, sizeof(frame))) {
        s_waiting = 0;
        return 0;
    }

    deadline = timer_get_ticks() + NTP_TIMEOUT_TICKS;
    while ((int)(timer_get_ticks() - deadline) < 0) {
        if (s_got_reply) {
            s_waiting = 0;
            if (out_unix_time) *out_unix_time = s_unix_time;
            return 1;
        }
        if (!net_poll_once()) {
            __asm__ __volatile__("hlt");
        }
    }

    s_waiting = 0;
    return 0;
}
