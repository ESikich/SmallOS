#ifndef UAPI_ERRNO_H
#define UAPI_ERRNO_H

/*
 * Shared SmallOS errno values. Syscalls return negative errno values on
 * failure; user-space POSIX wrappers translate them to -1 plus errno.
 */

#define EPERM          1
#define ENOENT         2
#define EINTR          4
#define EIO            5
#define EAGAIN        11
#define EBADF          9
#define ENOMEM        12
#define EACCES        13
#define EFAULT        14
#define EBUSY         16
#define EEXIST        17
#define ENOTDIR       20
#define EISDIR        21
#define EINVAL        22
#define ENFILE        23
#define EFBIG         27
#define EPIPE         32
#define ENOSYS        38
#define ENOTEMPTY     39
#define EPROTO        71
#define EOVERFLOW     75
#define EMSGSIZE      90
#define EADDRINUSE    98
#define ECONNRESET   104
#define ETIMEDOUT    110
#define ENAMETOOLONG  36

#define EWOULDBLOCK EAGAIN

#endif /* UAPI_ERRNO_H */
