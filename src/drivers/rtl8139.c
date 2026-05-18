#include "rtl8139.h"

#include "nic.h"
#include "pci.h"
#include "../kernel/klib.h"
#include "../kernel/paging.h"
#include "../kernel/pmm.h"
#include "../kernel/ports.h"
#include "terminal.h"

typedef unsigned short u16;

#define RTL8139_VENDOR_ID 0x10ECu
#define RTL8139_DEVICE_ID 0x8139u

#define RTL8139_IDR0       0x00u
#define RTL8139_MAR0       0x08u
#define RTL8139_TX_STATUS0 0x10u
#define RTL8139_TX_ADDR0   0x20u
#define RTL8139_RX_BUF     0x30u
#define RTL8139_COMMAND    0x37u
#define RTL8139_CAPR       0x38u
#define RTL8139_CBR        0x3Au
#define RTL8139_IMR        0x3Cu
#define RTL8139_ISR        0x3Eu
#define RTL8139_TX_CONFIG  0x40u
#define RTL8139_RX_CONFIG  0x44u
#define RTL8139_CR9346     0x50u
#define RTL8139_CONFIG1    0x52u
#define RTL8139_MEDIA_STAT 0x58u

#define RTL8139_CMD_BUFE 0x01u
#define RTL8139_CMD_TE   0x04u
#define RTL8139_CMD_RE   0x08u
#define RTL8139_CMD_RST  0x10u

#define RTL8139_ISR_ROK  0x0001u
#define RTL8139_ISR_TOK  0x0004u
#define RTL8139_ISR_RXOVW 0x0010u
#define RTL8139_ISR_RER  0x0002u
#define RTL8139_ISR_TER  0x0008u

#define RTL8139_RCR_AAP  0x00000001u
#define RTL8139_RCR_APM  0x00000002u
#define RTL8139_RCR_AM   0x00000004u
#define RTL8139_RCR_AB   0x00000008u
#define RTL8139_RCR_WRAP 0x00000080u
#define RTL8139_RCR_RBLEN_8K 0x00000000u
#define RTL8139_RCR_MXDMA_UNLIMITED 0x00000700u

#define RTL8139_RX_STATUS_OK 0x0001u
#define RTL8139_RX_BUF_SIZE 8192u
#define RTL8139_RX_STORAGE_SIZE (RTL8139_RX_BUF_SIZE + 16u + 1500u)
#define RTL8139_TX_DESC_COUNT 4u
#define RTL8139_TX_BUF_SIZE 2048u
#define RTL8139_MAX_FRAME_SIZE 1518u
#define RTL8139_TEST_ETHER_TYPE 0x88B5u
#define RTL8139_MIN_FRAME_SIZE 60u
#define RTL8139_RX_STORAGE_BYTES PAGE_ALIGN(RTL8139_RX_STORAGE_SIZE)
#define RTL8139_TX_STORAGE_BYTES PAGE_ALIGN(RTL8139_TX_DESC_COUNT * RTL8139_TX_BUF_SIZE)

static unsigned short s_io_base = 0;
static u8 s_mac[6];
static u8* s_rx_buf = 0;
static u8* s_tx_buf = 0;
static u32 s_rx_buf_phys = 0;
static u32 s_tx_buf_phys = 0;
static unsigned int s_rx_offset = 0;
static unsigned int s_tx_next = 0;
static u32 s_tx_packets = 0;
static u32 s_rx_packets = 0;
static u32 s_tx_errors = 0;
static u32 s_rx_errors = 0;
static int s_present = 0;

static inline u8 rtl_read8(u16 reg) {
    return inb((u16)(s_io_base + reg));
}

static inline u16 rtl_read16(u16 reg) {
    return inw((u16)(s_io_base + reg));
}

static inline u32 rtl_read32(u16 reg) {
    return inl((u16)(s_io_base + reg));
}

static inline void rtl_write8(u16 reg, u8 value) {
    outb((u16)(s_io_base + reg), value);
}

