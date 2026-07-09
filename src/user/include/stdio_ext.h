#ifndef USER_STDIO_EXT_H
#define USER_STDIO_EXT_H

#include "stdio.h"

#define FSETLOCKING_QUERY 0
#define FSETLOCKING_INTERNAL 1
#define FSETLOCKING_BYCALLER 2

static inline int __fsetlocking(FILE* stream, int type) {
    (void)stream;
    (void)type;
    return FSETLOCKING_INTERNAL;
}

#endif /* USER_STDIO_EXT_H */
