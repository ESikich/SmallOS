#include "tcp.h"

#include "arp.h"
#include "e1000.h"
#include "ipv4.h"
#include "net.h"
#include "../kernel/uapi_poll.h"
#include "../kernel/klib.h"
#include "../kernel/paging.h"
#include "../kernel/pmm.h"
#include "../kernel/process.h"
#include "../kernel/scheduler.h"
#include "../kernel/socket.h"
#include "../kernel/timer.h"
#include "../kernel/uapi_errno.h"
#include "../kernel/uapi_socket.h"
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
#define TCP_STATE_FIN_WAIT    3

#define TCP_RETRY_TICKS     (1u * SMALLOS_TIMER_HZ)
#define TCP_IDLE_TICKS      (12u * SMALLOS_TIMER_HZ)
#define TCP_MAX_RETRIES        3u
#define TCP_MAX_FRAME       1518u
#define TCP_MAX_PAYLOAD    (TCP_MAX_FRAME - 14u - 20u - 20u)
#define TCP_RX_BUFFER_SIZE  PAGE_SIZE
#define TCP_TX_BUFFER_SIZE  PAGE_SIZE
#define TCP_CONTROL_PORT     2121u
#define TCP_MAX_CONNS_PER_SLOT 32u
#define TCP_DEFAULT_BACKLOG     1u
#define TCP_MAX_BACKLOG        32u

typedef unsigned short u16;

typedef struct {
    int state;
    int accepted;
    u16 local_port;
    unsigned int conn_id;
    u8 remote_mac[6];
    u32 remote_ip;
    u16 remote_port;
    u32 remote_seq_next;
    u32 local_seq_next;
    u32 last_activity;
    u32 retransmit_at;
    unsigned int retries;
    u32 rx_frame;
    u8* rx_buf;
    u32 rx_len;
    u32 rx_head;
    int rx_window_closed;
    u32 tx_frame;
    u8* tx_buf;
    u32 tx_len;
    u32 tx_head;
    u32 tx_seq_base;
    u32 tx_next_send;
    u32 tx_retransmit_at;
    unsigned int tx_retries;
    u16 remote_window;
    int close_requested;
    int peer_closed;
    int local_read_closed;
    int local_write_closed;
} tcp_conn_t;

typedef struct {
    u16 local_port;
    int listener_active;
    unsigned int listen_backlog;
    tcp_conn_t conns[TCP_MAX_CONNS_PER_SLOT];
} tcp_slot_t;

#define TCP_MAX_SLOTS 8

static tcp_slot_t* s_slots;
static u32 s_slots_frame;
static u32 s_slots_frames;
static u16 s_active_port = TCP_LISTEN_PORT;
static unsigned int s_active_conn = TCP_SOCKET_CONN_NONE;
static u16 s_ip_id = 1u;
static u8 s_tx_frame[TCP_MAX_FRAME];
static u8 s_checksum_scratch[12u + TCP_MAX_FRAME];

static tcp_slot_t* tcp_slot_for_port(u16 port) {
    if (!s_slots) return 0;

    for (unsigned int i = 0; i < TCP_MAX_SLOTS; i++) {
        if (s_slots[i].local_port == port) {
            return &s_slots[i];
        }
    }
    return 0;
}

static tcp_slot_t* tcp_slot_for_port_create(u16 port) {
    tcp_slot_t* slot = tcp_slot_for_port(port);
    if (slot) {
        return slot;
    }

    for (unsigned int i = 0; i < TCP_MAX_SLOTS; i++) {
        if (s_slots[i].local_port == 0u) {
            k_memset(&s_slots[i], 0, sizeof(s_slots[i]));
            s_slots[i].local_port = port;
            return &s_slots[i];
        }
    }

    return 0;
}

static tcp_slot_t* tcp_active_slot(void) {
    tcp_slot_t* slot = tcp_slot_for_port(s_active_port);
    if (slot) {
        return slot;
    }
    if (!s_slots) {
        return 0;
    }
    return &s_slots[0];
}

#define s_socket_listener_port (tcp_active_slot()->local_port)
#define s_socket_listener_active (tcp_active_slot()->listener_active)

static tcp_conn_t* tcp_slot_conn(tcp_slot_t* slot, unsigned int conn_id) {
    if (!slot || conn_id >= TCP_MAX_CONNS_PER_SLOT) {
        return 0;
    }
    return &slot->conns[conn_id];
}

static tcp_conn_t* tcp_active_conn(void) {
    return tcp_slot_conn(tcp_active_slot(), s_active_conn);
}

static void tcp_conn_rx_release(tcp_conn_t* conn) {
    if (!conn || conn->rx_frame == 0u) {
        return;
    }

    pmm_free_frame(conn->rx_frame);
    conn->rx_frame = 0u;
    conn->rx_buf = 0;
    conn->rx_len = 0u;
    conn->rx_head = 0u;
    conn->rx_window_closed = 0;
}

static int tcp_conn_rx_ensure(tcp_conn_t* conn) {
    if (!conn) {
        return 0;
    }
    if (conn->rx_buf) {
        return 1;
    }

    conn->rx_frame = pmm_alloc_frame();
    if (!conn->rx_frame) {
        conn->rx_window_closed = 1;
        return 0;
    }

    conn->rx_buf = (u8*)paging_phys_to_kernel_virt(conn->rx_frame);
    conn->rx_head = 0u;
    conn->rx_len = 0u;
    conn->rx_window_closed = 0;
    return 1;
}

