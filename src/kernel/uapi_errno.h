#ifndef UAPI_ERRNO_H
#define UAPI_ERRNO_H

/*
 * Shared SmallOS errno values. Syscalls return negative errno values on
 * failure; user-space POSIX wrappers translate them to -1 plus errno.
 */

#define EPERM          1
#define ENOENT         2
#define EIO            5
#define EBADF          9
#define ENOMEM        12
#define EACCES        13
#define EFAULT        14
#define ENOTDIR       20
#define EISDIR        21
#define EINVAL        22
#define ENFILE        23
#define EFBIG         27
#define ENOSYS        38
#define ENAMETOOLONG  36

#endif /* UAPI_ERRNO_H */
