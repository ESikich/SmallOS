#ifndef USER_ARPA_INET_H
#define USER_ARPA_INET_H

#include "uapi_socket.h"
#include "../stddef.h"

#define INET_ADDRSTRLEN 16

static inline unsigned short htons(unsigned short value) {
    return (unsigned short)((value << 8) | (value >> 8));
}

static inline unsigned short ntohs(unsigned short value) {
    return htons(value);
}

static inline unsigned int htonl(unsigned int value) {
    return ((value & 0x000000FFu) << 24)
         | ((value & 0x0000FF00u) << 8)
         | ((value & 0x00FF0000u) >> 8)
         | ((value & 0xFF000000u) >> 24);
}

static inline unsigned int ntohl(unsigned int value) {
    return htonl(value);
}

static inline unsigned int inet_addr(const char* text) {
    unsigned int parts[4];
    unsigned int part = 0;
    unsigned int value = 0;
    int saw_digit = 0;

    if (!text) {
        return 0xFFFFFFFFu;
    }

    for (const char* p = text; ; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            value = value * 10u + (unsigned int)(c - '0');
            if (value > 255u) {
                return 0xFFFFFFFFu;
            }
            saw_digit = 1;
            continue;
        }

        if (c == '.' || c == '\0') {
            if (!saw_digit || part >= 4u) {
                return 0xFFFFFFFFu;
            }
            parts[part++] = value;
            value = 0u;
            saw_digit = 0;
            if (c == '\0') {
                break;
            }
            continue;
        }

        return 0xFFFFFFFFu;
    }

    if (part != 4u) {
        return 0xFFFFFFFFu;
    }

    return htonl((parts[0] << 24)
               | (parts[1] << 16)
               | (parts[2] << 8)
               | parts[3]);
}

static inline char* inet_ntoa(struct in_addr in) {
    static char bufs[4][INET_ADDRSTRLEN];
    static unsigned int slot = 0;
    char* out = bufs[slot++ & 3u];
    unsigned int host = ntohl(in.s_addr);
    unsigned int parts[4] = {
        (host >> 24) & 0xFFu,
        (host >> 16) & 0xFFu,
        (host >> 8) & 0xFFu,
        host & 0xFFu
    };
    unsigned int pos = 0;

    for (unsigned int i = 0; i < 4u; i++) {
        char tmp[4];
        unsigned int n = 0;
        unsigned int v = parts[i];
        if (v >= 100u) tmp[n++] = (char)('0' + (v / 100u));
        if (v >= 10u) tmp[n++] = (char)('0' + ((v / 10u) % 10u));
        tmp[n++] = (char)('0' + (v % 10u));
        for (unsigned int j = 0; j < n && pos + 1u < INET_ADDRSTRLEN; j++) {
            out[pos++] = tmp[j];
        }
        if (i + 1u < 4u && pos + 1u < INET_ADDRSTRLEN) {
            out[pos++] = '.';
        }
    }
    out[pos] = '\0';
    return out;
}

static inline const char* inet_ntop(int af, const void* src, char* dst, socklen_t size) {
    if (af != AF_INET || !src || !dst || size < INET_ADDRSTRLEN) {
        return 0;
    }
    struct in_addr in;
    in.s_addr = ((const struct in_addr*)src)->s_addr;
    const char* text = inet_ntoa(in);
    unsigned int i = 0;
    while (text[i] && i + 1u < size) {
        dst[i] = text[i];
        i++;
    }
    dst[i] = '\0';
    return dst;
}

#endif /* USER_ARPA_INET_H */