static u16 tcp_conn_rx_window(tcp_conn_t* conn) {
    unsigned int space;

    if (!conn || conn->rx_window_closed) {
        return 0u;
    }
    if (conn->local_read_closed) {
        return TCP_RX_BUFFER_SIZE > 0xFFFFu ? 0xFFFFu : TCP_RX_BUFFER_SIZE;
    }
    if (conn->rx_len >= TCP_RX_BUFFER_SIZE) {
        return 0u;
    }

    space = TCP_RX_BUFFER_SIZE - conn->rx_len;
    return (u16)(space > 0xFFFFu ? 0xFFFFu : space);
}

static unsigned int tcp_conn_rx_push(tcp_conn_t* conn,
                                     const u8* data,
                                     unsigned int len) {
    unsigned int to_copy;

    if (!conn || !data || len == 0u) {
        return 0u;
    }
    if (conn->rx_len >= TCP_RX_BUFFER_SIZE) {
        return 0u;
    }
    if (!tcp_conn_rx_ensure(conn)) {
        return 0u;
    }

    to_copy = TCP_RX_BUFFER_SIZE - conn->rx_len;
    if (to_copy > len) {
        to_copy = len;
    }

    for (unsigned int i = 0u; i < to_copy; i++) {
        unsigned int pos = conn->rx_head + conn->rx_len + i;
        if (pos >= TCP_RX_BUFFER_SIZE) {
            pos -= TCP_RX_BUFFER_SIZE;
        }
        conn->rx_buf[pos] = data[i];
    }
    conn->rx_len += to_copy;
    return to_copy;
}

static unsigned int tcp_conn_rx_pop(tcp_conn_t* conn,
                                    u8* buf,
                                    unsigned int len) {
    unsigned int to_copy;

    if (!conn || !buf || !conn->rx_buf || len == 0u) {
        return 0u;
    }

    to_copy = conn->rx_len;
    if (to_copy > len) {
        to_copy = len;
    }

    for (unsigned int i = 0u; i < to_copy; i++) {
        unsigned int pos = conn->rx_head + i;
        if (pos >= TCP_RX_BUFFER_SIZE) {
            pos -= TCP_RX_BUFFER_SIZE;
        }
        buf[i] = conn->rx_buf[pos];
    }

    conn->rx_head += to_copy;
    while (conn->rx_head >= TCP_RX_BUFFER_SIZE) {
        conn->rx_head -= TCP_RX_BUFFER_SIZE;
    }
    conn->rx_len -= to_copy;
    if (conn->rx_len == 0u) {
        tcp_conn_rx_release(conn);
    }
    return to_copy;
}

static void tcp_conn_tx_release(tcp_conn_t* conn) {
    if (!conn) {
        return;
    }

    if (conn->tx_frame != 0u) {
        pmm_free_frame(conn->tx_frame);
    }
    conn->tx_frame = 0u;
    conn->tx_buf = 0;
    conn->tx_len = 0u;
    conn->tx_head = 0u;
    conn->tx_seq_base = conn->local_seq_next;
    conn->tx_next_send = conn->local_seq_next;
    conn->tx_retransmit_at = 0u;
    conn->tx_retries = 0u;
}

static int tcp_conn_tx_ensure(tcp_conn_t* conn) {
    if (!conn) {
        return 0;
    }
    if (conn->tx_buf) {
        return 1;
    }

    conn->tx_frame = pmm_alloc_frame();
    if (!conn->tx_frame) {
        return 0;
    }

    conn->tx_buf = (u8*)paging_phys_to_kernel_virt(conn->tx_frame);
    conn->tx_len = 0u;
    conn->tx_head = 0u;
    conn->tx_seq_base = conn->local_seq_next;
    conn->tx_next_send = conn->local_seq_next;
    conn->tx_retransmit_at = 0u;
    conn->tx_retries = 0u;
    return 1;
}

static unsigned int tcp_conn_tx_space(tcp_conn_t* conn) {
    if (!conn || conn->tx_len >= TCP_TX_BUFFER_SIZE) {
        return 0u;
    }
    return TCP_TX_BUFFER_SIZE - conn->tx_len;
}

static unsigned int tcp_conn_tx_push(tcp_conn_t* conn,
                                     const u8* data,
                                     unsigned int len) {
    unsigned int to_copy;

    if (!conn || !data || len == 0u) {
        return 0u;
    }
    if (!tcp_conn_tx_ensure(conn)) {
        return 0u;
    }

    to_copy = tcp_conn_tx_space(conn);
    if (to_copy > len) {
        to_copy = len;
    }
    if (to_copy == 0u) {
        return 0u;
    }

    for (unsigned int i = 0u; i < to_copy; i++) {
        unsigned int pos = conn->tx_head + conn->tx_len + i;
        while (pos >= TCP_TX_BUFFER_SIZE) {
            pos -= TCP_TX_BUFFER_SIZE;
        }
        conn->tx_buf[pos] = data[i];
    }
    conn->tx_len += to_copy;
    return to_copy;
}

static void tcp_conn_tx_pop(tcp_conn_t* conn, unsigned int len) {
    if (!conn || len == 0u) {
        return;
    }
    if (len > conn->tx_len) {
        len = conn->tx_len;
    }

    conn->tx_head += len;
    while (conn->tx_head >= TCP_TX_BUFFER_SIZE) {
        conn->tx_head -= TCP_TX_BUFFER_SIZE;
    }
    conn->tx_len -= len;
    conn->tx_seq_base += len;

    if ((int)(conn->tx_next_send - conn->tx_seq_base) < 0) {
        conn->tx_next_send = conn->tx_seq_base;
    }
    if (conn->tx_len == 0u) {
        tcp_conn_tx_release(conn);
    }
}