static inline void rtl_write16(u16 reg, u16 value) {
    outw((u16)(s_io_base + reg), value);
}

static inline void rtl_write32(u16 reg, u32 value) {
    outl((u16)(s_io_base + reg), value);
}

static void rtl8139_print_mac(void) {
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned int i = 0; i < 6; i++) {
        unsigned char b = s_mac[i];
        terminal_putc(hex[b >> 4]);
        terminal_putc(hex[b & 0xF]);
        if (i != 5) {
            terminal_putc(':');
        }
    }
}

static void rtl8139_read_mac(void) {
    for (unsigned int i = 0; i < 6u; i++) {
        s_mac[i] = rtl_read8((u16)(RTL8139_IDR0 + i));
    }
}

static int rtl8139_find_device(void) {
    pci_device_t dev;
    unsigned short cmd;
    u32 bar0;

    if (!pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID, &dev)) {
        return 0;
    }

    cmd = pci_read_config_word(dev.bus, dev.slot, dev.func, 0x04);
    cmd |= 0x0005u; /* I/O space + bus mastering */
    pci_write_config_word(dev.bus, dev.slot, dev.func, 0x04, cmd);

    bar0 = pci_read_config_dword(dev.bus, dev.slot, dev.func, 0x10);
    if ((bar0 & 0x1u) == 0 || (bar0 & 0xFFF8u) == 0) {
        return 0;
    }

    s_io_base = (u16)(bar0 & 0xFFF8u);
    return 1;
}

static int rtl8139_wait_reset(void) {
    for (unsigned int i = 0; i < 1000000u; i++) {
        if ((rtl_read8(RTL8139_COMMAND) & RTL8139_CMD_RST) == 0) {
            return 1;
        }
    }
    return 0;
}

static int rtl8139_alloc_buffers(void) {
    u32 rx_frames = RTL8139_RX_STORAGE_BYTES / PAGE_SIZE;
    u32 tx_frames = RTL8139_TX_STORAGE_BYTES / PAGE_SIZE;

    if (s_rx_buf && s_tx_buf) {
        return 1;
    }

    s_rx_buf_phys = pmm_alloc_contiguous_frames(rx_frames);
    s_tx_buf_phys = pmm_alloc_contiguous_frames(tx_frames);
    if (!s_rx_buf_phys || !s_tx_buf_phys) {
        if (s_rx_buf_phys) pmm_free_contiguous_frames(s_rx_buf_phys, rx_frames);
        if (s_tx_buf_phys) pmm_free_contiguous_frames(s_tx_buf_phys, tx_frames);
        s_rx_buf_phys = 0;
        s_tx_buf_phys = 0;
        return 0;
    }

    s_rx_buf = (u8*)paging_phys_to_kernel_virt(s_rx_buf_phys);
    s_tx_buf = (u8*)paging_phys_to_kernel_virt(s_tx_buf_phys);
    k_memset(s_rx_buf, 0, RTL8139_RX_STORAGE_BYTES);
    k_memset(s_tx_buf, 0, RTL8139_TX_STORAGE_BYTES);
    return 1;
}

