#ifndef DIAG_UTIL_H
#define DIAG_UTIL_H

#include "user_lib.h"

static inline int diag_streq(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static inline int diag_parse_uint(const char* s, unsigned int* out) {
    unsigned int value = 0;

    if (!s || !*s || !out) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        value = value * 10u + (unsigned int)(*s - '0');
        s++;
    }
    *out = value;
    return 1;
}

static inline int diag_parse_ip(const char* text, unsigned int* out_ip) {
    unsigned int parts[4];
    unsigned int part = 0;
    unsigned int value = 0;
    int saw_digit = 0;

    if (!text || !out_ip) return 0;
    for (const char* p = text;; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            value = value * 10u + (unsigned int)(c - '0');
            if (value > 255u) return 0;
            saw_digit = 1;
            continue;
        }
        if (c == '.' || c == 0) {
            if (!saw_digit || part >= 4u) return 0;
            parts[part++] = value;
            value = 0;
            saw_digit = 0;
            if (c == 0) break;
            continue;
        }
        return 0;
    }

    if (part != 4u) return 0;
    *out_ip = (parts[0] << 24) | (parts[1] << 16) |
              (parts[2] << 8) | parts[3];
    return 1;
}

static inline void diag_put_hex_digit(unsigned int v) {
    u_putc((char)(v < 10u ? '0' + v : 'A' + (v - 10u)));
}

static inline void diag_put_hex32(unsigned int value) {
    int started = 0;

    u_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        unsigned int digit = (value >> shift) & 0xFu;
        if (digit || started || shift == 0) {
            started = 1;
            diag_put_hex_digit(digit);
        }
    }
}

static inline void diag_put_u64_hex(unsigned long long value) {
    unsigned int high = (unsigned int)(value >> 32);
    unsigned int low = (unsigned int)value;

    if (high) {
        diag_put_hex32(high);
        u_putc('_');
        diag_put_hex32(low);
    } else {
        diag_put_hex32(low);
    }
}

static inline void diag_put_ip(unsigned int ip) {
    u_put_uint((ip >> 24) & 0xFFu);
    u_putc('.');
    u_put_uint((ip >> 16) & 0xFFu);
    u_putc('.');
    u_put_uint((ip >> 8) & 0xFFu);
    u_putc('.');
    u_put_uint(ip & 0xFFu);
}

static inline void diag_put_mac(const unsigned char mac[6]) {
    for (unsigned int i = 0; i < 6u; i++) {
        diag_put_hex_digit((mac[i] >> 4) & 0xFu);
        diag_put_hex_digit(mac[i] & 0xFu);
        if (i != 5u) u_putc(':');
    }
}

static inline void diag_put_hex_byte(unsigned int value) {
    diag_put_hex_digit((value >> 4) & 0xFu);
    diag_put_hex_digit(value & 0xFu);
}

static inline void diag_usb_put_addr(const sys_usb_port_entry_t* ent) {
    diag_put_hex_byte(ent->bus);
    u_putc(':');
    diag_put_hex_byte(ent->slot);
    u_putc('.');
    u_putc((char)('0' + (ent->func & 7u)));
}

static inline void diag_usb_put_port_flags(unsigned int status, int ohci) {
    u_puts(" flags=");
    if (status & 0x00000001u) u_puts("conn,");
    if (status & 0x00000002u) u_puts("en,");
    if (status & 0x00000004u) u_puts(ohci ? "suspend," : "change,");
    if (status & 0x00000010u) u_puts("reset,");
    if (ohci && (status & 0x00000100u)) u_puts("power,");
    if (ohci && (status & 0x00000200u)) u_puts("low,");
    if (!ohci && (status & 0x00001000u)) u_puts("power,");
}

static inline void diag_usb_print_ports(const sys_usb_port_snapshot_t* snap) {
    u_puts("usbports: passive port dump\n");
    for (unsigned int i = 0; snap && i < snap->entry_count; i++) {
        const sys_usb_port_entry_t* ent = &snap->entries[i];
        int ohci = ent->prog_if == 0x10u;
        int ehci = ent->prog_if == 0x20u;

        if (ent->kind == SYS_USB_PORT_ENTRY_CONTROLLER) {
            u_puts("usbports: ctrl ");
            diag_usb_put_addr(ent);
            u_puts(" prog=");
            diag_put_hex32(ent->prog_if);
            u_puts(" bar=");
            diag_put_hex32(ent->bar);
            u_putc('\n');
            if (ohci) {
                u_puts("usbports: ohci ports=");
                u_put_uint(ent->port_count);
                u_puts(" rhda=");
                diag_put_hex32(ent->info);
                u_putc('\n');
            } else if (ehci) {
                u_puts("usbports: ehci ports=");
                u_put_uint(ent->port_count);
                u_puts(" caplen=");
                u_put_uint(ent->extra);
                u_puts(" hcs=");
                diag_put_hex32(ent->info);
                u_putc('\n');
            }
        } else if (ent->kind == SYS_USB_PORT_ENTRY_PORT) {
            if (ohci) {
                u_puts("usbports: ohci port=");
            } else if (ehci) {
                u_puts("usbports: ehci port=");
            } else {
                u_puts("usbports: port=");
            }
            u_put_uint(ent->port);
            u_puts(" st=");
            diag_put_hex32(ent->status);
            diag_usb_put_port_flags(ent->status, ohci);
            u_putc('\n');
        }
    }
    if (snap && snap->truncated) {
        u_puts("usbports: truncated\n");
    }
}

static inline void diag_usb_print_dry_run_candidates(const sys_usb_port_snapshot_t* snap) {
    unsigned int candidates = 0;

    u_puts("usbdiag: dry-run connected non-low OHCI ports\n");
    for (unsigned int i = 0; snap && i < snap->entry_count; i++) {
        const sys_usb_port_entry_t* ent = &snap->entries[i];
        if (ent->kind != SYS_USB_PORT_ENTRY_PORT || ent->prog_if != 0x10u) {
            continue;
        }
        if (!(ent->status & 0x00000001u)) {
            continue;
        }
        if (ent->status & 0x00000200u) {
            u_puts("usbdiag: skip low-speed port=");
            u_put_uint(ent->port);
            u_puts(" st=");
            diag_put_hex32(ent->status);
            u_putc('\n');
            continue;
        }
        candidates++;
        u_puts("usbdiag: peek port=");
        u_put_uint(ent->port);
        u_puts(" st=");
        diag_put_hex32(ent->status);
        u_puts(" dry-run no-reset no-dma\n");
    }
    if (candidates == 0) {
        u_puts("usbdiag: no non-low OHCI candidates\n");
    }
}

#endif
