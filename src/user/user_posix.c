#include "user_stdio.h"
#include "sys/stat.h"
#include "sys/socket.h"
#include "dirent.h"
#include "poll.h"
#include "time.h"
#include "sys/time.h"
#include "fcntl.h"
#include "errno.h"

int errno = 0;

static int errno_from_raw(int raw) {
    if (raw < 0) {
        errno = -raw;
        return -1;
    }
    return raw;
}

static void set_errno(int value) {
    errno = value;
}

static uint32_t open_flags_to_mode(int flags) {
    uint32_t mode = 0;
    int accmode = flags & 0x3;

    if (accmode == O_WRONLY) {
        mode |= SYS_OPEN_MODE_WRITE;
    } else if (accmode == O_RDWR) {
        mode |= SYS_OPEN_MODE_READ | SYS_OPEN_MODE_WRITE;
    } else {
        mode |= SYS_OPEN_MODE_READ;
    }

    if (flags & O_CREAT) {
        mode |= SYS_OPEN_MODE_CREATE;
    }
    if (flags & O_TRUNC) {
        mode |= SYS_OPEN_MODE_TRUNC;
    }
    if (flags & O_APPEND) {
        mode |= SYS_OPEN_MODE_APPEND;
    }

    return mode;
}

int open(const char* path, int flags, ...) {
    return errno_from_raw(sys_open_mode(path, open_flags_to_mode(flags)));
}

int close(int fd) {
    return errno_from_raw(sys_close(fd));
}

int read(int fd, void* buf, unsigned int len) {
    return errno_from_raw(sys_fread(fd, (char*)buf, len));
}

int write(int fd, const void* buf, unsigned int len) {
    return errno_from_raw(sys_writefd(fd, (const char*)buf, len));
}

int lseek(int fd, int offset, int whence) {
    return errno_from_raw(sys_lseek(fd, offset, whence));
}

int unlink(const char* path) {
    return errno_from_raw(sys_unlink(path));
}

int rename(const char* src, const char* dst) {
    return errno_from_raw(sys_rename(src, dst));
}

int mkdir(const char* path, unsigned int mode) {
    return errno_from_raw(sys_mkdir(path, mode));
}

int rmdir(const char* path) {
    return errno_from_raw(sys_rmdir(path));
}

int stat(const char* path, struct stat* st) {
    uint32_t size = 0;
    int is_dir = 0;
    if (!st) {
        set_errno(EFAULT);
        return -1;
    }
    if (errno_from_raw(sys_stat(path, &size, &is_dir)) < 0) {
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_size = (long)size;
    st->st_mode = is_dir ? S_IFDIR : S_IFREG;
    return 0;
}

int fstat(int fd, struct stat* st) {
    (void)fd;
    if (!st) {
        set_errno(EFAULT);
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG;
    return 0;
}

int lstat(const char* path, struct stat* st) {
    return stat(path, st);
}

int access(const char* path, int mode) {
    uint32_t size = 0;
    int is_dir = 0;
    (void)mode;
    return errno_from_raw(sys_stat(path, &size, &is_dir));
}

int remove(const char* path) {
    return unlink(path);
}

int execvp(const char* file, char* const argv[]) {
    (void)file;
    (void)argv;
    set_errno(ENOSYS);
    return -1;
}

int socket(int domain, int type, int protocol) {
    return errno_from_raw(sys_socket(domain, type, protocol));
}

int bind(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    return errno_from_raw(sys_bind(fd, addr, addrlen));
}

int listen(int fd, int backlog) {
    return errno_from_raw(sys_listen(fd, backlog));
}

int accept(int fd, struct sockaddr* addr, socklen_t* addrlen) {
    return errno_from_raw(sys_accept(fd, addr, addrlen));
}

int connect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    return errno_from_raw(sys_connect(fd, addr, addrlen));
}

int send(int fd, const void* buf, unsigned int len) {
    return errno_from_raw(sys_send(fd, buf, len));
}

int recv(int fd, void* buf, unsigned int len) {
    return errno_from_raw(sys_recv(fd, buf, len));
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    return errno_from_raw(sys_poll(fds, nfds, timeout));
}

char* realpath(const char* path, char* resolved_path) {
    if (!path) return 0;
    if (!resolved_path) {
        resolved_path = (char*)malloc(strlen(path) + 1u);
        if (!resolved_path) return 0;
    }
    strcpy(resolved_path, path);
    return resolved_path;
}

char* strerror(int errnum) {
    switch (errnum) {
        case EPERM: return "operation not permitted";
        case ENOENT: return "no such file or directory";
        case EIO: return "input/output error";
        case EBADF: return "bad file descriptor";
        case ENOMEM: return "out of memory";
        case EACCES: return "permission denied";
        case EFAULT: return "bad address";
        case EINVAL: return "invalid argument";
        case ENOSYS: return "function not implemented";
        case ENOTDIR: return "not a directory";
        case EISDIR: return "is a directory";
        case ENFILE: return "file table full";
        case ENAMETOOLONG: return "file name too long";
        case EFBIG: return "file too large";
        default: return "unknown error";
    }
}

char* getcwd(char* buf, unsigned int size) {
    if (!buf || size == 0) {
        set_errno(EFAULT);
        return 0;
    }
    return errno_from_raw(sys_getcwd(buf, size)) < 0 ? 0 : buf;
}

int chdir(const char* path) {
    return errno_from_raw(sys_chdir(path));
}

int setsockopt(int fd, int level, int optname, const void* optval, unsigned int optlen) {
    return errno_from_raw(sys_setsockopt(fd, level, optname, optval, (socklen_t)optlen));
}

int getsockname(int fd, struct sockaddr* addr, socklen_t* addrlen) {
    return errno_from_raw(sys_getsockname(fd, addr, addrlen));
}

time_t time(time_t* out) {
    time_t now = (time_t)(sys_get_ticks() / SMALLOS_TIMER_HZ);
    if (out) {
        *out = now;
    }
    return now;
}

struct tm* localtime(const time_t* timep) {
    static struct tm t;
    time_t v = timep ? *timep : time(0);
    memset(&t, 0, sizeof(t));
    t.tm_year = 70 + (int)(v / 31536000u);
    t.tm_mon = 0;
    t.tm_mday = 1;
    return &t;
}

int gettimeofday(struct timeval* tv, struct timezone* tz) {
    if (!tv) {
        set_errno(EFAULT);
        return -1;
    }
    unsigned int ticks = sys_get_ticks();
    tv->tv_sec = (long)(ticks / SMALLOS_TIMER_HZ);
    tv->tv_usec = (long)((ticks % SMALLOS_TIMER_HZ) *
                         (SMALLOS_US_PER_SECOND / SMALLOS_TIMER_HZ));
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}

int clock_gettime(int clock_id, struct timespec* ts) {
    unsigned int ticks;
    unsigned int rem;

    if (!ts || clock_id != CLOCK_REALTIME) {
        set_errno(EINVAL);
        return -1;
    }

    ticks = sys_get_ticks();
    rem = ticks % SMALLOS_TIMER_HZ;
    ts->tv_sec = (time_t)(ticks / SMALLOS_TIMER_HZ);
    ts->tv_nsec = (long)(rem * (SMALLOS_NS_PER_SECOND / SMALLOS_TIMER_HZ));
    return 0;
}
