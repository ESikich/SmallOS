#ifndef USER_LIB_H
#define USER_LIB_H

#include "user_syscall.h"

typedef unsigned int uint32_t;

static inline uint32_t str_len(const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static inline void u_puts(const char* s) {
    sys_write(s, str_len(s));
}

static inline void u_putc(char c) {
    sys_putc(c);
}

static inline void put_uint_dec(void (*outc)(char), uint32_t value) {
    char buf[16];
    int i = 0;

    if (value == 0) {
        outc('0');
        return;
    }

    while (value > 0) {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }

    while (i > 0) {
        outc(buf[--i]);
    }
}

static inline void u_put_uint(uint32_t value) {
    put_uint_dec(u_putc, value);
}

/*
 * u_readline(buf, maxlen)
 *
 * Reads one line of keyboard input (up to maxlen-1 bytes) into buf.
 * Stops at newline or when maxlen-1 bytes have been read.
 * Always null-terminates buf.
 * Returns the number of characters read (not counting the null terminator).
 */
static inline int u_readline(char* buf, uint32_t maxlen) {
    if (maxlen == 0) return 0;
    if (maxlen == 1) { buf[0] = '\0'; return 0; }

    int n = sys_read(buf, maxlen - 1);
    if (n < 0) n = 0;

    /* Strip trailing newline if present, but count it in n so caller
       knows a full line was received. */
    if (n > 0 && buf[n - 1] == '\n') {
        buf[n - 1] = '\0';
    } else {
        buf[n] = '\0';
    }

    return n;
}

#endif