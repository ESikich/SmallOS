#include "e1000.h"

#include "pci.h"
#include "../kernel/klib.h"
#include "../kernel/paging.h"
#include "../kernel/ports.h"
#include "terminal.h"

#define E1000_VENDOR_ID 0x8086u
#define E1000_DEVICE_ID 0x100Eu

#define E1000_CTRL      0x0000u
#define E1000_STATUS    0x0008u
#define E1000_EERD      0x0014u
#define E1000_IMS       0x00D0u
#define E1000_IMC       0x00D8u
#define E1000_RCTL      0x0100u
#define E1000_TCTL      0x0400u
#define E1000_TIPG      0x0410u
#define E1000_RDBAL     0x2800u
#define E1000_RDBAH     0x2804u
#define E1000_RDLEN     0x2808u
#define E1000_RDH       0x2810u
#define E1000_RDT       0x2818u
#define E1000_TDBAL     0x3800u
#define E1000_TDBAH     0x3804u
#define E1000_TDLEN     0x3808u
#define E1000_TDH       0x3810u
#define E1000_TDT       0x3818u
#define E1000_RAL0      0x5400u
#define E1000_RAH0      0x5404u

#define E1000_CTRL_RST  0x04000000u
#define E1000_STATUS_LU  0x00000002u

#define E1000_RCTL_EN   0x00000002u
#define E1000_RCTL_BAM  0x00008000u
#define E1000_RCTL_SECRC 0x04000000u

#define E1000_TCTL_EN   0x00000002u
#define E1000_TCTL_PSP  0x00000008u
#define E1000_TCTL_CT_SHIFT   4
#define E1000_TCTL_COLD_SHIFT 12

#define E1000_TXD_CMD_EOP  0x01u
#define E1000_TXD_CMD_IFCS 0x02u
#define E1000_TXD_CMD_RS   0x08u
#define E1000_TXD_STAT_DD  0x01u

#define E1000_RX_DESC_COUNT 8u
#define E1000_TX_DESC_COUNT 8u
#define E1000_RX_BUF_SIZE    2048u
#define E1000_MAX_FRAME_SIZE 1518u
#define E1000_MMIO_MAP_SIZE  0x20000u
#define E1000_TEST_ETHER_TYPE 0x88B5u

/* 16-byte ring descriptors used by the e1000 DMA engine. */
typedef struct {
    u32 addr_lo;
    u32 addr_hi;
    u16 length;
    u16 csum;
    u8  status;
    u8  errors;
    u16 special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    u32 addr_lo;
    u32 addr_hi;
    u16 length;
    u8  cso;
    u8  cmd;
    u8  status;
    u8  css;
    u16 special;
} __attribute__((packed)) e1000_tx_desc_t;

static volatile u32* s_regs = 0;
static u8 s_mac[6];
static e1000_rx_desc_t s_rx_desc[E1000_RX_DESC_COUNT] __attribute__((aligned(16)));
static e1000_tx_desc_t s_tx_desc[E1000_TX_DESC_COUNT] __attribute__((aligned(16)));
static u8 s_rx_buf[E1000_RX_DESC_COUNT][E1000_RX_BUF_SIZE] __attribute__((aligned(16)));
static u8 s_tx_buf[E1000_TX_DESC_COUNT][E1000_RX_BUF_SIZE] __attribute__((aligned(16)));
static unsigned int s_rx_head = 0;
static unsigned int s_tx_head = 0;
static unsigned int s_tx_tail = 0;
static int s_present = 0;

static inline u32 e1000_reg_read(u32 reg) {
    return s_regs[reg / 4u];
}

static inline void e1000_reg_write(u32 reg, u32 value) {
    s_regs[reg / 4u] = value;
}

static void e1000_wait_for_reset(void) {
    for (unsigned int i = 0; i < 1000000u; i++) {
        if (!(e1000_reg_read(E1000_CTRL) & E1000_CTRL_RST)) {
            return;
        }
    }
}

static void e1000_read_mac(void) {
    u32 ral = e1000_reg_read(E1000_RAL0);
    u32 rah = e1000_reg_read(E1000_RAH0);

    s_mac[0] = (u8)(ral & 0xFFu);
    s_mac[1] = (u8)((ral >> 8) & 0xFFu);
    s_mac[2] = (u8)((ral >> 16) & 0xFFu);
    s_mac[3] = (u8)((ral >> 24) & 0xFFu);
    s_mac[4] = (u8)(rah & 0xFFu);
    s_mac[5] = (u8)((rah >> 8) & 0xFFu);
}