static unsigned int tcp_conn_tx_contiguous(tcp_conn_t* conn,
                                           u32 seq,
                                           unsigned int max_len,
                                           const u8** out) {
    u32 offset;
    unsigned int pos;
    unsigned int avail;

    if (!conn || !conn->tx_buf || !out || max_len == 0u) {
        return 0u;
    }
    if ((int)(seq - conn->tx_seq_base) < 0) {
        return 0u;
    }
    offset = seq - conn->tx_seq_base;
    if (offset >= conn->tx_len) {
        return 0u;
    }

    pos = conn->tx_head + offset;
    while (pos >= TCP_TX_BUFFER_SIZE) {
        pos -= TCP_TX_BUFFER_SIZE;
    }

    avail = conn->tx_len - offset;
    if (avail > max_len) {
        avail = max_len;
    }
    if (avail > TCP_TX_BUFFER_SIZE - pos) {
        avail = TCP_TX_BUFFER_SIZE - pos;
    }
    *out = &conn->tx_buf[pos];
    return avail;
}

static void tcp_conn_tx_arm_retransmit(tcp_conn_t* conn) {
    if (!conn || conn->tx_len == 0u) {
        return;
    }
    conn->tx_retransmit_at = timer_get_ticks() + TCP_RETRY_TICKS;
}

static tcp_conn_t* tcp_find_conn(tcp_slot_t* slot,
                                 u32 remote_ip,
                                 u16 remote_port,
                                 unsigned int* out_id) {
    if (!slot) return 0;

    for (unsigned int i = 0; i < TCP_MAX_CONNS_PER_SLOT; i++) {
        tcp_conn_t* conn = &slot->conns[i];
        if (conn->state != TCP_STATE_CLOSED &&
            conn->remote_ip == remote_ip &&
            conn->remote_port == remote_port) {
            if (out_id) *out_id = i;
            return conn;
        }
    }

    return 0;
}

static tcp_conn_t* tcp_alloc_conn(tcp_slot_t* slot, unsigned int* out_id) {
    if (!slot) return 0;

    for (unsigned int i = 0; i < TCP_MAX_CONNS_PER_SLOT; i++) {
        tcp_conn_t* conn = &slot->conns[i];
        if (conn->state == TCP_STATE_CLOSED) {
            if (out_id) *out_id = i;
            return conn;
        }
    }

    return 0;
}

static unsigned int tcp_slot_pending_count(tcp_slot_t* slot) {
    unsigned int count = 0u;

    if (!slot) return 0u;
    for (unsigned int i = 0; i < TCP_MAX_CONNS_PER_SLOT; i++) {
        tcp_conn_t* conn = &slot->conns[i];
        if (conn->state != TCP_STATE_CLOSED && !conn->accepted) {
            count++;
        }
    }
    return count;
}

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
static int tcp_send_segment(u32 src_ip,
                            u32 dst_ip,
                            const u8* dst_mac,
                            u16 src_port,
                            u16 dst_port,
                            u32 seq,
                            u32 ack,
                            u8 flags,
                            u16 window,
                            const u8* payload,
                            u32 payload_len);
static void tcp_begin_close(tcp_conn_t* conn);

static void tcp_conn_tx_flush(tcp_conn_t* conn) {
    u32 tx_end;

    if (!conn || conn->state != TCP_STATE_ESTABLISHED || conn->tx_len == 0u) {
        return;
    }

    tx_end = conn->tx_seq_base + conn->tx_len;
    while ((int)(tx_end - conn->tx_next_send) > 0) {
        u32 outstanding = conn->tx_next_send - conn->tx_seq_base;
        u32 window = conn->remote_window;
        u32 chunk;
        const u8* payload = 0;

        if (window <= outstanding) {
            break;
        }

        chunk = tx_end - conn->tx_next_send;
        if (chunk > window - outstanding) {
            chunk = window - outstanding;
        }
        if (chunk > TCP_MAX_PAYLOAD) {
            chunk = TCP_MAX_PAYLOAD;
        }
        chunk = tcp_conn_tx_contiguous(conn,
                                       conn->tx_next_send,
                                       chunk,
                                       &payload);
        if (chunk == 0u) {
            break;
        }

        if (!tcp_send_segment(TCP_LOCAL_IP,
                              conn->remote_ip,
                              conn->remote_mac,
                              conn->local_port,
                              conn->remote_port,
                              conn->tx_next_send,
                              conn->remote_seq_next,
                              (u8)(TCP_ACK | TCP_PSH),
                              tcp_conn_rx_window(conn),
                              payload,
                              chunk)) {
            break;
        }

        conn->tx_next_send += chunk;
        if (conn->tx_retransmit_at == 0u) {
            tcp_conn_tx_arm_retransmit(conn);
        }
    }
}

