#include "string.h"
#include "errno.h"

char* strerror(int errnum) {
    switch (errnum) {
        case EPERM: return "operation not permitted";
        case ENOENT: return "no such file or directory";
        case ESRCH: return "no such process";
        case EINTR: return "interrupted system call";
        case EIO: return "input/output error";
        case EBADF: return "bad file descriptor";
        case ECHILD: return "no child processes";
        case EAGAIN: return "resource temporarily unavailable";
        case ENOMEM: return "out of memory";
        case EACCES: return "permission denied";
        case EFAULT: return "bad address";
        case EBUSY: return "device or resource busy";
        case EEXIST: return "file exists";
        case ENOTDIR: return "not a directory";
        case EISDIR: return "is a directory";
        case EINVAL: return "invalid argument";
        case ENFILE: return "file table full";
        case EFBIG: return "file too large";
        case ESPIPE: return "illegal seek";
        case EPIPE: return "broken pipe";
        case ERANGE: return "numerical result out of range";
        case ENOSYS: return "function not implemented";
        case ENOTEMPTY: return "directory not empty";
        case ENOTTY: return "inappropriate ioctl for device";
        case EPROTO: return "protocol error";
        case EOVERFLOW: return "value too large";
        case EMSGSIZE: return "message too long";
        case ENETUNREACH: return "network unreachable";
        case ECONNRESET: return "connection reset";
        case EADDRINUSE: return "address already in use";
        case EISCONN: return "socket is connected";
        case ETIMEDOUT: return "connection timed out";
        case ECONNREFUSED: return "connection refused";
        case EHOSTUNREACH: return "host unreachable";
        case EALREADY: return "operation already in progress";
        case EINPROGRESS: return "operation now in progress";
        case ENAMETOOLONG: return "file name too long";
        default: return "unknown error";
    }
}

void bcopy(const void* src, void* dst, size_t len) {
    memmove(dst, src, len);
}

void bzero(void* dst, size_t len) {
    memset(dst, 0, len);
}

int bcmp(const void* a, const void* b, size_t len) {
    return memcmp(a, b, len);
}

int strncasecmp(const char* a, const char* b, size_t n) {
    return strnicmp(a, b, n);
}