int rtl8139_init(void) {
    if (!rtl8139_find_device()) {
        terminal_puts("rtl8139: not found\n");
        s_present = 0;
        return 0;
    }

    rtl_write8(RTL8139_CR9346, 0xC0u);
    rtl_write8(RTL8139_CONFIG1, 0x00u);
    rtl_write8(RTL8139_CR9346, 0x00u);
    rtl_write8(RTL8139_COMMAND, RTL8139_CMD_RST);
    if (!rtl8139_wait_reset()) {
        terminal_puts("rtl8139: reset timeout\n");
        s_present = 0;
        return 0;
    }

    s_present = 1;
    if (!rtl8139_alloc_buffers()) {
        terminal_puts("rtl8139: cannot allocate buffers\n");
        s_present = 0;
        return 0;
    }

    k_memset(s_rx_buf, 0, RTL8139_RX_STORAGE_SIZE);
    for (unsigned int i = 0; i < RTL8139_TX_DESC_COUNT; i++) {
        u8* tx_buf = s_tx_buf + i * RTL8139_TX_BUF_SIZE;
        k_memset(tx_buf, 0, RTL8139_TX_BUF_SIZE);
        rtl_write32((u16)(RTL8139_TX_ADDR0 + i * 4u),
                    s_tx_buf_phys + i * RTL8139_TX_BUF_SIZE);
    }

    rtl8139_read_mac();
    rtl_write32(RTL8139_RX_BUF, s_rx_buf_phys);
    s_rx_offset = 0;
    s_tx_next = 0;
    s_tx_packets = 0;
    s_rx_packets = 0;
    s_tx_errors = 0;
    s_rx_errors = 0;
    rtl_write16(RTL8139_CAPR, 0xFFF0u);

    rtl_write16(RTL8139_IMR, 0x0000u);
    rtl_write16(RTL8139_ISR, 0xFFFFu);
    rtl_write32(RTL8139_MAR0, 0xFFFFFFFFu);
    rtl_write32((u16)(RTL8139_MAR0 + 4u), 0xFFFFFFFFu);
    rtl_write8(RTL8139_COMMAND, RTL8139_CMD_RE | RTL8139_CMD_TE);
    rtl_write32(RTL8139_RX_CONFIG,
                RTL8139_RCR_APM |
                RTL8139_RCR_AM |
                RTL8139_RCR_AB |
                RTL8139_RCR_WRAP |
                RTL8139_RCR_RBLEN_8K |
                RTL8139_RCR_MXDMA_UNLIMITED);
    rtl_write32(RTL8139_TX_CONFIG, 0x03000000u);

    terminal_puts("rtl8139: ready io=");
    terminal_put_hex(s_io_base);
    terminal_puts(" mac=");
    rtl8139_print_mac();
    terminal_puts(" link=");
    terminal_puts(rtl8139_link_up() ? "up" : "down");
    terminal_putc('\n');

    return 1;
}

void rtl8139_print_info(void) {
    if (!s_present) {
        terminal_puts("rtl8139: not initialized\n");
        return;
    }

    terminal_puts("rtl8139: mac=");
    rtl8139_print_mac();
    terminal_puts(" link=");
    terminal_puts(rtl8139_link_up() ? "up" : "down");
    terminal_puts(" rx=");
    terminal_put_uint(s_rx_offset);
    terminal_puts(" tx=");
    terminal_put_uint(s_tx_next);
    terminal_puts(" pkts=");
    terminal_put_uint(s_tx_packets);
    terminal_putc('/');
    terminal_put_uint(s_rx_packets);
    terminal_puts(" err=");
    terminal_put_uint(s_tx_errors);
    terminal_putc('/');
    terminal_put_uint(s_rx_errors);
    terminal_puts(" isr=");
    terminal_put_hex(rtl_read16(RTL8139_ISR));
    terminal_puts(" cbr=");
    terminal_put_uint(rtl_read16(RTL8139_CBR));
    terminal_putc('\n');
}

int rtl8139_link_up(void) {
    if (!s_present) {
        return 0;
    }

    return (rtl_read8(RTL8139_MEDIA_STAT) & 0x04u) == 0;
}

const u8* rtl8139_mac(void) {
    return s_present ? s_mac : 0;
}

void rtl8139_get_stats(nic_stats_t* out) {
    if (!out) return;
    out->tx_packets = s_tx_packets;
    out->rx_packets = s_rx_packets;
    out->tx_errors = s_tx_errors;
    out->rx_errors = s_rx_errors;
    out->status = s_present ? rtl_read16(RTL8139_ISR) : 0;
    out->command = s_present ? rtl_read8(RTL8139_COMMAND) : 0;
    out->rx_config = s_present ? rtl_read32(RTL8139_RX_CONFIG) : 0;
    out->tx_config = s_present ? rtl_read32(RTL8139_TX_CONFIG) : 0;
    out->rx_cursor = s_rx_offset;
    out->rx_hw_cursor = s_present ? rtl_read16(RTL8139_CBR) : 0;
}