static void tcp_conn_tx_retransmit(tcp_conn_t* conn) {
    u32 sent_len;
    unsigned int chunk;
    const u8* payload = 0;

    if (!conn || conn->tx_len == 0u) {
        return;
    }

    sent_len = conn->tx_next_send - conn->tx_seq_base;
    if (sent_len == 0u) {
        tcp_conn_tx_flush(conn);
        tcp_conn_tx_arm_retransmit(conn);
        return;
    }

    chunk = sent_len;
    if (chunk > TCP_MAX_PAYLOAD) {
        chunk = TCP_MAX_PAYLOAD;
    }
    chunk = tcp_conn_tx_contiguous(conn,
                                   conn->tx_seq_base,
                                   chunk,
                                   &payload);
    if (chunk == 0u) {
        return;
    }

    (void)tcp_send_segment(TCP_LOCAL_IP,
                           conn->remote_ip,
                           conn->remote_mac,
                           conn->local_port,
                           conn->remote_port,
                           conn->tx_seq_base,
                           conn->remote_seq_next,
                           (u8)(TCP_ACK | TCP_PSH),
                           tcp_conn_rx_window(conn),
                           payload,
                           chunk);
    conn->tx_retries++;
    tcp_conn_tx_arm_retransmit(conn);
}

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
                           u16 window,
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
        tcp_write_u16_be(frame, tcp_off + 14u, window);
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

static void tcp_reset_connection(tcp_conn_t* conn) {
    u16 local_port;
    unsigned int conn_id;

    if (!conn) {
        return;
    }

    local_port = conn->local_port;
    conn_id = conn->conn_id;
    if (local_port != 0u && conn_id != TCP_SOCKET_CONN_NONE) {
        socket_wake_tcp_connection(local_port,
                                   conn_id,
                                   (short)(POLLIN | POLLOUT | POLLHUP));
    }

    tcp_conn_rx_release(conn);
    tcp_conn_tx_release(conn);
    k_memset(conn, 0, sizeof(*conn));
    conn->state = TCP_STATE_CLOSED;
}

static void tcp_reset_slot(tcp_slot_t* slot) {
    if (!slot) {
        return;
    }
    for (unsigned int i = 0; i < TCP_MAX_CONNS_PER_SLOT; i++) {
        tcp_reset_connection(&slot->conns[i]);
    }
}

static void tcp_begin_close(tcp_conn_t* conn) {
    if (!conn || conn->state != TCP_STATE_ESTABLISHED) {
        return;
    }

    conn->local_write_closed = 1;
    if (conn->tx_len > 0u) {
        conn->close_requested = 1;
        tcp_conn_tx_flush(conn);
        return;
    }

    tcp_send_segment(TCP_LOCAL_IP,
                     conn->remote_ip,
                     conn->remote_mac,
                     conn->local_port,
                     conn->remote_port,
                     conn->local_seq_next,
                     conn->remote_seq_next,
                     (u8)(TCP_FIN | TCP_ACK),
                     tcp_conn_rx_window(conn),
                     0,
                     0);
    conn->local_seq_next += 1u;
    conn->close_requested = 0;
    conn->state = TCP_STATE_FIN_WAIT;
    conn->last_activity = timer_get_ticks();
    conn->retransmit_at = conn->last_activity + TCP_RETRY_TICKS;
    conn->retries = 0;
    socket_wake_tcp_connection(conn->local_port,
                               conn->conn_id,
                               (short)(POLLIN | POLLOUT | POLLHUP));
}

static void tcp_conn_tx_ack(tcp_conn_t* conn, u32 ack) {
    u32 sent_end;
    unsigned int acked;

    if (!conn || conn->tx_len == 0u) {
        return;
    }

    sent_end = conn->tx_next_send;
    if ((int)(ack - conn->tx_seq_base) <= 0) {
        tcp_conn_tx_flush(conn);
        return;
    }

    if ((int)(ack - sent_end) > 0) {
        ack = sent_end;
    }
    acked = ack - conn->tx_seq_base;
    if (acked == 0u) {
        tcp_conn_tx_flush(conn);
        return;
    }

    tcp_conn_tx_pop(conn, acked);
    socket_wake_tcp_connection(conn->local_port, conn->conn_id, POLLOUT);

    if (conn->tx_len > 0u) {
        conn->tx_retries = 0u;
        tcp_conn_tx_arm_retransmit(conn);
        tcp_conn_tx_flush(conn);
        return;
    }

    if (conn->close_requested) {
        tcp_begin_close(conn);
        if (conn->peer_closed) {
            tcp_reset_connection(conn);
        }
    }
}

void tcp_socket_use_port(unsigned int port) {
    s_active_port = (u16)port;
    s_active_conn = TCP_SOCKET_CONN_NONE;
}

void tcp_socket_use_connection(unsigned int port, unsigned int conn_id) {
    s_active_port = (u16)port;
    s_active_conn = conn_id;
}

void tcp_socket_close_listener(unsigned int port) {
    tcp_slot_t* slot;

    slot = tcp_slot_for_port((u16)port);
    if (!slot) {
        return;
    }

    tcp_socket_use_port(port);
    slot->listener_active = 0;
    slot->listen_backlog = 0u;
    socket_wake_tcp_listener(port);
    for (unsigned int i = 0; i < TCP_MAX_CONNS_PER_SLOT; i++) {
        tcp_conn_t* pending = &slot->conns[i];
        if (!pending->accepted) {
            tcp_reset_connection(pending);
        }
    }
}

