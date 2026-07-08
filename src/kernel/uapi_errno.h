#ifndef UAPI_ERRNO_H
#define UAPI_ERRNO_H

/*
 * Shared SmallOS errno values. Syscalls return negative errno values on
 * failure; user-space POSIX wrappers translate them to -1 plus errno.
 */

#define EPERM          1
#define ENOENT         2
#define ESRCH          3
#define EINTR          4
#define EIO            5
#define ENOEXEC        8
#define EAGAIN        11
#define ECHILD        10
#define EBADF          9
#define ENOMEM        12
#define EACCES        13
#define EFAULT        14
#define EBUSY         16
#define EEXIST        17
#define EXDEV         18
#define ENODEV        19
#define ENXIO          6
#define ENOTDIR       20
#define EISDIR        21
#define EINVAL        22
#define ENFILE        23
#define EFBIG         27
#define ESPIPE        29
#define EPIPE         32
#define EDOM          33
#define ERANGE        34
#define EROFS         30
#define EDEADLK       35
#define ENOSYS        38
#define ENOTEMPTY     39
#define ELOOP         40
#define ENOTTY        25
#define ETXTBSY       26
#define EMLINK        31
#define EPROTO        71
#define ENOPROTOOPT   92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EAFNOSUPPORT  97
#define EPFNOSUPPORT  96
#define EOVERFLOW     75
#define EMSGSIZE      90
#define EPROTOTYPE    91
#define EOPNOTSUPP    95
#define EDESTADDRREQ  89
#define ENETDOWN     100
#define ENETUNREACH  101
#define ENETRESET    102
#define ECONNABORTED 103
#define EADDRINUSE    98
#define EADDRNOTAVAIL 99
#define ENOBUFS      105
#define EISCONN      106
#define ENOTCONN     107
#define ESHUTDOWN    108
#define ETOOMANYREFS 109
#define ECONNRESET   104
#define ETIMEDOUT    110
#define ECONNREFUSED 111
#define EHOSTDOWN    112
#define EHOSTUNREACH 113
#define EALREADY     114
#define EINPROGRESS  115
#define ENAMETOOLONG  36
#define EUSERS        87
#define EDQUOT       122
#define ESTALE       116
#define EREMOTE       66
#define EBADRPC       72
#define ERPCMISMATCH  73
#define EPROGUNAVAIL  74
#define EPROGMISMATCH 75
#define EPROCUNAVAIL  76
#define ENOLCK        37
#define EFTYPE        79
#define EAUTH         80
#define ENEEDAUTH     81
#define ENOTBLK       15
#define ENOSTR        60
#define ETIME         62
#define ENOSR         63
#define ENOMSG        42
#define EBADMSG       77
#define EIDRM         43
#define ENONET        64
#define ENOLINK       67
#define EADV          68
#define ESRMNT        69
#define ECOMM         70
#define EMULTIHOP     72
#define EREMCHG       78
#define E2BIG          7
#define EMFILE        24

#define EWOULDBLOCK EAGAIN

#endif /* UAPI_ERRNO_H */
