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

#endif
