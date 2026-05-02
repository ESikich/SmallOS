#ifndef USER_ERRNO_WRAPPER_H
#define USER_ERRNO_WRAPPER_H

#include "uapi_errno.h"

extern int errno;

#define EINTR        4
#define EEXIST      17
#define EPIPE       32
#define ENOTEMPTY   39
#define EPROTO      71
#define EOVERFLOW   75
#define EMSGSIZE    90
#define EADDRINUSE  98
#define ECONNRESET 104
#define ETIMEDOUT  110

#endif
