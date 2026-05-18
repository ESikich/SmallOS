#include "dhcp.h"

#include "ipv4.h"
#include "nic.h"
#include "net.h"
#include "../kernel/klib.h"
#include "../kernel/timer.h"
#include "terminal.h"

#define DHCP_ETHERTYPE_IPV4 0x0800u
#define DHCP_IPV4_VERSION_IHL 0x45u
#define DHCP_IPV4_TTL 64u
#define DHCP_IPV4_PROTO_UDP 17u
#define DHCP_CLIENT_PORT 68u
#define DHCP_SERVER_PORT 67u
#define DHCP_BOOTREQUEST 1u
#define DHCP_BOOTREPLY 2u
#define DHCP_HTYPE_ETHERNET 1u
#define DHCP_HLEN_ETHERNET 6u
#define DHCP_MAGIC_COOKIE 0x63825363u
#define DHCP_OPTION_PAD 0u
#define DHCP_OPTION_SUBNET_MASK 1u
#define DHCP_OPTION_ROUTER 3u
#define DHCP_OPTION_DNS 6u
#define DHCP_OPTION_REQUESTED_IP 50u
#define DHCP_OPTION_LEASE_TIME 51u
#define DHCP_OPTION_MESSAGE_TYPE 53u
#define DHCP_OPTION_SERVER_ID 54u
#define DHCP_OPTION_PARAM_REQUEST 55u
#define DHCP_OPTION_END 255u
#define DHCPDISCOVER 1u
#define DHCPOFFER 2u
#define DHCPREQUEST 3u
#define DHCPACK 5u
#define DHCP_BASE_LEN 240u
#define DHCP_DISCOVER_LEN 286u
#define DHCP_REQUEST_LEN 298u
#define DHCP_FRAME_MAX 590u
#define DHCP_TIMEOUT_TICKS (4u * SMALLOS_TIMER_HZ)
#define DHCP_ATTEMPTS 8u

typedef unsigned short u16;

static int s_waiting;
static u32 s_xid;
static u32 s_offered_ip;
static u32 s_server_ip;
static u32 s_ack_ip;
static u32 s_netmask;
static u32 s_gateway;
static u32 s_dns;
static u32 s_lease_seconds;
static u8 s_message_type;
static volatile int s_verbose = 1;
static dhcp_log_hook_t s_log_hook = 0;

void dhcp_set_verbose(int verbose) {
    s_verbose = verbose ? 1 : 0;
}

void dhcp_set_log_hook(dhcp_log_hook_t hook) {
    s_log_hook = hook;
}

static void dhcp_log_text(const char* text) {
    if (s_verbose) {
        terminal_puts(text);
    } else if (s_log_hook) {
        s_log_hook(text, 0, 0);
    }
}

static void dhcp_log_ip_line(const char* text, u32 ip) {
    if (s_verbose) {
        terminal_puts(text);
        ipv4_print_ip(ip);
        terminal_putc('\n');
    } else if (s_log_hook) {
        s_log_hook(text, ip, 1);
    }
}

static void write_u16_be(u8* buf, u32 off, u16 value) {
    buf[off] = (u8)(value >> 8);
    buf[off + 1u] = (u8)(value & 0xFFu);
}

static void write_u32_be(u8* buf, u32 off, u32 value) {
    buf[off] = (u8)((value >> 24) & 0xFFu);
    buf[off + 1u] = (u8)((value >> 16) & 0xFFu);
    buf[off + 2u] = (u8)((value >> 8) & 0xFFu);
    buf[off + 3u] = (u8)(value & 0xFFu);
}

static u16 read_u16_be(const u8* buf, u32 off) {
    return (u16)(((u16)buf[off] << 8) | (u16)buf[off + 1u]);
}

