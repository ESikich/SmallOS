#ifndef USER_BYTESWAP_H
#define USER_BYTESWAP_H

#define bswap_16(x) __builtin_bswap16((unsigned short)(x))
#define bswap_32(x) __builtin_bswap32((unsigned int)(x))
#define bswap_64(x) __builtin_bswap64((unsigned long long)(x))

#endif /* USER_BYTESWAP_H */