int rtl8139_send(const void* data, u32 len) {
    unsigned int index;
    u32 tx_len;

    if (!s_present || !data || len == 0u || len > RTL8139_MAX_FRAME_SIZE) {
        s_tx_errors++;
        return 0;
    }

    index = s_tx_next;
    u8* tx_buf = s_tx_buf + index * RTL8139_TX_BUF_SIZE;
    k_memcpy(tx_buf, data, len);
    if (len < RTL8139_MIN_FRAME_SIZE) {
        k_memset(tx_buf + len, 0, RTL8139_MIN_FRAME_SIZE - len);
        tx_len = RTL8139_MIN_FRAME_SIZE;
    } else {
        tx_len = len;
    }
    rtl_write32((u16)(RTL8139_TX_ADDR0 + index * 4u),
                s_tx_buf_phys + index * RTL8139_TX_BUF_SIZE);
    rtl_write32((u16)(RTL8139_TX_STATUS0 + index * 4u), tx_len);
    s_tx_next = (index + 1u) % RTL8139_TX_DESC_COUNT;
    s_tx_packets++;

    return 1;
}

int rtl8139_recv(void* out, u32 out_size, u32* out_len) {
    u32 offset;
    u16 status;
    u16 packet_len;
    u32 payload_len;
    u32 next_offset;

    if (!s_present || !out || out_size == 0u) {
        s_rx_errors++;
        return 0;
    }

    if (rtl_read8(RTL8139_COMMAND) & RTL8139_CMD_BUFE) {
        return 0;
    }

    offset = s_rx_offset % RTL8139_RX_BUF_SIZE;
    status = (u16)(s_rx_buf[offset] | ((u16)s_rx_buf[offset + 1u] << 8));
    packet_len = (u16)(s_rx_buf[offset + 2u] | ((u16)s_rx_buf[offset + 3u] << 8));
    if ((status & RTL8139_RX_STATUS_OK) == 0 || packet_len < 4u) {
        s_rx_errors++;
        rtl_write16(RTL8139_ISR, RTL8139_ISR_ROK | RTL8139_ISR_RER | RTL8139_ISR_RXOVW);
        next_offset = (offset + 4u + packet_len + 3u) & ~3u;
        s_rx_offset = next_offset % RTL8139_RX_BUF_SIZE;
        rtl_write16(RTL8139_CAPR, (u16)((s_rx_offset - 16u) & 0xFFFFu));
        return 0;
    }

    payload_len = (u32)packet_len - 4u;
    if (payload_len > out_size) {
        payload_len = out_size;
    }

    k_memcpy(out, &s_rx_buf[offset + 4u], payload_len);
    if (out_len) {
        *out_len = payload_len;
    }

    next_offset = (offset + 4u + packet_len + 3u) & ~3u;
    s_rx_offset = next_offset % RTL8139_RX_BUF_SIZE;
    rtl_write16(RTL8139_CAPR, (u16)((s_rx_offset - 16u) & 0xFFFFu));
    rtl_write16(RTL8139_ISR, RTL8139_ISR_ROK | RTL8139_ISR_RXOVW);
    s_rx_packets++;

    return 1;
}

int rtl8139_send_test_frame(void) {
    u8 frame[64];
    static const u8 payload[] = "SmallOS rtl8139 test";
    const u8* mac = rtl8139_mac();

    if (!s_present || !mac) {
        return 0;
    }

    for (unsigned int i = 0; i < 6u; i++) {
        frame[i] = 0xFFu;
        frame[6u + i] = mac[i];
    }

    frame[12] = (u8)(RTL8139_TEST_ETHER_TYPE >> 8);
    frame[13] = (u8)(RTL8139_TEST_ETHER_TYPE & 0xFFu);
    k_memset(&frame[14], 0, sizeof(frame) - 14u);
    k_memcpy(&frame[14], payload, sizeof(payload) - 1u);

    return rtl8139_send(frame, sizeof(frame));
}