void tcp_socket_close_connection(unsigned int port, unsigned int conn_id) {
    tcp_slot_t* slot;
    tcp_conn_t* conn;

    slot = tcp_slot_for_port((u16)port);
    if (!slot) {
        return;
    }

    tcp_socket_use_connection(port, conn_id);
    conn = tcp_active_conn();
    if (!conn) {
        return;
    }

    if (conn->state == TCP_STATE_ESTABLISHED) {
        tcp_begin_close(conn);
        if (conn->state == TCP_STATE_FIN_WAIT && conn->peer_closed) {
            tcp_reset_connection(conn);
        }
        return;
    }
    tcp_reset_connection(conn);
}

int tcp_socket_bind(unsigned int port) {
    if (port == 0u || port > 0xFFFFu) {
        return -1;
    }

    if (!tcp_slot_for_port_create((u16)port)) {
        return -1;
    }
    tcp_socket_use_port(port);
    s_socket_listener_active = 1;
    tcp_active_slot()->listen_backlog = TCP_DEFAULT_BACKLOG;
    tcp_reset_slot(tcp_active_slot());
    return 0;
}

int tcp_socket_listen(unsigned int backlog) {
    tcp_slot_t* slot = tcp_slot_for_port(s_active_port);

    if (!slot) {
        return -1;
    }
    if (backlog == 0u) {
        backlog = TCP_DEFAULT_BACKLOG;
    }
    if (backlog > TCP_MAX_BACKLOG) {
        backlog = TCP_MAX_BACKLOG;
    }
    s_socket_listener_active = 1;
    slot->listen_backlog = backlog;
    return 0;
}

int tcp_socket_accept_ready(void) {
    tcp_slot_t* slot = tcp_active_slot();
    if (!slot) return 0;

    for (unsigned int i = 0; i < TCP_MAX_CONNS_PER_SLOT; i++) {
        tcp_conn_t* conn = &slot->conns[i];
        if (conn->state == TCP_STATE_ESTABLISHED && !conn->accepted) {
            return 1;
        }
    }

    return 0;
}

unsigned int tcp_socket_mark_accepted(void) {
    tcp_slot_t* slot = tcp_active_slot();
    if (!slot) return TCP_SOCKET_CONN_NONE;

    for (unsigned int i = 0; i < TCP_MAX_CONNS_PER_SLOT; i++) {
        tcp_conn_t* conn = &slot->conns[i];
        if (conn->state == TCP_STATE_ESTABLISHED && !conn->accepted) {
            conn->accepted = 1;
            s_active_conn = i;
            return i;
        }
    }

    return TCP_SOCKET_CONN_NONE;
}

int tcp_socket_connection_established(void) {
    tcp_conn_t* conn = tcp_active_conn();
    return conn &&
           (conn->state == TCP_STATE_ESTABLISHED ||
            conn->state == TCP_STATE_FIN_WAIT);
}

int tcp_socket_recv_ready(void) {
    tcp_conn_t* conn = tcp_active_conn();
    return conn &&
           (conn->rx_len > 0u ||
            conn->peer_closed ||
            conn->local_read_closed);
}

int tcp_socket_peer_closed(void) {
    tcp_conn_t* conn = tcp_active_conn();
    return conn && conn->peer_closed;
}

int tcp_socket_recv(void* buf, unsigned int len) {
    tcp_conn_t* conn = tcp_active_conn();
    unsigned int to_copy;

    if (!conn || !buf) {
        return -1;
    }
    if (conn->local_read_closed) {
        return 0;
    }
    if (conn->state != TCP_STATE_ESTABLISHED &&
        conn->state != TCP_STATE_FIN_WAIT &&
        !conn->peer_closed) {
        return -1;
    }

    if (conn->rx_len == 0u) {
        return 0;
    }

    to_copy = tcp_conn_rx_pop(conn, (u8*)buf, len);
    if (to_copy > 0u &&
        (conn->state == TCP_STATE_ESTABLISHED ||
         conn->state == TCP_STATE_FIN_WAIT)) {
        tcp_send_segment(TCP_LOCAL_IP,
                         conn->remote_ip,
                         conn->remote_mac,
                         conn->local_port,
                         conn->remote_port,
                         conn->local_seq_next,
                         conn->remote_seq_next,
                         TCP_ACK,
                         tcp_conn_rx_window(conn),
                         0,
                         0);
    }
    return (int)to_copy;
}

int tcp_socket_send_ready(void) {
    tcp_conn_t* conn = tcp_active_conn();
    return conn && conn->state == TCP_STATE_ESTABLISHED &&
           !conn->local_write_closed &&
           tcp_conn_tx_space(conn) > 0u;
}

int tcp_socket_send(const void* buf, unsigned int len) {
    tcp_conn_t* conn = tcp_active_conn();
    unsigned int queued;
    unsigned int space;

    if (!buf || len == 0u) {
        return 0;
    }
    if (!conn || conn->state != TCP_STATE_ESTABLISHED) {
        return -ECONNRESET;
    }
    if (conn->local_write_closed) {
        return -EPIPE;
    }
    if (!tcp_conn_tx_ensure(conn)) {
        return -ENOMEM;
    }

    space = tcp_conn_tx_space(conn);
    if (space == 0u) {
        return -EAGAIN;
    }
    if (len > space) {
        len = space;
    }

    if (conn->tx_len == 0u) {
        conn->tx_seq_base = conn->local_seq_next;
        conn->tx_next_send = conn->local_seq_next;
    }

    queued = tcp_conn_tx_push(conn, (const u8*)buf, len);
    if (queued == 0u) {
        return -EAGAIN;
    }

    conn->local_seq_next += queued;
    tcp_conn_tx_flush(conn);
    if (conn->tx_retransmit_at == 0u) {
        tcp_conn_tx_arm_retransmit(conn);
    }
    return (int)queued;
}