static void e1000_print_mac(void) {
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

static void e1000_setup_rings(void) {
    /*
     * RX buffers are owned by the driver and handed to the NIC as a
     * ring of ready-to-fill packet targets.
     */
    for (unsigned int i = 0; i < E1000_RX_DESC_COUNT; i++) {
        s_rx_desc[i].addr_lo = (u32)(unsigned int)&s_rx_buf[i][0];
        s_rx_desc[i].addr_hi = 0;
        s_rx_desc[i].length = 0;
        s_rx_desc[i].csum = 0;
        s_rx_desc[i].status = 0;
        s_rx_desc[i].errors = 0;
        s_rx_desc[i].special = 0;
        k_memset(s_rx_buf[i], 0, E1000_RX_BUF_SIZE);
    }

    /*
     * TX buffers mirror the RX layout: the CPU copies a frame into a
     * descriptor-owned buffer, then tells the NIC to DMA it out.
     */
    for (unsigned int i = 0; i < E1000_TX_DESC_COUNT; i++) {
        s_tx_desc[i].addr_lo = (u32)(unsigned int)&s_tx_buf[i][0];
        s_tx_desc[i].addr_hi = 0;
        s_tx_desc[i].length = 0;
        s_tx_desc[i].cso = 0;
        s_tx_desc[i].cmd = 0;
        s_tx_desc[i].status = E1000_TXD_STAT_DD;
        s_tx_desc[i].css = 0;
        s_tx_desc[i].special = 0;
        k_memset(s_tx_buf[i], 0, E1000_RX_BUF_SIZE);
    }

    e1000_reg_write(E1000_RDBAL, (u32)(unsigned int)&s_rx_desc[0]);
    e1000_reg_write(E1000_RDBAH, 0);
    e1000_reg_write(E1000_RDLEN, sizeof(s_rx_desc));
    e1000_reg_write(E1000_RDH, 0);
    e1000_reg_write(E1000_RDT, E1000_RX_DESC_COUNT - 1u);

    e1000_reg_write(E1000_TDBAL, (u32)(unsigned int)&s_tx_desc[0]);
    e1000_reg_write(E1000_TDBAH, 0);
    e1000_reg_write(E1000_TDLEN, sizeof(s_tx_desc));
    e1000_reg_write(E1000_TDH, 0);
    e1000_reg_write(E1000_TDT, 0);

    s_rx_head = 0;
    s_tx_head = 0;
    s_tx_tail = 0;
}

static void e1000_program_rx_tx(void) {
    /* Minimal steady-state config: accept packets and enable transmit. */
    e1000_reg_write(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
    e1000_reg_write(E1000_TIPG, 0x0060200Au);
    e1000_reg_write(E1000_TCTL,
                    E1000_TCTL_EN |
                    E1000_TCTL_PSP |
                    (15u << E1000_TCTL_CT_SHIFT) |
                    (64u << E1000_TCTL_COLD_SHIFT));
    e1000_reg_write(E1000_IMC, 0xFFFFFFFFu);
}

static int e1000_find_and_map(void) {
    pci_device_t dev;
    unsigned short cmd;
    u32 bar0;
    u32* pd;

    if (!pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_ID, &dev)) {
        return 0;
    }

    cmd = pci_read_config_word(dev.bus, dev.slot, dev.func, 0x04);
    cmd |= 0x0006u; /* memory + bus mastering */
    pci_write_config_word(dev.bus, dev.slot, dev.func, 0x04, cmd);

    bar0 = pci_read_config_dword(dev.bus, dev.slot, dev.func, 0x10) & 0xFFFFFFF0u;
    if (bar0 == 0) {
        return 0;
    }

    /*
     * The BAR sits above the identity-mapped low memory window, so map
     * the MMIO pages into the kernel page table before touching them.
     */
    pd = paging_get_kernel_pd();
    for (u32 off = 0; off < E1000_MMIO_MAP_SIZE; off += PAGE_SIZE) {
        paging_map_page(pd, bar0 + off, bar0 + off, PAGE_WRITE);
    }

    s_regs = (volatile u32*)(unsigned int)bar0;
    return 1;
}

int e1000_init(void) {
    if (!e1000_find_and_map()) {
        terminal_puts("e1000: not found\n");
        s_present = 0;
        return 0;
    }

    s_present = 1;

    /* Reset the controller before programming the rings. */
    e1000_reg_write(E1000_IMC, 0xFFFFFFFFu);
    e1000_reg_write(E1000_CTRL, E1000_CTRL_RST);
    e1000_wait_for_reset();

    e1000_read_mac();
    e1000_setup_rings();
    e1000_program_rx_tx();

    terminal_puts("e1000: ready mac=");
    e1000_print_mac();
    terminal_puts(" link=");
    terminal_puts(e1000_link_up() ? "up" : "down");
    terminal_putc('\n');

    return 1;
}

void e1000_print_info(void) {
    if (!s_present) {
        terminal_puts("e1000: not initialized\n");
        return;
    }

    terminal_puts("e1000: mac=");
    e1000_print_mac();
    terminal_puts(" link=");
    terminal_puts(e1000_link_up() ? "up" : "down");
    terminal_puts(" tx=");
    terminal_put_uint(s_tx_tail);
    terminal_puts(" rx=");
    terminal_put_uint((unsigned int)e1000_reg_read(E1000_RDH));
    terminal_putc('/');
    terminal_put_uint((unsigned int)e1000_reg_read(E1000_RDT));
    terminal_putc('\n');
}

int e1000_link_up(void) {
    if (!s_present) {
        return 0;
    }

    return (e1000_reg_read(E1000_STATUS) & E1000_STATUS_LU) != 0;
}

const u8* e1000_mac(void) {
    return s_mac;
}

int e1000_send(const void* data, u32 len) {
    unsigned int next;
    void* dst;

    if (!s_present || !data || len == 0 || len > E1000_MAX_FRAME_SIZE) {
        return 0;
    }

    next = s_tx_tail;
    if ((s_tx_desc[next].status & E1000_TXD_STAT_DD) == 0) {
        return 0;
    }

    /* Copy the packet into the descriptor-owned staging buffer. */
    dst = s_tx_buf[next];
    k_memcpy(dst, data, len);

    s_tx_desc[next].length = (u16)len;
    s_tx_desc[next].cmd = (u8)(E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS);
    s_tx_desc[next].status = 0;

    /* Advance the tail so the NIC starts DMA on this descriptor. */
    s_tx_tail = (next + 1u) % E1000_TX_DESC_COUNT;
    e1000_reg_write(E1000_TDT, s_tx_tail);

    return 1;
}

int e1000_recv(void* out, u32 out_size, u32* out_len) {
    e1000_rx_desc_t* desc;
    u32 len;

    if (!s_present || !out || out_size == 0) {
        return 0;
    }

    desc = &s_rx_desc[s_rx_head];
    if ((desc->status & 0x01u) == 0) {
        return 0;
    }

    len = desc->length;
    if (len > out_size) {
        len = out_size;
    }

    k_memcpy(out, s_rx_buf[s_rx_head], len);
    if (out_len) {
        *out_len = len;
    }

    /* Mark the descriptor available again and hand it back to hardware. */
    desc->status = 0;
    e1000_reg_write(E1000_RDT, s_rx_head);
    s_rx_head = (s_rx_head + 1u) % E1000_RX_DESC_COUNT;

    return 1;
}

int e1000_send_test_frame(void) {
    u8 frame[64];
    static const u8 payload[] = "SmallOS e1000 test";
    const u8* mac = e1000_mac();

    if (!s_present) {
        return 0;
    }

    /*
     * Build a broadcast Ethernet frame with a private ethertype so the
     * transmit path is exercised without depending on ARP/IP being up.
     */
    frame[0] = 0xFF;
    frame[1] = 0xFF;
    frame[2] = 0xFF;
    frame[3] = 0xFF;
    frame[4] = 0xFF;
    frame[5] = 0xFF;

    frame[6] = mac[0];
    frame[7] = mac[1];
    frame[8] = mac[2];
    frame[9] = mac[3];
    frame[10] = mac[4];
    frame[11] = mac[5];

    frame[12] = (u8)(E1000_TEST_ETHER_TYPE >> 8);
    frame[13] = (u8)(E1000_TEST_ETHER_TYPE & 0xFFu);

    k_memset(&frame[14], 0, sizeof(frame) - 14u);
    k_memcpy(&frame[14], payload, sizeof(payload) - 1u);

    return e1000_send(frame, sizeof(frame));
}
