#include "tcp.h"

#include "e1000.h"
#include "ipv4.h"
#include "../kernel/klib.h"
#include "../kernel/process.h"
#include "../kernel/scheduler.h"
#include "../kernel/timer.h"
#include "terminal.h"

#define TCP_ETHERTYPE        0x0800u
#define TCP_IPV4_VERSION_IHL 0x45u
#define TCP_IPV4_TTL         64u
#define TCP_IPV4_PROTO       6u
#define TCP_LISTEN_PORT      2323u
#define TCP_LOCAL_IP         0x0A00020Fu  /* 10.0.2.15 */

#define TCP_SYN              0x02u
#define TCP_RST              0x04u
#define TCP_PSH              0x08u
#define TCP_ACK              0x10u
#define TCP_FIN              0x01u

#define TCP_STATE_CLOSED     0
#define TCP_STATE_SYN_RCVD    1
#define TCP_STATE_ESTABLISHED 2

#define TCP_RETRY_TICKS      100u
#define TCP_IDLE_TICKS      1200u
#define TCP_MAX_RETRIES        3u
#define TCP_MAX_FRAME       1600u
#define TCP_MAX_PAYLOAD    (TCP_MAX_FRAME - 14u - 20u - 20u)

typedef unsigned short u16;

typedef struct {
    int state;
    u8 remote_mac[6];
    u32 remote_ip;
    u16 remote_port;
    u32 remote_seq_next;
    u32 local_seq_next;
    u32 last_activity;
    u32 retransmit_at;
    unsigned int retries;
} tcp_conn_t;

static tcp_conn_t s_conn;
static u16 s_ip_id = 1u;
static u8 s_rx_frame[TCP_MAX_FRAME];
static u8 s_tx_frame[TCP_MAX_FRAME];
static u8 s_checksum_scratch[12u + TCP_MAX_FRAME];

static u16 tcp_read_u16_be(const u8* buf, u32 off) {
    return (u16)(((u16)buf[off] << 8) | (u16)buf[off + 1]);
}

static u32 tcp_read_u32_be(const u8* buf, u32 off) {
    return ((u32)buf[off] << 24)
         | ((u32)buf[off + 1] << 16)
         | ((u32)buf[off + 2] << 8)
         | (u32)buf[off + 3];
}

static void tcp_write_u16_be(u8* buf, u32 off, u16 value) {
    buf[off] = (u8)(value >> 8);
    buf[off + 1] = (u8)(value & 0xFFu);
}

static void tcp_write_u32_be(u8* buf, u32 off, u32 value) {
    buf[off] = (u8)((value >> 24) & 0xFFu);
    buf[off + 1] = (u8)((value >> 16) & 0xFFu);
    buf[off + 2] = (u8)((value >> 8) & 0xFFu);
    buf[off + 3] = (u8)(value & 0xFFu);
}