int tcp_socket_shutdown(int how) {
    tcp_conn_t* conn = tcp_active_conn();

    if (!conn) {
        return -ECONNRESET;
    }
    if (conn->state != TCP_STATE_ESTABLISHED &&
        conn->state != TCP_STATE_FIN_WAIT) {
        return -ECONNRESET;
    }

    if (how == SHUT_RD || how == SHUT_RDWR) {
        conn->local_read_closed = 1;
        tcp_conn_rx_release(conn);
        socket_wake_tcp_connection(conn->local_port, conn->conn_id, POLLIN);
    }

    if (how == SHUT_WR || how == SHUT_RDWR) {
        if (conn->state == TCP_STATE_ESTABLISHED &&
            !conn->local_write_closed) {
            tcp_begin_close(conn);
        }
        socket_wake_tcp_connection(conn->local_port, conn->conn_id, POLLOUT);
    }

    return 0;
}

unsigned int tcp_socket_poll_events(void) {
    unsigned int events = 0u;
    tcp_conn_t* conn = tcp_active_conn();

    if (conn && conn->state == TCP_STATE_ESTABLISHED) {
        if (tcp_socket_send_ready()) {
            events |= POLLOUT;
        }
        if (tcp_socket_recv_ready()) {
            events |= POLLIN;
        }
    } else if (tcp_socket_accept_ready()) {
        events |= POLLIN;
    }

    return events;
}

unsigned int tcp_socket_peer_ip(void) {
    tcp_conn_t* conn = tcp_active_conn();
    return conn ? conn->remote_ip : 0u;
}

unsigned int tcp_socket_peer_port(void) {
    tcp_conn_t* conn = tcp_active_conn();
    return conn ? conn->remote_port : 0u;
}

unsigned int tcp_socket_local_port(void) {
    return s_socket_listener_port;
}

static int tcp_send_segment(u32 src_ip,
                            u32 dst_ip,
                            const u8* dst_mac,
                            u16 src_port,
                            u16 dst_port,
                            u32 seq,
                            u32 ack,
                            u8 flags,
                            u16 window,
                            const u8* payload,
                            u32 payload_len) {
    u32 frame_len = tcp_build_frame(s_tx_frame, dst_mac, src_ip, dst_ip,
                                    src_port, dst_port, seq, ack,
                                    flags, window, payload, payload_len);
    return e1000_send(s_tx_frame, frame_len);
}

static void tcp_accept_syn(const u8* frame,
                           u32 ip_header_len,
                           u32 total_len) {
    tcp_slot_t* slot;
    tcp_conn_t* conn;
    unsigned int conn_id;
    u32 tcp_off = 14u + ip_header_len;
    u16 src_port = tcp_read_u16_be(frame, tcp_off + 0u);
    u16 dst_port = tcp_read_u16_be(frame, tcp_off + 2u);
    u32 seq = tcp_read_u32_be(frame, tcp_off + 4u);
    u32 src_ip = tcp_read_u32_be(frame, 26);
    u8 data_offset = (u8)(frame[tcp_off + 12u] >> 4);
    u32 tcp_header_len = (u32)data_offset * 4u;
    u32 payload_len;

    if (data_offset < 5u) {
        return;
    }
    if (total_len < ip_header_len + tcp_header_len) {
        return;
    }
    slot = tcp_slot_for_port(dst_port);
    if (!slot) {
        return;
    }
    tcp_socket_use_port(dst_port);
    if (!slot->listener_active) {
        return;
    }
    if ((frame[tcp_off + 13u] & TCP_SYN) == 0u) {
        return;
    }

    payload_len = total_len - ip_header_len - tcp_header_len;
    if (payload_len > TCP_MAX_PAYLOAD) {
        return;
    }
    if (payload_len != 0u) {
        return;
    }

    conn = tcp_find_conn(slot, src_ip, src_port, 0);
    if (conn) {
        return;
    }
    if (slot->listen_backlog != 0u &&
        tcp_slot_pending_count(slot) >= slot->listen_backlog) {
        return;
    }
    conn = tcp_alloc_conn(slot, &conn_id);
    if (!conn) {
        return;
    }
    s_active_conn = conn_id;

    for (unsigned int i = 0; i < 6; i++) {
        conn->remote_mac[i] = frame[6 + i];
    }
    conn->local_port = dst_port;
    conn->conn_id = conn_id;
    conn->remote_ip = src_ip;
    conn->remote_port = src_port;
    conn->remote_seq_next = seq + 1u;
    conn->local_seq_next = 0x534D414Cu + conn_id; /* "SMAL" plus stream id */
    conn->tx_seq_base = conn->local_seq_next;
    conn->tx_next_send = conn->local_seq_next;
    conn->remote_window = tcp_read_u16_be(frame, tcp_off + 14u);
    conn->state = TCP_STATE_SYN_RCVD;
    conn->last_activity = timer_get_ticks();
    conn->retransmit_at = conn->last_activity + TCP_RETRY_TICKS;
    conn->retries = 0;
    conn->accepted = 0;
    conn->rx_frame = 0u;
    conn->rx_buf = 0;
    conn->rx_len = 0u;
    conn->rx_head = 0u;
    conn->rx_window_closed = 0;
    conn->tx_frame = 0u;
    conn->tx_buf = 0;
    conn->tx_len = 0u;
    conn->tx_head = 0u;
    conn->tx_retransmit_at = 0u;
    conn->tx_retries = 0u;
    conn->close_requested = 0;
    conn->peer_closed = 0;
    conn->local_read_closed = 0;
    conn->local_write_closed = 0;

    tcp_send_segment(TCP_LOCAL_IP,
                     conn->remote_ip,
                     conn->remote_mac,
                     conn->local_port,
                     conn->remote_port,
                     conn->local_seq_next,
                     conn->remote_seq_next,
                     (u8)(TCP_SYN | TCP_ACK),
                     tcp_conn_rx_window(conn),
                     0,
                     0);
}

