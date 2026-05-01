#include "arp.h"

#include "e1000.h"
#include "../kernel/klib.h"
#include "terminal.h"

#define ARP_ETHERTYPE      0x0806u
#define ARP_HTYPE_ETHERNET 0x0001u
#define ARP_PTYPE_IPV4     0x0800u
#define ARP_OP_REQUEST     0x0001u
#define ARP_OP_REPLY       0x0002u

#define ARP_FRAME_SIZE     42u
#define ARP_MAX_POLL_COUNT 200u

static void arp_write_u16_be(u8* buf, u32 off, u16 value) {
    buf[off] = (u8)(value >> 8);
    buf[off + 1] = (u8)(value & 0xFFu);
}

static void arp_write_u32_be(u8* buf, u32 off, u32 value) {
    buf[off]     = (u8)((value >> 24) & 0xFFu);
    buf[off + 1] = (u8)((value >> 16) & 0xFFu);
    buf[off + 2] = (u8)((value >> 8) & 0xFFu);
    buf[off + 3] = (u8)(value & 0xFFu);
}

static u16 arp_read_u16_be(const u8* buf, u32 off) {
    return (u16)(((u16)buf[off] << 8) | (u16)buf[off + 1]);
}

static u32 arp_read_u32_be(const u8* buf, u32 off) {
    return ((u32)buf[off] << 24)
         | ((u32)buf[off + 1] << 16)
         | ((u32)buf[off + 2] << 8)
         | (u32)buf[off + 3];
}

void arp_print_ip(u32 ip) {
    terminal_put_uint((ip >> 24) & 0xFFu);
    terminal_putc('.');
    terminal_put_uint((ip >> 16) & 0xFFu);
    terminal_putc('.');
    terminal_put_uint((ip >> 8) & 0xFFu);
    terminal_putc('.');
    terminal_put_uint(ip & 0xFFu);
}

static void arp_print_mac(const u8* mac) {
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned int i = 0; i < 6; i++) {
        terminal_putc(hex[mac[i] >> 4]);
        terminal_putc(hex[mac[i] & 0xF]);
        if (i != 5) {
            terminal_putc(':');
        }
    }
}

static void arp_wait(void) {
    /*
     * Give the guest a chance to advance timers and device emulation
     * instead of burning the whole poll window in a tight busy loop.
     */
    __asm__ __volatile__("hlt");
}

static int arp_send_request(u32 sender_ip, u32 target_ip) {
    u8 frame[ARP_FRAME_SIZE];
    const u8* src_mac = e1000_mac();

    k_memset(frame, 0, sizeof(frame));

    /* Ethernet broadcast destination. */
    for (unsigned int i = 0; i < 6; i++) {
        frame[i] = 0xFF;
    }

    /* Source MAC from the NIC. */
    for (unsigned int i = 0; i < 6; i++) {
        frame[6 + i] = src_mac[i];
    }

    /* Ethernet type = ARP. */
    arp_write_u16_be(frame, 12, ARP_ETHERTYPE);

    /* ARP payload. */
    arp_write_u16_be(frame, 14, ARP_HTYPE_ETHERNET);
    arp_write_u16_be(frame, 16, ARP_PTYPE_IPV4);
    frame[18] = 6;
    frame[19] = 4;
    arp_write_u16_be(frame, 20, ARP_OP_REQUEST);

    for (unsigned int i = 0; i < 6; i++) {
        frame[22 + i] = src_mac[i];
    }

    arp_write_u32_be(frame, 28, sender_ip);
    /* target MAC stays zero */
    arp_write_u32_be(frame, 38, target_ip);

    return e1000_send(frame, sizeof(frame));
}

int arp_resolve(u32 sender_ip, u32 target_ip, u8* out_mac) {
    u8 frame[1600];
    u32 len = 0;

    if (!out_mac) {
        return 0;
    }

    if (!arp_send_request(sender_ip, target_ip)) {
        return 0;
    }

    for (unsigned int i = 0; i < ARP_MAX_POLL_COUNT; i++) {
        if (!e1000_recv(frame, sizeof(frame), &len)) {
            arp_wait();
            continue;
        }

        if (len < 42u) {
            arp_wait();
            continue;
        }

        if (arp_read_u16_be(frame, 12) != ARP_ETHERTYPE) {
            arp_wait();
            continue;
        }
        if (arp_read_u16_be(frame, 14) != ARP_HTYPE_ETHERNET) {
            arp_wait();
            continue;
        }
        if (arp_read_u16_be(frame, 16) != ARP_PTYPE_IPV4) {
            arp_wait();
            continue;
        }
        if (frame[18] != 6 || frame[19] != 4) {
            arp_wait();
            continue;
        }
        if (arp_read_u16_be(frame, 20) != ARP_OP_REPLY) {
            arp_wait();
            continue;
        }
        if (arp_read_u32_be(frame, 28) != target_ip) {
            arp_wait();
            continue;
        }
        if (arp_read_u32_be(frame, 38) != sender_ip) {
            arp_wait();
            continue;
        }

        for (unsigned int j = 0; j < 6; j++) {
            out_mac[j] = frame[22 + j];
        }
        return 1;
    }

    return 0;
}