static u16 tcp_checksum16(const u8* data, u32 len) {
    u32 sum = 0;

    while (len > 1u) {
        sum += (u16)(((u16)data[0] << 8) | (u16)data[1]);
        data += 2;
        len -= 2u;
    }

    if (len > 0u) {
        sum += (u16)((u16)data[0] << 8);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return (u16)~sum;
}

static u16 tcp_segment_checksum(u32 src_ip,
                                u32 dst_ip,
                                const u8* tcp_segment,
                                u32 tcp_len);

static u32 tcp_ipv4_header_len(const u8* frame, u32 len) {
    u8 ihl = (u8)(frame[14] & 0x0Fu);
    u32 header_len = (u32)ihl * 4u;
    if (ihl < 5u || len < 14u + header_len) {
        return 0u;
    }
    return header_len;
}

static int tcp_frame_matches_ipv4(const u8* frame, u32 len) {
    if (len < 54u) {
        return 0;
    }
    if (tcp_read_u16_be(frame, 12) != TCP_ETHERTYPE) {
        return 0;
    }
    if ((frame[14] >> 4) != 4u) {
        return 0;
    }
    return 1;
}

static int tcp_validate_ipv4(const u8* frame, u32 len, u32* out_ip_header_len, u32* out_total_len) {
    u32 header_len;
    u16 total_len;
    u16 header_sum;

    if (!tcp_frame_matches_ipv4(frame, len)) {
        return 0;
    }

    header_len = tcp_ipv4_header_len(frame, len);
    if (header_len == 0u) {
        return 0;
    }

    total_len = tcp_read_u16_be(frame, 16);
    if ((u32)total_len + 14u > len) {
        return 0;
    }
    if ((u32)total_len < header_len) {
        return 0;
    }

    if (frame[23] != TCP_IPV4_PROTO) {
        return 0;
    }
    if (tcp_read_u32_be(frame, 30) != TCP_LOCAL_IP) {
        return 0;
    }

    header_sum = tcp_checksum16(&frame[14], header_len);
    if (header_sum != 0u) {
        return 0;
    }

    if (out_ip_header_len) {
        *out_ip_header_len = header_len;
    }
    if (out_total_len) {
        *out_total_len = (u32)total_len;
    }
    return 1;
}

static int tcp_validate_tcp_segment(const u8* frame,
                                    u32 ip_header_len,
                                    u32 total_len) {
    u32 tcp_off = 14u + ip_header_len;
    u32 tcp_len = total_len - ip_header_len;

    if (tcp_len > TCP_MAX_FRAME) {
        return 0;
    }

    return tcp_segment_checksum(tcp_read_u32_be(frame, 26),
                                tcp_read_u32_be(frame, 30),
                                &frame[tcp_off],
                                tcp_len) == 0u;
}

static u16 tcp_segment_checksum(u32 src_ip,
                                u32 dst_ip,
                                const u8* tcp_segment,
                                u32 tcp_len) {
    u16 sum;

    if (tcp_len > TCP_MAX_FRAME) {
        return 0u;
    }

    tcp_write_u32_be(s_checksum_scratch, 0, src_ip);
    tcp_write_u32_be(s_checksum_scratch, 4, dst_ip);
    s_checksum_scratch[8] = 0;
    s_checksum_scratch[9] = TCP_IPV4_PROTO;
    tcp_write_u16_be(s_checksum_scratch, 10, (u16)tcp_len);

    for (u32 i = 0; i < tcp_len; i++) {
        s_checksum_scratch[12u + i] = tcp_segment[i];
    }

    sum = tcp_checksum16(s_checksum_scratch, 12u + tcp_len);
    return sum;
}

static void tcp_build_ipv4_header(u8* frame,
                                  u32 payload_len,
                                  u32 src_ip,
                                  u32 dst_ip) {
    u16 total_len = (u16)(20u + payload_len);

    frame[14] = TCP_IPV4_VERSION_IHL;
    frame[15] = 0;
    tcp_write_u16_be(frame, 16, total_len);
    tcp_write_u16_be(frame, 18, s_ip_id++);
    tcp_write_u16_be(frame, 20, 0x4000u);
    frame[22] = TCP_IPV4_TTL;
    frame[23] = TCP_IPV4_PROTO;
    tcp_write_u16_be(frame, 24, 0);
    tcp_write_u32_be(frame, 26, src_ip);
    tcp_write_u32_be(frame, 30, dst_ip);
    tcp_write_u16_be(frame, 24, tcp_checksum16(&frame[14], 20u));
}

static u32 tcp_build_frame(u8* frame,
                           const u8* dst_mac,
                           u32 src_ip,
                           u32 dst_ip,
                           u16 src_port,
                           u16 dst_port,
                           u32 seq,
                           u32 ack,
                           u8 flags,
                           const u8* payload,
                           u32 payload_len) {
    u32 tcp_len = 20u + payload_len;
    u32 frame_len = 14u + 20u + tcp_len;

    k_memset(frame, 0, frame_len < 60u ? 60u : frame_len);

    for (unsigned int i = 0; i < 6; i++) {
        frame[i] = dst_mac[i];
        frame[6 + i] = e1000_mac()[i];
    }
    tcp_write_u16_be(frame, 12, TCP_ETHERTYPE);

    tcp_build_ipv4_header(frame, tcp_len, src_ip, dst_ip);

    {
        u32 tcp_off = 34u;
        tcp_write_u16_be(frame, tcp_off + 0u, src_port);
        tcp_write_u16_be(frame, tcp_off + 2u, dst_port);
        tcp_write_u32_be(frame, tcp_off + 4u, seq);
        tcp_write_u32_be(frame, tcp_off + 8u, ack);
        frame[tcp_off + 12u] = 5u << 4;
        frame[tcp_off + 13u] = flags;
        tcp_write_u16_be(frame, tcp_off + 14u, 0x2000u);
        tcp_write_u16_be(frame, tcp_off + 16u, 0);
        tcp_write_u16_be(frame, tcp_off + 18u, 0);

        if (payload_len > 0u && payload) {
            k_memcpy(&frame[tcp_off + 20u], payload, payload_len);
        }

        tcp_write_u16_be(frame, tcp_off + 16u,
                         tcp_segment_checksum(src_ip, dst_ip,
                                              &frame[tcp_off], tcp_len));
    }

    if (frame_len < 60u) {
        frame_len = 60u;
    }
    return frame_len;
}

static void tcp_reset_connection(void) {
    k_memset(&s_conn, 0, sizeof(s_conn));
    s_conn.state = TCP_STATE_CLOSED;
}

static void tcp_send_segment(u32 src_ip,
                             u32 dst_ip,
                             const u8* dst_mac,
                             u16 src_port,
                             u16 dst_port,
                             u32 seq,
                             u32 ack,
                             u8 flags,
                             const u8* payload,
                             u32 payload_len) {
    u32 frame_len = tcp_build_frame(s_tx_frame, dst_mac, src_ip, dst_ip,
                                    src_port, dst_port, seq, ack,
                                    flags, payload, payload_len);
    if (!e1000_send(s_tx_frame, frame_len)) {
        terminal_puts("tcp: send failed\n");
    }
}

static void tcp_accept_syn(const u8* frame,
                           u32 ip_header_len,
                           u32 total_len) {
    u32 tcp_off = 14u + ip_header_len;
    u16 src_port = tcp_read_u16_be(frame, tcp_off + 0u);
    u16 dst_port = tcp_read_u16_be(frame, tcp_off + 2u);
    u32 seq = tcp_read_u32_be(frame, tcp_off + 4u);
    u32 payload_len;

    if (total_len < ip_header_len + 20u) {
        return;
    }
    if (dst_port != TCP_LISTEN_PORT) {
        return;
    }
    if ((frame[tcp_off + 13u] & TCP_SYN) == 0u) {
        return;
    }

    payload_len = total_len - ip_header_len - 20u;
    if (payload_len > TCP_MAX_PAYLOAD) {
        return;
    }
    if (payload_len != 0u) {
        return;
    }

    if (s_conn.state != TCP_STATE_CLOSED) {
        return;
    }

    for (unsigned int i = 0; i < 6; i++) {
        s_conn.remote_mac[i] = frame[6 + i];
    }
    s_conn.remote_ip = tcp_read_u32_be(frame, 26);
    s_conn.remote_port = src_port;
    s_conn.remote_seq_next = seq + 1u;
    s_conn.local_seq_next = 0x534D414Cu; /* "SMAL" */
    s_conn.state = TCP_STATE_SYN_RCVD;
    s_conn.last_activity = timer_get_ticks();
    s_conn.retransmit_at = s_conn.last_activity + TCP_RETRY_TICKS;
    s_conn.retries = 0;

    terminal_puts("tcp: SYN from ");
    ipv4_print_ip(s_conn.remote_ip);
    terminal_puts("\n");

    tcp_send_segment(TCP_LOCAL_IP,
                     s_conn.remote_ip,
                     s_conn.remote_mac,
                     TCP_LISTEN_PORT,
                     s_conn.remote_port,
                     s_conn.local_seq_next,
                     s_conn.remote_seq_next,
                     (u8)(TCP_SYN | TCP_ACK),
                     0,
                     0);
}

static void tcp_echo_payload(const u8* frame,
                             u32 ip_header_len,
                             u32 total_len) {
    u32 tcp_off = 14u + ip_header_len;
    u32 tcp_len = total_len - ip_header_len;
    u8 flags = frame[tcp_off + 13u];
    u8 data_offset = (u8)(frame[tcp_off + 12u] >> 4);
    u32 header_len = (u32)data_offset * 4u;
    u32 payload_off = tcp_off + header_len;
    u32 payload_len = tcp_len - header_len;
    u32 seq = tcp_read_u32_be(frame, tcp_off + 4u);
    u32 ack = tcp_read_u32_be(frame, tcp_off + 8u);
    u32 src_ip = tcp_read_u32_be(frame, 26);

    if (total_len < ip_header_len + 20u) {
        return;
    }
    if (!tcp_validate_tcp_segment(frame, ip_header_len, total_len)) {
        return;
    }
    if (tcp_len < header_len) {
        return;
    }

    if (s_conn.state == TCP_STATE_CLOSED) {
        tcp_accept_syn(frame, ip_header_len, total_len);
        return;
    }

    if (s_conn.remote_ip != src_ip ||
        s_conn.remote_port != tcp_read_u16_be(frame, tcp_off + 0u)) {
        return;
    }

    if (flags & TCP_RST) {
        terminal_puts("tcp: reset\n");
        tcp_reset_connection();
        return;
    }

    if (s_conn.state == TCP_STATE_SYN_RCVD) {
        if ((flags & TCP_ACK) == 0u) {
            return;
        }
        if (ack != s_conn.local_seq_next + 1u) {
            return;
        }
        s_conn.state = TCP_STATE_ESTABLISHED;
        s_conn.local_seq_next += 1u;
        s_conn.last_activity = timer_get_ticks();
        terminal_puts("tcp: established\n");

        if (payload_len == 0u) {
            return;
        }
    }

    if (payload_len > TCP_MAX_PAYLOAD) {
        return;
    }

    if (s_conn.state != TCP_STATE_ESTABLISHED) {
        return;
    }

    if (seq != s_conn.remote_seq_next) {
        return;
    }

    s_conn.last_activity = timer_get_ticks();
    s_conn.remote_seq_next += payload_len;

    if (flags & TCP_FIN) {
        s_conn.remote_seq_next += 1u;
    }

    if (payload_len > 0u) {
        tcp_send_segment(TCP_LOCAL_IP,
                         s_conn.remote_ip,
                         s_conn.remote_mac,
                         TCP_LISTEN_PORT,
                         s_conn.remote_port,
                         s_conn.local_seq_next,
                         s_conn.remote_seq_next,
                         (u8)(TCP_ACK | TCP_PSH),
                         &frame[payload_off],
                         payload_len);
        s_conn.local_seq_next += payload_len;
    } else {
        tcp_send_segment(TCP_LOCAL_IP,
                         s_conn.remote_ip,
                         s_conn.remote_mac,
                         TCP_LISTEN_PORT,
                         s_conn.remote_port,
                         s_conn.local_seq_next,
                         s_conn.remote_seq_next,
                         TCP_ACK,
                         0,
                         0);
    }

    if (flags & TCP_FIN) {
        tcp_send_segment(TCP_LOCAL_IP,
                         s_conn.remote_ip,
                         s_conn.remote_mac,
                         TCP_LISTEN_PORT,
                         s_conn.remote_port,
                         s_conn.local_seq_next,
                         s_conn.remote_seq_next,
                         (u8)(TCP_FIN | TCP_ACK),
                         0,
                         0);
        s_conn.local_seq_next += 1u;
        terminal_puts("tcp: close\n");
        tcp_reset_connection();
    }
}

static void tcp_maybe_retransmit(void) {
    u32 now = timer_get_ticks();

    if (s_conn.state == TCP_STATE_SYN_RCVD) {
        if (now < s_conn.retransmit_at) {
            return;
        }
        if (s_conn.retries >= TCP_MAX_RETRIES) {
            terminal_puts("tcp: handshake timeout\n");
            tcp_reset_connection();
            return;
        }

        s_conn.retries++;
        s_conn.retransmit_at = now + TCP_RETRY_TICKS;
        terminal_puts("tcp: retransmit SYN-ACK\n");
        tcp_send_segment(TCP_LOCAL_IP,
                         s_conn.remote_ip,
                         s_conn.remote_mac,
                         TCP_LISTEN_PORT,
                         s_conn.remote_port,
                         s_conn.local_seq_next,
                         s_conn.remote_seq_next,
                         (u8)(TCP_SYN | TCP_ACK),
                         0,
                         0);
        return;
    }

    if (s_conn.state == TCP_STATE_ESTABLISHED) {
        if (now - s_conn.last_activity > TCP_IDLE_TICKS) {
            terminal_puts("tcp: idle timeout\n");
            tcp_send_segment(TCP_LOCAL_IP,
                             s_conn.remote_ip,
                             s_conn.remote_mac,
                             TCP_LISTEN_PORT,
                             s_conn.remote_port,
                             s_conn.local_seq_next,
                             s_conn.remote_seq_next,
                             (u8)(TCP_FIN | TCP_ACK),
                             0,
                             0);
            tcp_reset_connection();
        }
    }
}

static void tcp_service_main(void) {
    u32 len = 0;

    terminal_puts("tcp: listener on ");
    ipv4_print_ip(TCP_LOCAL_IP);
    terminal_puts(":");
    terminal_put_uint(TCP_LISTEN_PORT);
    terminal_putc('\n');

    for (;;) {
        while (e1000_recv(s_rx_frame, sizeof(s_rx_frame), &len)) {
            u32 ip_header_len = 0;
            u32 total_len = 0;

            if (!tcp_validate_ipv4(s_rx_frame, len, &ip_header_len, &total_len)) {
                continue;
            }

            tcp_echo_payload(s_rx_frame, ip_header_len, total_len);
        }

        tcp_maybe_retransmit();
        __asm__ __volatile__("sti; hlt");
    }
}

void tcp_init(void) {
    process_t* proc = process_create_kernel_task("tcp", tcp_service_main);
    if (!proc) {
        terminal_puts("tcp: failed to create task\n");
        return;
    }

    tcp_reset_connection();

    if (!sched_enqueue(proc)) {
        terminal_puts("tcp: failed to enqueue task\n");
        process_destroy(proc);
    }
}