static void tcp_process_rx_payload(tcp_slot_t* slot,
                                   tcp_conn_t* conn,
                                   unsigned int conn_id,
                                   const u8* payload,
                                   u32 payload_len,
                                   u32 seq,
                                   u8 flags) {
    if (!slot || !conn) {
        return;
    }
    if (seq != conn->remote_seq_next) {
        return;
    }

    conn->last_activity = timer_get_ticks();

    if (payload_len > 0u) {
        unsigned int accepted;

        if (conn->local_read_closed) {
            accepted = payload_len;
        } else {
            accepted = tcp_conn_rx_push(conn, payload, payload_len);
            if (accepted > 0u) {
                socket_wake_tcp_connection(slot->local_port, conn_id, POLLIN);
            }
        }

        conn->remote_seq_next += accepted;
        if (accepted < payload_len) {
            tcp_send_segment(TCP_LOCAL_IP,
                             conn->remote_ip,
                             conn->remote_mac,
                             conn->local_port,
                             conn->remote_port,
                             conn->local_seq_next,
                             conn->remote_seq_next,
                             TCP_ACK,
                             tcp_conn_rx_window(conn),
                             0,
                             0);
            return;
        }
    }

    if (flags & TCP_FIN) {
        conn->remote_seq_next += 1u;
    }

    tcp_send_segment(TCP_LOCAL_IP,
                     conn->remote_ip,
                     conn->remote_mac,
                     conn->local_port,
                     conn->remote_port,
                     conn->local_seq_next,
                     conn->remote_seq_next,
                     TCP_ACK,
                     tcp_conn_rx_window(conn),
                     0,
                     0);

    if (flags & TCP_FIN) {
        conn->peer_closed = 1;
        socket_wake_tcp_connection(slot->local_port,
                                   conn_id,
                                   (short)(POLLIN | POLLHUP));
    }
}

static void tcp_echo_payload(const u8* frame,
                             u32 ip_header_len,
                             u32 total_len) {
    tcp_slot_t* slot;
    tcp_conn_t* conn;
    unsigned int conn_id = TCP_SOCKET_CONN_NONE;
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
    u16 src_port = tcp_read_u16_be(frame, tcp_off + 0u);
    u16 dst_port = tcp_read_u16_be(frame, tcp_off + 2u);

    if (total_len < ip_header_len + 20u) {
        return;
    }
    if (!tcp_validate_tcp_segment(frame, ip_header_len, total_len)) {
        return;
    }
    if (tcp_len < header_len) {
        return;
    }
    slot = tcp_slot_for_port(dst_port);
    if (!slot) {
        return;
    }
    tcp_socket_use_port(dst_port);

    conn = tcp_find_conn(slot, src_ip, src_port, &conn_id);
    if (!conn) {
        tcp_accept_syn(frame, ip_header_len, total_len);
        return;
    }
    s_active_conn = conn_id;

    if (conn->remote_ip != src_ip || conn->remote_port != src_port) {
        return;
    }

    if (flags & TCP_RST) {
        tcp_reset_connection(conn);
        return;
    }

    conn->remote_window = tcp_read_u16_be(frame, tcp_off + 14u);
    if ((flags & TCP_ACK) != 0u) {
        tcp_conn_tx_ack(conn, ack);
    }

    if (conn->state == TCP_STATE_FIN_WAIT) {
        if (payload_len > TCP_MAX_PAYLOAD) {
            return;
        }
        tcp_process_rx_payload(slot,
                               conn,
                               conn_id,
                               &frame[payload_off],
                               payload_len,
                               seq,
                               flags);
        return;
    }

    if (conn->state == TCP_STATE_SYN_RCVD) {
        if ((flags & TCP_ACK) == 0u) {
            return;
        }
        if (ack != conn->local_seq_next + 1u) {
            return;
        }
        conn->state = TCP_STATE_ESTABLISHED;
        conn->local_seq_next += 1u;
        conn->tx_seq_base = conn->local_seq_next;
        conn->tx_next_send = conn->local_seq_next;
        conn->last_activity = timer_get_ticks();
        socket_wake_tcp_listener(slot->local_port);

        if (payload_len == 0u && (flags & TCP_FIN) == 0u) {
            return;
        }
    }

    if (payload_len > TCP_MAX_PAYLOAD) {
        return;
    }

    if (conn->state != TCP_STATE_ESTABLISHED) {
        return;
    }

    tcp_process_rx_payload(slot,
                           conn,
                           conn_id,
                           &frame[payload_off],
                           payload_len,
                           seq,
                           flags);
}

