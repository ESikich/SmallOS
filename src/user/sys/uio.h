#ifndef USER_SYS_UIO_H
#define USER_SYS_UIO_H

#include "../stddef.h"
#include "types.h"

struct iovec {
    void* iov_base;
    size_t iov_len;
};

ssize_t writev(int fd, const struct iovec* iov, int iovcnt);

#endif /* USER_SYS_UIO_H */
