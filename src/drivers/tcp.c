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

#define TCP_SYN              0x02u
#define TCP_RST              0x04u
#define TCP_PSH              0x08u
#define TCP_ACK              0x10u
#define TCP_FIN              0x01u

#define TCP_STATE_CLOSED      0
#define TCP_STATE_SYN_SENT    1
#define TCP_STATE_SYN_RCVD    2
#define TCP_STATE_ESTABLISHED 3
#define TCP_STATE_FIN_WAIT    4

#define TCP_RETRY_TICKS     (1u * SMALLOS_TIMER_HZ)
#define TCP_IDLE_TICKS      (12u * SMALLOS_TIMER_HZ)
#define TCP_MAX_RETRIES        3u
#define TCP_MAX_FRAME       1518u
#define TCP_MAX_PAYLOAD    (TCP_MAX_FRAME - 14u - 20u - 20u)
#define TCP_RX_BUFFER_FRAMES 16u
#define TCP_RX_BUFFER_SIZE  (TCP_RX_BUFFER_FRAMES * PAGE_SIZE)
#define TCP_TX_BUFFER_FRAMES 4u
#define TCP_TX_BUFFER_SIZE  (TCP_TX_BUFFER_FRAMES * PAGE_SIZE)
#define TCP_MAX_RX_BUFFER_BYTES (128u * TCP_RX_BUFFER_SIZE)
#define TCP_MAX_TX_BUFFER_BYTES (64u * TCP_TX_BUFFER_SIZE)
#define TCP_CONTROL_PORT        2121u
#define TCP_MAX_LISTENERS          8u
#define TCP_MAX_CONNECTIONS      256u
#define TCP_DEFAULT_BACKLOG        1u
#define TCP_MAX_BACKLOG           32u
#define TCP_EPHEMERAL_FIRST    49152u
#define TCP_EPHEMERAL_LAST     60999u

typedef unsigned short u16;

typedef struct {
    int state;
    int accepted;
    u32 local_ip;
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
    u32 rx_frames;
    u8* rx_buf;
    u32 rx_len;
    u32 rx_head;
    int rx_window_closed;
    u32 tx_frame;
    u32 tx_frames;
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
    int local_fin_acked;
} tcp_conn_t;

typedef struct {
    u16 local_port;
    int listener_active;
    unsigned int listen_backlog;
} tcp_slot_t;

typedef struct {
    tcp_slot_t slots[TCP_MAX_LISTENERS];
    tcp_conn_t conns[TCP_MAX_CONNECTIONS];
} tcp_tables_t;

static tcp_tables_t* s_tables;
static tcp_slot_t* s_slots;
static tcp_conn_t* s_conns;
static u32 s_tables_frame;
static u32 s_tables_frames;
static u16 s_next_ephemeral = TCP_EPHEMERAL_FIRST;
static u16 s_active_port = TCP_LISTEN_PORT;
static unsigned int s_active_conn = TCP_SOCKET_CONN_NONE;
static u16 s_ip_id = 1u;
static u8 s_tx_frame[TCP_MAX_FRAME];
static u8 s_checksum_scratch[12u + TCP_MAX_FRAME];