static void tcp_maybe_retransmit(void) {
    u32 now = timer_get_ticks();

    for (unsigned int i = 0; i < TCP_MAX_SLOTS; i++) {
        tcp_slot_t* slot = &s_slots[i];

        if (slot->local_port == 0u) {
            continue;
        }

        tcp_socket_use_port(slot->local_port);

        for (unsigned int c = 0; c < TCP_MAX_CONNS_PER_SLOT; c++) {
            tcp_conn_t* conn = &slot->conns[c];
            s_active_conn = c;

            if (conn->state == TCP_STATE_SYN_RCVD) {
                if (now < conn->retransmit_at) {
                    continue;
                }
                if (conn->retries >= TCP_MAX_RETRIES) {
                    tcp_reset_connection(conn);
                    continue;
                }

                conn->retries++;
                conn->retransmit_at = now + TCP_RETRY_TICKS;
                tcp_send_segment(TCP_LOCAL_IP,
                                 conn->remote_ip,
                                 conn->remote_mac,
                                 conn->local_port,
                                 conn->remote_port,
                                 conn->local_seq_next,
                                 conn->remote_seq_next,
                                 (u8)(TCP_SYN | TCP_ACK),
                                 tcp_conn_rx_window(conn),
                                 0,
                                 0);
                continue;
            }

            if (conn->state == TCP_STATE_ESTABLISHED) {
                tcp_conn_tx_flush(conn);
                if (conn->tx_len > 0u &&
                    conn->tx_retransmit_at != 0u &&
                    now >= conn->tx_retransmit_at) {
                    tcp_conn_tx_retransmit(conn);
                }
                if (slot->local_port != TCP_CONTROL_PORT &&
                    now - conn->last_activity > TCP_IDLE_TICKS) {
                    tcp_begin_close(conn);
                }
            } else if (conn->state == TCP_STATE_FIN_WAIT) {
                if (now - conn->last_activity > TCP_IDLE_TICKS) {
                    tcp_reset_connection(conn);
                }
            }
        }
    }
}

int tcp_handle_ipv4_frame(const unsigned char* frame, unsigned int len) {
    u32 ip_header_len = 0;
    u32 total_len = 0;

    if (!tcp_validate_ipv4(frame, len, &ip_header_len, &total_len)) {
        return 0;
    }

    tcp_echo_payload(frame, ip_header_len, total_len);
    return 1;
}

void tcp_get_stats(tcp_stats_t* out) {
    if (!out) {
        return;
    }

    k_memset(out, 0, sizeof(*out));
    out->max_listeners = TCP_MAX_SLOTS;
    out->max_connections_per_listener = TCP_MAX_CONNS_PER_SLOT;
    out->max_connections = TCP_MAX_SLOTS * TCP_MAX_CONNS_PER_SLOT;
    out->max_backlog = TCP_MAX_BACKLOG;

    if (!s_slots) {
        return;
    }

    for (unsigned int i = 0; i < TCP_MAX_SLOTS; i++) {
        tcp_slot_t* slot = &s_slots[i];
        if (slot->local_port == 0u) {
            continue;
        }

        if (slot->listener_active) {
            out->listeners++;
        }

        for (unsigned int c = 0; c < TCP_MAX_CONNS_PER_SLOT; c++) {
            tcp_conn_t* conn = &slot->conns[c];
            if (conn->state == TCP_STATE_CLOSED) {
                continue;
            }

            out->connections++;
            if (conn->accepted) {
                out->accepted_connections++;
            } else {
                out->pending_connections++;
            }

            if (conn->state == TCP_STATE_SYN_RCVD) {
                out->syn_recv_connections++;
            } else if (conn->state == TCP_STATE_ESTABLISHED) {
                out->established_connections++;
            } else if (conn->state == TCP_STATE_FIN_WAIT) {
                out->fin_wait_connections++;
            }

            if (conn->rx_frame != 0u) {
                out->rx_rings++;
                out->rx_buffer_bytes += TCP_RX_BUFFER_SIZE;
            }
            if (conn->tx_frame != 0u) {
                out->tx_rings++;
                out->tx_buffer_bytes += TCP_TX_BUFFER_SIZE;
            }
            out->rx_bytes += conn->rx_len;
            out->tx_bytes += conn->tx_len;
        }
    }
}

static void tcp_service_main(void) {
    for (;;) {
        net_poll_drain();
        tcp_maybe_retransmit();
        __asm__ __volatile__("sti; hlt");
    }
}

int tcp_init(void) {
    process_t* proc = process_create_kernel_task("tcp", tcp_service_main);
    unsigned int slot_bytes = sizeof(tcp_slot_t) * TCP_MAX_SLOTS;

    if (!proc) {
        return 0;
    }

    if (!s_slots) {
        s_slots_frames = PAGE_ALIGN(slot_bytes) / PAGE_SIZE;
        s_slots_frame = pmm_alloc_contiguous_frames(s_slots_frames);
        if (!s_slots_frame) {
            process_destroy(proc);
            return 0;
        }
        s_slots = (tcp_slot_t*)paging_phys_to_kernel_virt(s_slots_frame);
    }
    k_memset(s_slots, 0, s_slots_frames * PAGE_SIZE);
    if (!tcp_slot_for_port_create(TCP_LISTEN_PORT)) {
        pmm_free_contiguous_frames(s_slots_frame, s_slots_frames);
        s_slots = 0;
        s_slots_frame = 0;
        s_slots_frames = 0;
        process_destroy(proc);
        return 0;
    }
    tcp_socket_use_port(TCP_LISTEN_PORT);
    tcp_reset_slot(tcp_active_slot());

    if (!sched_enqueue(proc)) {
        process_destroy(proc);
        return 0;
    }

    return 1;
}
