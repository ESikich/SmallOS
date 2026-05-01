#include "user_stdio.h"
#include "sys/stat.h"
#include "sys/socket.h"
#include "poll.h"
#include "time.h"
#include "sys/time.h"

int errno = 0;

static int open_flags_to_mode(int flags) {
    if ((flags & 0x3) == 0x1 || (flags & 0x3) == 0x2) {
        return 1;
    }
    return 0;
}

int open(const char* path, int flags, ...) {
    if (open_flags_to_mode(flags)) {
        return sys_open_write(path);
    }
    return sys_open(path);
}

int close(int fd) {
    return sys_close(fd);
}

int read(int fd, void* buf, unsigned int len) {
    return sys_fread(fd, (char*)buf, len);
}

int write(int fd, const void* buf, unsigned int len) {
    if (fd == 1 || fd == 2) {
        sys_write((const char*)buf, len);
        return (int)len;
    }
    return sys_writefd(fd, (const char*)buf, len);
}

int lseek(int fd, int offset, int whence) {
    return sys_lseek(fd, offset, whence);
}

int unlink(const char* path) {
    return sys_unlink(path);
}

int rename(const char* src, const char* dst) {
    return sys_rename(src, dst);
}

int stat(const char* path, struct stat* st) {
    uint32_t size = 0;
    int is_dir = 0;
    if (!st) {
        return -1;
    }
    if (sys_stat(path, &size, &is_dir) < 0) {
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_size = (long)size;
    st->st_mode = is_dir ? S_IFDIR : S_IFREG;
    return 0;
}

int fstat(int fd, struct stat* st) {
    (void)fd;
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG;
    return 0;
}

int access(const char* path, int mode) {
    uint32_t size = 0;
    int is_dir = 0;
    (void)mode;
    return sys_stat(path, &size, &is_dir);
}

int remove(const char* path) {
    return unlink(path);
}

int execvp(const char* file, char* const argv[]) {
    (void)file;
    (void)argv;
    return -1;
}

int socket(int domain, int type, int protocol) {
    return sys_socket(domain, type, protocol);
}

int bind(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    return sys_bind(fd, addr, addrlen);
}

int listen(int fd, int backlog) {
    return sys_listen(fd, backlog);
}

int accept(int fd, struct sockaddr* addr, socklen_t* addrlen) {
    return sys_accept(fd, addr, addrlen);
}

int connect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    return sys_connect(fd, addr, addrlen);
}

int send(int fd, const void* buf, unsigned int len) {
    return sys_send(fd, buf, len);
}

int recv(int fd, void* buf, unsigned int len) {
    return sys_recv(fd, buf, len);
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    return sys_poll(fds, nfds, timeout);
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
    (void)errnum;
    return "error";
}

char* getcwd(char* buf, unsigned int size) {
    if (!buf || size == 0) return 0;
    if (size < 2) return 0;
    buf[0] = '/';
    buf[1] = '\0';
    return buf;
}

int chdir(const char* path) {
    (void)path;
    return 0;
}

time_t time(time_t* out) {
    time_t now = (time_t)(sys_get_ticks() / 100u);
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
    if (!tv) return -1;
    tv->tv_sec = (long)(sys_get_ticks() / 100u);
    tv->tv_usec = (long)((sys_get_ticks() % 100u) * 10000u);
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}