static tcp_slot_t* tcp_slot_for_port(u16 port) {
    if (!s_slots) return 0;

    for (unsigned int i = 0; i < TCP_MAX_LISTENERS; i++) {
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

    for (unsigned int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (s_slots[i].local_port == 0u) {
            k_memset(&s_slots[i], 0, sizeof(s_slots[i]));
            s_slots[i].local_port = port;
            return &s_slots[i];
        }
    }

    return 0;
}

static u32 tcp_local_ip(void) {
    return net_ipv4_local_ip();
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

static tcp_conn_t* tcp_conn_by_id(unsigned int conn_id) {
    if (!s_conns || conn_id >= TCP_MAX_CONNECTIONS) {
        return 0;
    }
    return &s_conns[conn_id];
}

static tcp_conn_t* tcp_active_conn(void) {
    tcp_conn_t* conn = tcp_conn_by_id(s_active_conn);

    if (!conn || conn->state == TCP_STATE_CLOSED) {
        return 0;
    }
    if (conn->local_port != s_active_port) {
        return 0;
    }
    return conn;
}

static unsigned int tcp_allocated_rx_bytes(void) {
    unsigned int bytes = 0u;

    if (!s_conns) return 0u;
    for (unsigned int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (s_conns[i].rx_frame != 0u) {
            bytes += TCP_RX_BUFFER_SIZE;
        }
    }
    return bytes;
}

static unsigned int tcp_allocated_tx_bytes(void) {
    unsigned int bytes = 0u;

    if (!s_conns) return 0u;
    for (unsigned int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (s_conns[i].tx_frame != 0u) {
            bytes += TCP_TX_BUFFER_SIZE;
        }
    }
    return bytes;
}

static int tcp_port_in_use(u16 port) {
    if (port == 0u) return 1;

    if (tcp_slot_for_port(port)) {
        return 1;
    }

    if (!s_conns) return 0;
    for (unsigned int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* conn = &s_conns[i];
        if (conn->state != TCP_STATE_CLOSED &&
            conn->local_port == port) {
            return 1;
        }
    }
    return 0;
}

static u16 tcp_alloc_ephemeral_port(void) {
    u16 start = s_next_ephemeral;

    for (;;) {
        u16 port = s_next_ephemeral;
        s_next_ephemeral++;
        if (s_next_ephemeral > TCP_EPHEMERAL_LAST) {
            s_next_ephemeral = TCP_EPHEMERAL_FIRST;
        }

        if (!tcp_port_in_use(port)) {
            return port;
        }
        if (s_next_ephemeral == start) {
            break;
        }
    }
    return 0u;
}

static u32 tcp_route_next_hop(u32 remote_ip) {
    u32 local_ip = net_ipv4_local_ip();
    u32 netmask = net_ipv4_netmask();
    u32 gateway = net_ipv4_gateway();

    if (local_ip == 0u) {
        return 0;
    }
    if (netmask != 0u && (remote_ip & netmask) == (local_ip & netmask)) {
        return remote_ip;
    }
    return gateway;
}

static void tcp_conn_rx_release(tcp_conn_t* conn) {
    if (!conn || conn->rx_frame == 0u) {
        return;
    }

    if (conn->rx_frames > 1u) {
        pmm_free_contiguous_frames(conn->rx_frame, conn->rx_frames);
    } else {
        pmm_free_frame(conn->rx_frame);
    }
    conn->rx_frame = 0u;
    conn->rx_frames = 0u;
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
    if (tcp_allocated_rx_bytes() + TCP_RX_BUFFER_SIZE > TCP_MAX_RX_BUFFER_BYTES) {
        conn->rx_window_closed = 1;
        return 0;
    }

    conn->rx_frame = pmm_alloc_contiguous_frames(TCP_RX_BUFFER_FRAMES);
    if (!conn->rx_frame) {
        conn->rx_window_closed = 1;
        return 0;
    }
    conn->rx_frames = TCP_RX_BUFFER_FRAMES;

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

    unsigned int tail = conn->rx_head + conn->rx_len;
    while (tail >= TCP_RX_BUFFER_SIZE) {
        tail -= TCP_RX_BUFFER_SIZE;
    }
    unsigned int first = TCP_RX_BUFFER_SIZE - tail;
    if (first > to_copy) {
        first = to_copy;
    }
    k_memcpy(conn->rx_buf + tail, data, first);
    if (first < to_copy) {
        k_memcpy(conn->rx_buf, data + first, to_copy - first);
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

    unsigned int first = TCP_RX_BUFFER_SIZE - conn->rx_head;
    if (first > to_copy) {
        first = to_copy;
    }
    k_memcpy(buf, conn->rx_buf + conn->rx_head, first);
    if (first < to_copy) {
        k_memcpy(buf + first, conn->rx_buf, to_copy - first);
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
        if (conn->tx_frames > 1u) {
            pmm_free_contiguous_frames(conn->tx_frame, conn->tx_frames);
        } else {
            pmm_free_frame(conn->tx_frame);
        }
    }
    conn->tx_frame = 0u;
    conn->tx_frames = 0u;
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
    if (tcp_allocated_tx_bytes() + TCP_TX_BUFFER_SIZE > TCP_MAX_TX_BUFFER_BYTES) {
        return 0;
    }

    conn->tx_frame = pmm_alloc_contiguous_frames(TCP_TX_BUFFER_FRAMES);
    if (!conn->tx_frame) {
        return 0;
    }
    conn->tx_frames = TCP_TX_BUFFER_FRAMES;

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

static tcp_conn_t* tcp_find_conn(u32 local_ip,
                                 u16 local_port,
                                 u32 remote_ip,
                                 u16 remote_port,
                                 unsigned int* out_id) {
    if (!s_conns) return 0;

    for (unsigned int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* conn = &s_conns[i];
        if (conn->state != TCP_STATE_CLOSED &&
            conn->local_ip == local_ip &&
            conn->local_port == local_port &&
            conn->remote_ip == remote_ip &&
            conn->remote_port == remote_port) {
            if (out_id) *out_id = i;
            return conn;
        }
    }

    return 0;
}

static tcp_conn_t* tcp_alloc_conn(unsigned int* out_id) {
    if (!s_conns) return 0;

    for (unsigned int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* conn = &s_conns[i];
        if (conn->state == TCP_STATE_CLOSED) {
            if (out_id) *out_id = i;
            return conn;
        }
    }

    return 0;
}

static unsigned int tcp_slot_pending_count(tcp_slot_t* slot) {
    unsigned int count = 0u;

    if (!slot || !s_conns) return 0u;
    for (unsigned int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* conn = &s_conns[i];
        if (conn->state != TCP_STATE_CLOSED &&
            conn->local_ip == tcp_local_ip() &&
            conn->local_port == slot->local_port &&
            !conn->accepted) {
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

        if (!tcp_send_segment(conn->local_ip,
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
        if (conn->remote_window == 0u) {
            chunk = tcp_conn_tx_contiguous(conn,
                                           conn->tx_seq_base,
                                           1u,
                                           &payload);
            if (chunk != 0u) {
                (void)tcp_send_segment(conn->local_ip,
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
            }
            conn->tx_retries++;
            tcp_conn_tx_arm_retransmit(conn);
            return;
        }
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

    (void)tcp_send_segment(conn->local_ip,
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
    if (!net_ipv4_is_configured() || tcp_read_u32_be(frame, 30) != tcp_local_ip()) {
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

static void tcp_reset_slot_connections(tcp_slot_t* slot, int accepted_too) {
    if (!slot || !s_conns) {
        return;
    }
    for (unsigned int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* conn = &s_conns[i];
        if (conn->state == TCP_STATE_CLOSED) continue;
        if (conn->local_ip != tcp_local_ip()) continue;
        if (conn->local_port != slot->local_port) continue;
        if (!accepted_too && conn->accepted) continue;
        tcp_reset_connection(conn);
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

    tcp_send_segment(conn->local_ip,
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
    conn->local_fin_acked = 0;
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
        if (conn->peer_closed && conn->local_fin_acked) {
            tcp_reset_connection(conn);
        }
    }
}

static void tcp_conn_fin_ack(tcp_conn_t* conn, u32 ack) {
    if (!conn || conn->state != TCP_STATE_FIN_WAIT) {
        return;
    }
    if (conn->local_fin_acked) {
        return;
    }
    if ((int)(ack - conn->local_seq_next) < 0) {
        return;
    }

    conn->local_fin_acked = 1;
    conn->retransmit_at = 0u;
    conn->retries = 0u;
    if (conn->peer_closed) {
        tcp_reset_connection(conn);
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
    tcp_reset_slot_connections(slot, 0);
}

void tcp_socket_close_connection(unsigned int port, unsigned int conn_id) {
    tcp_conn_t* conn;

    conn = tcp_conn_by_id(conn_id);
    if (!conn || conn->state == TCP_STATE_CLOSED) {
        return;
    }
    if (conn->local_port != (u16)port) {
        return;
    }

    tcp_socket_use_connection(port, conn_id);
    if (conn->state == TCP_STATE_ESTABLISHED) {
        tcp_begin_close(conn);
        if (conn->state == TCP_STATE_FIN_WAIT &&
            conn->peer_closed &&
            conn->local_fin_acked) {
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

    tcp_slot_t* slot = tcp_slot_for_port_create((u16)port);
    if (!slot) {
        return -1;
    }
    tcp_socket_use_port(port);
    slot->listener_active = 1;
    slot->listen_backlog = TCP_DEFAULT_BACKLOG;
    tcp_reset_slot_connections(slot, 1);
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
    slot->listener_active = 1;
    slot->listen_backlog = backlog;
    return 0;
}

int tcp_socket_connect(unsigned int local_port,
                       unsigned int remote_ip,
                       unsigned int remote_port,
                       unsigned int* out_local_port,
                       unsigned int* out_conn_id) {
    tcp_conn_t* conn;
    unsigned int conn_id;
    u16 chosen_port;
    u8 remote_mac[6];
    u32 next_hop;

    if (!out_local_port || !out_conn_id) return -EINVAL;
    if (remote_ip == 0u || remote_port == 0u || remote_port > 0xFFFFu) {
        return -EINVAL;
    }

    if (local_port != 0u) {
        if (local_port > 0xFFFFu) return -EINVAL;
        chosen_port = (u16)local_port;
        if (tcp_port_in_use(chosen_port)) return -EADDRINUSE;
    } else {
        chosen_port = tcp_alloc_ephemeral_port();
        if (chosen_port == 0u) return -EADDRINUSE;
    }

    next_hop = tcp_route_next_hop(remote_ip);
    if (next_hop == 0u || !arp_resolve(tcp_local_ip(), next_hop, remote_mac)) {
        return -EHOSTUNREACH;
    }

    conn = tcp_alloc_conn(&conn_id);
    if (!conn) return -ENOMEM;

    k_memset(conn, 0, sizeof(*conn));
    for (unsigned int i = 0; i < 6; i++) {
        conn->remote_mac[i] = remote_mac[i];
    }
    conn->local_ip = tcp_local_ip();
    conn->local_port = chosen_port;
    conn->conn_id = conn_id;
    conn->remote_ip = remote_ip;
    conn->remote_port = (u16)remote_port;
    conn->remote_seq_next = 0u;
    conn->local_seq_next = 0x434F4E4Eu + conn_id; /* "CONN" plus stream id */
    conn->tx_seq_base = conn->local_seq_next;
    conn->tx_next_send = conn->local_seq_next;
    conn->remote_window = 1u;
    conn->state = TCP_STATE_SYN_SENT;
    conn->accepted = 1;
    conn->last_activity = timer_get_ticks();
    conn->retransmit_at = conn->last_activity + TCP_RETRY_TICKS;
    conn->retries = 0u;

    s_active_port = chosen_port;
    s_active_conn = conn_id;

    if (!tcp_send_segment(conn->local_ip,
                          conn->remote_ip,
                          conn->remote_mac,
                          conn->local_port,
                          conn->remote_port,
                          conn->local_seq_next,
                          0u,
                          TCP_SYN,
                          tcp_conn_rx_window(conn),
                          0,
                          0)) {
        tcp_reset_connection(conn);
        return -EIO;
    }

    *out_local_port = chosen_port;
    *out_conn_id = conn_id;
    return 0;
}

int tcp_socket_accept_ready(void) {
    tcp_slot_t* slot = tcp_active_slot();
    if (!slot) return 0;

    if (!s_conns) return 0;
    for (unsigned int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* conn = &s_conns[i];
        if (conn->state == TCP_STATE_ESTABLISHED &&
            conn->local_ip == tcp_local_ip() &&
            conn->local_port == slot->local_port &&
            !conn->accepted) {
            return 1;
        }
    }

    return 0;
}

unsigned int tcp_socket_mark_accepted(void) {
    tcp_slot_t* slot = tcp_active_slot();
    if (!slot) return TCP_SOCKET_CONN_NONE;

    if (!s_conns) return TCP_SOCKET_CONN_NONE;
    for (unsigned int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t* conn = &s_conns[i];
        if (conn->state == TCP_STATE_ESTABLISHED &&
            conn->local_ip == tcp_local_ip() &&
            conn->local_port == slot->local_port &&
            !conn->accepted) {
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

int tcp_socket_connect_pending(void) {
    tcp_conn_t* conn = tcp_active_conn();
    return conn && conn->state == TCP_STATE_SYN_SENT;
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
        tcp_send_segment(conn->local_ip,
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

unsigned int tcp_socket_local_ip(void) {
    tcp_conn_t* conn = tcp_active_conn();
    return conn ? conn->local_ip : tcp_local_ip();
}

unsigned int tcp_socket_local_port(void) {
    tcp_conn_t* conn = tcp_active_conn();
    tcp_slot_t* slot;

    if (conn) {
        return conn->local_port;
    }
    slot = tcp_active_slot();
    return slot ? slot->local_port : 0u;
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

    conn = tcp_find_conn(tcp_local_ip(), dst_port, src_ip, src_port, 0);
    if (conn) {
        return;
    }
    if (slot->listen_backlog != 0u &&
        tcp_slot_pending_count(slot) >= slot->listen_backlog) {
        return;
    }
    conn = tcp_alloc_conn(&conn_id);
    if (!conn) {
        return;
    }
    s_active_conn = conn_id;

    k_memset(conn, 0, sizeof(*conn));
    for (unsigned int i = 0; i < 6; i++) {
        conn->remote_mac[i] = frame[6 + i];
    }
    conn->local_ip = tcp_local_ip();
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
    conn->local_fin_acked = 0;

    tcp_send_segment(conn->local_ip,
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

static void tcp_process_rx_payload(tcp_conn_t* conn,
                                   unsigned int conn_id,
                                   const u8* payload,
                                   u32 payload_len,
                                   u32 seq,
                                   u8 flags) {
    if (!conn) {
        return;
    }
    if (seq != conn->remote_seq_next) {
        tcp_send_segment(conn->local_ip,
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

    conn->last_activity = timer_get_ticks();

    if (payload_len > 0u) {
        unsigned int accepted;

        if (conn->local_read_closed) {
            accepted = payload_len;
        } else {
            accepted = tcp_conn_rx_push(conn, payload, payload_len);
            if (accepted > 0u) {
                socket_wake_tcp_connection(conn->local_port, conn_id, POLLIN);
            }
        }

        conn->remote_seq_next += accepted;
        if (accepted < payload_len) {
            tcp_send_segment(conn->local_ip,
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

    tcp_send_segment(conn->local_ip,
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
        socket_wake_tcp_connection(conn->local_port,
                                   conn_id,
                                   (short)(POLLIN | POLLHUP));
        if (conn->state == TCP_STATE_FIN_WAIT && conn->local_fin_acked) {
            tcp_reset_connection(conn);
        }
    }
}

static void tcp_echo_payload(const u8* frame,
                             u32 ip_header_len,
                             u32 total_len) {
    tcp_slot_t* slot = 0;
    tcp_conn_t* conn;
    unsigned int conn_id = TCP_SOCKET_CONN_NONE;
    u32 tcp_off = 14u + ip_header_len;
    u32 tcp_len = total_len - ip_header_len;
    u8 flags = frame[tcp_off + 13u];
    u8 data_offset = (u8)(frame[tcp_off + 12u] >> 4);
    u32 header_len = (u32)data_offset * 4u;
    u32 payload_off;
    u32 payload_len;
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
    if (data_offset < 5u || tcp_len < header_len) {
        return;
    }
    payload_off = tcp_off + header_len;
    payload_len = tcp_len - header_len;

    conn = tcp_find_conn(tcp_local_ip(), dst_port, src_ip, src_port, &conn_id);
    if (!conn) {
        slot = tcp_slot_for_port(dst_port);
        if (!slot) {
            return;
        }
        tcp_socket_use_port(dst_port);
        tcp_accept_syn(frame, ip_header_len, total_len);
        return;
    }
    tcp_socket_use_connection(dst_port, conn_id);
    s_active_conn = conn_id;

    if (conn->remote_ip != src_ip || conn->remote_port != src_port) {
        return;
    }

    if (flags & TCP_RST) {
        tcp_reset_connection(conn);
        return;
    }

    conn->remote_window = tcp_read_u16_be(frame, tcp_off + 14u);

    if (conn->state == TCP_STATE_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK)) {
            return;
        }
        if (ack != conn->local_seq_next + 1u) {
            return;
        }
        conn->remote_seq_next = seq + 1u;
        conn->local_seq_next += 1u;
        conn->tx_seq_base = conn->local_seq_next;
        conn->tx_next_send = conn->local_seq_next;
        conn->state = TCP_STATE_ESTABLISHED;
        conn->retransmit_at = 0u;
        conn->retries = 0u;
        conn->last_activity = timer_get_ticks();
        tcp_send_segment(conn->local_ip,
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
        socket_wake_tcp_connection(conn->local_port, conn_id, POLLOUT);

        if (payload_len == 0u && (flags & TCP_FIN) == 0u) {
            return;
        }
        seq += 1u;
    }

    if ((flags & TCP_ACK) != 0u) {
        tcp_conn_tx_ack(conn, ack);
        tcp_conn_fin_ack(conn, ack);
        if (conn->state == TCP_STATE_CLOSED) {
            return;
        }
    }

    if (conn->state == TCP_STATE_FIN_WAIT) {
        if (payload_len > TCP_MAX_PAYLOAD) {
            return;
        }
        tcp_process_rx_payload(conn,
                               conn_id,
                               &frame[payload_off],
                               payload_len,
                               seq,
                               flags);
        return;
    }

    if (conn->state == TCP_STATE_SYN_RCVD) {
        slot = tcp_slot_for_port(dst_port);
        if (!slot) {
            return;
        }
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

    tcp_process_rx_payload(conn,
                           conn_id,
                           &frame[payload_off],
                           payload_len,
                           seq,
                           flags);
}

static void tcp_maybe_retransmit(void) {
    u32 now = timer_get_ticks();

    if (!s_conns) {
        return;
    }

    for (unsigned int c = 0; c < TCP_MAX_CONNECTIONS; c++) {
        tcp_conn_t* conn = &s_conns[c];
        s_active_conn = c;

        if (conn->state == TCP_STATE_CLOSED) {
            continue;
        }
        tcp_socket_use_connection(conn->local_port, c);

        if (conn->state == TCP_STATE_SYN_SENT) {
            if (now < conn->retransmit_at) {
                continue;
            }
            if (conn->retries >= TCP_MAX_RETRIES) {
                tcp_reset_connection(conn);
                continue;
            }

            conn->retries++;
            conn->retransmit_at = now + TCP_RETRY_TICKS;
            tcp_send_segment(conn->local_ip,
                             conn->remote_ip,
                             conn->remote_mac,
                             conn->local_port,
                             conn->remote_port,
                             conn->local_seq_next,
                             0u,
                             TCP_SYN,
                             tcp_conn_rx_window(conn),
                             0,
                             0);
            continue;
        }

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
            tcp_send_segment(conn->local_ip,
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
            if (conn->local_port != TCP_CONTROL_PORT &&
                now - conn->last_activity > TCP_IDLE_TICKS) {
                tcp_begin_close(conn);
            }
        } else if (conn->state == TCP_STATE_FIN_WAIT) {
            if (conn->local_fin_acked && conn->peer_closed) {
                tcp_reset_connection(conn);
                continue;
            }
            if (!conn->local_fin_acked &&
                conn->retransmit_at != 0u &&
                now >= conn->retransmit_at) {
                if (conn->retries >= TCP_MAX_RETRIES) {
                    tcp_reset_connection(conn);
                    continue;
                }

                conn->retries++;
                conn->retransmit_at = now + TCP_RETRY_TICKS;
                tcp_send_segment(conn->local_ip,
                                 conn->remote_ip,
                                 conn->remote_mac,
                                 conn->local_port,
                                 conn->remote_port,
                                 conn->local_seq_next - 1u,
                                 conn->remote_seq_next,
                                 (u8)(TCP_FIN | TCP_ACK),
                                 tcp_conn_rx_window(conn),
                                 0,
                                 0);
            }
            if (now - conn->last_activity > TCP_IDLE_TICKS) {
                tcp_reset_connection(conn);
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
    out->max_listeners = TCP_MAX_LISTENERS;
    out->max_connections_per_listener = TCP_MAX_CONNECTIONS;
    out->max_connections = TCP_MAX_CONNECTIONS;
    out->max_backlog = TCP_MAX_BACKLOG;
    out->max_rx_buffer_bytes = TCP_MAX_RX_BUFFER_BYTES;
    out->max_tx_buffer_bytes = TCP_MAX_TX_BUFFER_BYTES;

    if (!s_slots || !s_conns) {
        return;
    }

    for (unsigned int i = 0; i < TCP_MAX_LISTENERS; i++) {
        tcp_slot_t* slot = &s_slots[i];
        if (slot->local_port == 0u) {
            continue;
        }

        if (slot->listener_active) {
            out->listeners++;
        }
    }

    for (unsigned int c = 0; c < TCP_MAX_CONNECTIONS; c++) {
        tcp_conn_t* conn = &s_conns[c];
        if (conn->state == TCP_STATE_CLOSED) {
            continue;
        }

        out->connections++;
        if (conn->accepted) {
            out->accepted_connections++;
        } else {
            out->pending_connections++;
        }

        if (conn->state == TCP_STATE_SYN_SENT ||
            conn->state == TCP_STATE_SYN_RCVD) {
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

static void tcp_service_main(void) {
    for (;;) {
        net_poll_drain();
        tcp_maybe_retransmit();
        __asm__ __volatile__("sti; hlt");
    }
}

int tcp_init(void) {
    process_t* proc = process_create_kernel_task("tcp", tcp_service_main);
    unsigned int table_bytes = sizeof(tcp_tables_t);

    if (!proc) {
        return 0;
    }

    if (!s_tables) {
        s_tables_frames = PAGE_ALIGN(table_bytes) / PAGE_SIZE;
        s_tables_frame = pmm_alloc_contiguous_frames(s_tables_frames);
        if (!s_tables_frame) {
            process_destroy(proc);
            return 0;
        }
        s_tables = (tcp_tables_t*)paging_phys_to_kernel_virt(s_tables_frame);
        s_slots = s_tables->slots;
        s_conns = s_tables->conns;
    }
    k_memset(s_tables, 0, s_tables_frames * PAGE_SIZE);
    if (!tcp_slot_for_port_create(TCP_LISTEN_PORT)) {
        pmm_free_contiguous_frames(s_tables_frame, s_tables_frames);
        s_tables = 0;
        s_slots = 0;
        s_conns = 0;
        s_tables_frame = 0;
        s_tables_frames = 0;
        process_destroy(proc);
        return 0;
    }
    tcp_socket_use_port(TCP_LISTEN_PORT);
    tcp_reset_slot_connections(tcp_active_slot(), 1);

    if (!sched_enqueue(proc)) {
        process_destroy(proc);
        return 0;
    }

    return 1;
}
