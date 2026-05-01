#ifndef USER_ERRNO_WRAPPER_H
#define USER_ERRNO_WRAPPER_H

extern int errno;

#define EINTR 4
#define EBADF 9
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define EINVAL 22
#define EPIPE 32
#define ENAMETOOLONG 36
#define ENOTEMPTY 39
#define EPROTO 71
#define EMSGSIZE 90
#define EADDRINUSE 98
#define ECONNRESET 104
#define ETIMEDOUT 110
#define EOVERFLOW 75
#define ENOENT 2
#define ENOTDIR 20
#define EISDIR 21

#endif
