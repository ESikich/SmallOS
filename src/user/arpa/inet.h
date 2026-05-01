#ifndef USER_ARPA_INET_H
#define USER_ARPA_INET_H

#include "uapi_socket.h"

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

#endif /* USER_ARPA_INET_H */