static u32 read_u32_be(const u8* buf, u32 off) {
    return ((u32)buf[off] << 24)
         | ((u32)buf[off + 1u] << 16)
         | ((u32)buf[off + 2u] << 8)
         | (u32)buf[off + 3u];
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

static void build_ipv4_udp_header(u8* frame, u32 src_ip, u32 udp_len, u16 ip_id) {
    u32 ip_off = 14u;
    u32 udp_off = ip_off + 20u;
    u16 total_len = (u16)(20u + udp_len);

    write_u16_be(frame, 12u, DHCP_ETHERTYPE_IPV4);
    frame[ip_off] = DHCP_IPV4_VERSION_IHL;
    frame[ip_off + 1u] = 0;
    write_u16_be(frame, ip_off + 2u, total_len);
    write_u16_be(frame, ip_off + 4u, ip_id);
    write_u16_be(frame, ip_off + 6u, 0);
    frame[ip_off + 8u] = DHCP_IPV4_TTL;
    frame[ip_off + 9u] = DHCP_IPV4_PROTO_UDP;
    write_u16_be(frame, ip_off + 10u, 0);
    write_u32_be(frame, ip_off + 12u, src_ip);
    write_u32_be(frame, ip_off + 16u, 0xFFFFFFFFu);
    write_u16_be(frame, ip_off + 10u, checksum16(&frame[ip_off], 20u));

    write_u16_be(frame, udp_off, DHCP_CLIENT_PORT);
    write_u16_be(frame, udp_off + 2u, DHCP_SERVER_PORT);
    write_u16_be(frame, udp_off + 4u, (u16)udp_len);
    write_u16_be(frame, udp_off + 6u, 0);
}

static void build_bootp_base(u8* frame, u32 xid, u32 requested_ip) {
    const u8* mac = nic_mac();
    u32 bootp_off = 14u + 20u + 8u;

    for (unsigned int i = 0; i < 6u; i++) {
        frame[i] = 0xFFu;
        frame[6u + i] = mac[i];
    }

    frame[bootp_off] = DHCP_BOOTREQUEST;
    frame[bootp_off + 1u] = DHCP_HTYPE_ETHERNET;
    frame[bootp_off + 2u] = DHCP_HLEN_ETHERNET;
    frame[bootp_off + 3u] = 0;
    write_u32_be(frame, bootp_off + 4u, xid);
    write_u16_be(frame, bootp_off + 8u, 0);
    write_u16_be(frame, bootp_off + 10u, 0x8000u);
    write_u32_be(frame, bootp_off + 12u, 0);
    write_u32_be(frame, bootp_off + 16u, requested_ip);
    write_u32_be(frame, bootp_off + 20u, 0);
    write_u32_be(frame, bootp_off + 24u, 0);
    for (unsigned int i = 0; i < 6u; i++) {
        frame[bootp_off + 28u + i] = mac[i];
    }
    write_u32_be(frame, bootp_off + 236u, DHCP_MAGIC_COOKIE);
}

static u32 add_option_u8(u8* frame, u32 off, u8 code, u8 value) {
    frame[off++] = code;
    frame[off++] = 1u;
    frame[off++] = value;
    return off;
}

static u32 add_option_u32(u8* frame, u32 off, u8 code, u32 value) {
    frame[off++] = code;
    frame[off++] = 4u;
    write_u32_be(frame, off, value);
    return off + 4u;
}

static u32 add_param_request(u8* frame, u32 off) {
    frame[off++] = DHCP_OPTION_PARAM_REQUEST;
    frame[off++] = 4u;
    frame[off++] = DHCP_OPTION_SUBNET_MASK;
    frame[off++] = DHCP_OPTION_ROUTER;
    frame[off++] = DHCP_OPTION_DNS;
    frame[off++] = DHCP_OPTION_LEASE_TIME;
    return off;
}

static u32 build_discover(u8* frame, u32 xid) {
    u32 udp_len = 8u + DHCP_DISCOVER_LEN;
    u32 off = 14u + 20u + 8u + DHCP_BASE_LEN;

    k_memset(frame, 0, DHCP_FRAME_MAX);
    build_bootp_base(frame, xid, 0);
    build_ipv4_udp_header(frame, 0, udp_len, 0x4448u);
    off = add_option_u8(frame, off, DHCP_OPTION_MESSAGE_TYPE, DHCPDISCOVER);
    off = add_param_request(frame, off);
    frame[off++] = DHCP_OPTION_END;
    return 14u + 20u + udp_len;
}

static u32 build_request(u8* frame, u32 xid, u32 requested_ip, u32 server_ip) {
    u32 udp_len = 8u + DHCP_REQUEST_LEN;
    u32 off = 14u + 20u + 8u + DHCP_BASE_LEN;

    k_memset(frame, 0, DHCP_FRAME_MAX);
    build_bootp_base(frame, xid, requested_ip);
    build_ipv4_udp_header(frame, 0, udp_len, 0x4452u);
    off = add_option_u8(frame, off, DHCP_OPTION_MESSAGE_TYPE, DHCPREQUEST);
    off = add_option_u32(frame, off, DHCP_OPTION_REQUESTED_IP, requested_ip);
    off = add_option_u32(frame, off, DHCP_OPTION_SERVER_ID, server_ip);
    off = add_param_request(frame, off);
    frame[off++] = DHCP_OPTION_END;
    return 14u + 20u + udp_len;
}

static void reset_reply_state(void) {
    s_message_type = 0;
    s_offered_ip = 0;
    s_ack_ip = 0;
    s_server_ip = 0;
    s_netmask = 0;
    s_gateway = 0;
    s_dns = 0;
    s_lease_seconds = 0;
}

static int wait_for_message(u8 message_type) {
    unsigned int deadline = timer_get_ticks() + DHCP_TIMEOUT_TICKS;

    while ((int)(timer_get_ticks() - deadline) < 0) {
        if (s_message_type == message_type) {
            return 1;
        }
        if (!net_poll_once()) {
            __asm__ __volatile__("sti; hlt; cli");
        }
    }
    return 0;
}

int dhcp_handle_ipv4_frame(const u8* frame, u32 len) {
    u32 ip_off = 14u;
    u32 header_len;
    u32 udp_off;
    u32 bootp_off;
    u32 options_off;
    u32 options_end;
    u16 udp_len;
    u8 msg_type = 0;
    u32 offered_ip;
    u32 server_ip = 0;
    u32 netmask = 0;
    u32 gateway = 0;
    u32 dns = 0;
    u32 lease = 0;

    if (!s_waiting || !frame || len < 14u + 20u + 8u + DHCP_BASE_LEN) return 0;
    if (read_u16_be(frame, 12u) != DHCP_ETHERTYPE_IPV4) return 0;
    if ((frame[ip_off] >> 4) != 4u || (frame[ip_off] & 0x0Fu) < 5u) return 0;

    header_len = (u32)(frame[ip_off] & 0x0Fu) * 4u;
    if (len < ip_off + header_len + 8u + DHCP_BASE_LEN) return 0;
    if (frame[ip_off + 9u] != DHCP_IPV4_PROTO_UDP) return 0;

    udp_off = ip_off + header_len;
    if (read_u16_be(frame, udp_off) != DHCP_SERVER_PORT) return 0;
    if (read_u16_be(frame, udp_off + 2u) != DHCP_CLIENT_PORT) return 0;
    udp_len = read_u16_be(frame, udp_off + 4u);
    if (udp_len < 8u + DHCP_BASE_LEN || len < udp_off + udp_len) return 0;

    bootp_off = udp_off + 8u;
    if (frame[bootp_off] != DHCP_BOOTREPLY) return 0;
    if (frame[bootp_off + 1u] != DHCP_HTYPE_ETHERNET) return 0;
    if (frame[bootp_off + 2u] != DHCP_HLEN_ETHERNET) return 0;
    if (read_u32_be(frame, bootp_off + 4u) != s_xid) return 0;
    if (read_u32_be(frame, bootp_off + 236u) != DHCP_MAGIC_COOKIE) return 0;

    offered_ip = read_u32_be(frame, bootp_off + 16u);
    options_off = bootp_off + DHCP_BASE_LEN;
    options_end = udp_off + udp_len;

    while (options_off < options_end) {
        u8 code = frame[options_off++];
        u8 opt_len;

        if (code == DHCP_OPTION_PAD) continue;
        if (code == DHCP_OPTION_END) break;
        if (options_off >= options_end) break;
        opt_len = frame[options_off++];
        if (options_off + opt_len > options_end) break;

        if (code == DHCP_OPTION_MESSAGE_TYPE && opt_len >= 1u) {
            msg_type = frame[options_off];
        } else if (code == DHCP_OPTION_SERVER_ID && opt_len >= 4u) {
            server_ip = read_u32_be(frame, options_off);
        } else if (code == DHCP_OPTION_SUBNET_MASK && opt_len >= 4u) {
            netmask = read_u32_be(frame, options_off);
        } else if (code == DHCP_OPTION_ROUTER && opt_len >= 4u) {
            gateway = read_u32_be(frame, options_off);
        } else if (code == DHCP_OPTION_DNS && opt_len >= 4u) {
            dns = read_u32_be(frame, options_off);
        } else if (code == DHCP_OPTION_LEASE_TIME && opt_len >= 4u) {
            lease = read_u32_be(frame, options_off);
        }

        options_off += opt_len;
    }

    if (msg_type != DHCPOFFER && msg_type != DHCPACK) return 0;
    if (offered_ip == 0u) return 0;

    s_message_type = msg_type;
    if (msg_type == DHCPOFFER) {
        s_offered_ip = offered_ip;
    } else {
        s_ack_ip = offered_ip;
    }
    s_server_ip = server_ip;
    s_netmask = netmask ? netmask : 0xFFFFFF00u;
    s_gateway = gateway;
    s_dns = dns;
    s_lease_seconds = lease;
    return 1;
}

int dhcp_configure(void) {
    u8 frame[DHCP_FRAME_MAX];
    u32 frame_len;

    net_ipv4_clear_config();
    s_xid = 0x534D0000u ^ timer_get_ticks();

    for (unsigned int attempt = 0; attempt < DHCP_ATTEMPTS; attempt++) {
        dhcp_log_text("dhcp: discover\n");
        reset_reply_state();
        s_waiting = 1;
        frame_len = build_discover(frame, s_xid);
        if (!nic_send(frame, frame_len)) {
            s_waiting = 0;
            return 0;
        }
        if (!wait_for_message(DHCPOFFER) || s_offered_ip == 0u || s_server_ip == 0u) {
            s_waiting = 0;
            continue;
        }

        dhcp_log_ip_line("dhcp: offer ", s_offered_ip);

        s_message_type = 0;
        frame_len = build_request(frame, s_xid, s_offered_ip, s_server_ip);
        if (!nic_send(frame, frame_len)) {
            s_waiting = 0;
            return 0;
        }
        if (!wait_for_message(DHCPACK) || s_ack_ip == 0u) {
            s_waiting = 0;
            continue;
        }

        s_waiting = 0;
        net_ipv4_configure(s_ack_ip, s_netmask, s_gateway, s_dns, s_server_ip, s_lease_seconds);
        dhcp_log_ip_line("dhcp: bound ", s_ack_ip);
        if (s_verbose) {
            net_ipv4_print_config();
        } else if (s_log_hook) {
            s_log_hook("net: ip=", s_ack_ip, 1);
        }
        return 1;
    }

    s_waiting = 0;
    return 0;
}
