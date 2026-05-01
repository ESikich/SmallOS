#ifndef USER_LIB_H
#define USER_LIB_H

#include "user_syscall.h"
#include "user_file.h"

typedef unsigned int uint32_t;
typedef unsigned int size_t;

#define USER_HEAP_BASE 0x10000000u
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

static inline uint32_t str_len(const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

#ifndef USER_LIB_NO_STRING_IMPL
static inline void* memset(void* dst, int value, size_t len) {
    unsigned char* p = (unsigned char*)dst;
    unsigned char c = (unsigned char)value;
    for (size_t i = 0; i < len; i++) {
        p[i] = c;
    }
    return dst;
}

static inline void* memcpy(void* dst, const void* src, size_t len) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return dst;
}

static inline void* memmove(void* dst, const void* src, size_t len) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || len == 0) {
        return dst;
    }
    if (d < s) {
        for (size_t i = 0; i < len; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = len; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

static inline int memcmp(const void* a, const void* b, size_t len) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    for (size_t i = 0; i < len; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}
#endif

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

void* malloc(size_t size);
void  free(void* ptr);
void* realloc(void* ptr, size_t size);
void* calloc(size_t nmemb, size_t size);

static inline int u_open_write(const char* name) {
    return sys_open_write(name);
}

static inline int u_writefd(int fd, const char* buf, uint32_t len) {
    return sys_writefd(fd, buf, len);
}

static inline int u_lseek(int fd, int offset, int whence) {
    return sys_lseek(fd, offset, whence);
}

static inline int u_unlink(const char* path) {
    return sys_unlink(path);
}

static inline int u_rename(const char* src, const char* dst) {
    return sys_rename(src, dst);
}

static inline int u_stat(const char* path, uint32_t* out_size, int* out_is_dir) {
    return sys_stat(path, out_size, out_is_dir);
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
