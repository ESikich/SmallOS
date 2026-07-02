#include "stdio.h"
#include "string.h"
#include "sys/stat.h"
#include "sys/socket.h"
#include "dirent.h"
#include "poll.h"
#include "time.h"
#include "sys/time.h"
#include "sys/epoll.h"
#include "sys/timerfd.h"
#include "sys/signalfd.h"
#include "sys/uio.h"
#include "sys/mman.h"
#include "sys/sendfile.h"
#include "sys/wait.h"
#include "sys/ioctl.h"
#include "sys/resource.h"
#include "sys/prctl.h"
#include "sys/utsname.h"
#include "sys/vfs.h"
#include "sys/statvfs.h"
#include "sys/sysinfo.h"
#include "libgen.h"
#include "fnmatch.h"
#include "netdb.h"
#include "net/if.h"
#include "pwd.h"
#include "grp.h"
#include "termios.h"
#include "getopt.h"
#include "mntent.h"
#include "regex.h"
#include "arpa/inet.h"
#include "fcntl.h"
#include "unistd.h"
#include "errno.h"
#include "signal.h"
#include "stdlib.h"
#include "stdint.h"
#include "stdarg.h"
#include "user_syscall.h"
#include "uapi_time.h"

int errno = 0;
int h_errno = 0;
char* __smallos_empty_env[] = { 0 };
char** environ = __smallos_empty_env;
static int s_environ_owned = 0;
static sighandler_t s_signal_handlers[32];
char* optarg = 0;
int optind = 1;
int opterr = 1;
int optopt = 0;

static void set_errno(int value);

void __smallos_set_environ(char** envp) {
    environ = envp ? envp : __smallos_empty_env;
    s_environ_owned = 0;
}

static int env_name_len(const char* name) {
    int len = 0;
    if (!name || !name[0]) {
        return -1;
    }
    while (name[len]) {
        if (name[len] == '=') {
            return -1;
        }
        len++;
    }
    return len;
}

static int env_entry_matches(const char* entry, const char* name, int name_len) {
    return entry && strncmp(entry, name, (size_t)name_len) == 0 && entry[name_len] == '=';
}

static unsigned int env_count(void) {
    unsigned int count = 0;
    if (!environ) {
        return 0;
    }
    while (environ[count]) {
        count++;
    }
    return count;
}

int putenv(char* string) {
    unsigned int count;
    unsigned int i;
    int name_len = 0;
    char** next;

    if (!string || string[0] == '=') {
        set_errno(EINVAL);
        return -1;
    }
    while (string[name_len] && string[name_len] != '=') {
        name_len++;
    }
    if (string[name_len] != '=') {
        set_errno(EINVAL);
        return -1;
    }

    count = env_count();
    for (i = 0; i < count; i++) {
        if (env_entry_matches(environ[i], string, name_len)) {
            environ[i] = string;
            return 0;
        }
    }

    next = (char**)malloc((count + 2u) * sizeof(char*));
    if (!next) {
        set_errno(ENOMEM);
        return -1;
    }
    for (i = 0; i < count; i++) {
        next[i] = environ[i];
    }
    next[count] = string;
    next[count + 1u] = 0;
    if (s_environ_owned) {
        free(environ);
    }
    environ = next;
    s_environ_owned = 1;
    return 0;
}

int setenv(const char* name, const char* value, int overwrite) {
    int name_len = env_name_len(name);
    size_t value_len;
    char* entry;

    if (name_len < 0) {
        set_errno(EINVAL);
        return -1;
    }
    if (!overwrite && getenv(name)) {
        return 0;
    }
    if (!value) {
        value = "";
    }
    value_len = strlen(value);
    entry = (char*)malloc((size_t)name_len + 1u + value_len + 1u);
    if (!entry) {
        set_errno(ENOMEM);
        return -1;
    }
    memcpy(entry, name, (size_t)name_len);
    entry[name_len] = '=';
    memcpy(entry + name_len + 1, value, value_len + 1u);
    return putenv(entry);
}

int unsetenv(const char* name) {
    int name_len = env_name_len(name);
    unsigned int read_i;
    unsigned int write_i = 0;

    if (name_len < 0) {
        set_errno(EINVAL);
        return -1;
    }
    if (!environ) {
        return 0;
    }
    for (read_i = 0; environ[read_i]; read_i++) {
        if (!env_entry_matches(environ[read_i], name, name_len)) {
            environ[write_i++] = environ[read_i];
        }
    }
    environ[write_i] = 0;
    return 0;
}

int getopt(int argc, char* const argv[], const char* optstring) {
    static const char* next = 0;
    const char* optdecl;
    int c;
    int wants_arg;
    int optional_arg;

    if (optind == 0) {
        optind = 1;
        next = 0;
    }
    optarg = 0;
    if (!argv || !optstring || optind >= argc) {
        return -1;
    }

    if (!next || !*next) {
        const char* arg = argv[optind];
        if (!arg || arg[0] != '-' || arg[1] == '\0') {
            return -1;
        }
        if (arg[1] == '-' && arg[2] == '\0') {
            optind++;
            return -1;
        }
        next = arg + 1;
    }

    c = (unsigned char)*next++;
    optdecl = strchr(optstring, c);
    if (!optdecl || c == ':') {
        optopt = c;
        if (!*next) {
            optind++;
            next = 0;
        }
        return '?';
    }

    wants_arg = optdecl[1] == ':';
    optional_arg = wants_arg && optdecl[2] == ':';
    if (wants_arg) {
        if (*next) {
            optarg = (char*)next;
            optind++;
            next = 0;
        } else if (!optional_arg && optind + 1 < argc) {
            optarg = argv[++optind];
            optind++;
            next = 0;
        } else {
            if (optional_arg) {
                optind++;
                next = 0;
                return c;
            }
            optopt = c;
            optind++;
            next = 0;
            return optstring[0] == ':' ? ':' : '?';
        }
    } else if (!*next) {
        optind++;
        next = 0;
    }

    return c;
}

int getopt_long(int argc, char* const argv[], const char* optstring,
                const struct option* longopts, int* longindex) {
    (void)longopts;
    if (longindex) {
        *longindex = -1;
    }
    return getopt(argc, argv, optstring);
}

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

char* mktemp(char* template) {
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t len;
    size_t start;
    unsigned int attempt;

    if (!template) {
        set_errno(EINVAL);
        return 0;
    }
    len = strlen(template);
    if (len < 6u || strcmp(template + len - 6u, "XXXXXX") != 0) {
        template[0] = '\0';
        set_errno(EINVAL);
        return template;
    }
    start = len - 6u;
    for (attempt = 0; attempt < 100u; attempt++) {
        unsigned int value = (unsigned int)rand() ^ (attempt * 1103515245u);
        unsigned int i;
        for (i = 0; i < 6u; i++) {
            template[start + i] = alphabet[value % (sizeof(alphabet) - 1u)];
            value = value / (sizeof(alphabet) - 1u);
            value ^= (unsigned int)rand();
        }
        if (access(template, F_OK) != 0 && errno == ENOENT) {
            return template;
        }
    }
    template[0] = '\0';
    set_errno(EEXIST);
    return template;
}

int mkstemp(char* template) {
    char* path = mktemp(template);
    if (!path || !path[0]) {
        return -1;
    }
    return open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
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
    if (flags & O_EXCL) {
        mode |= SYS_OPEN_MODE_EXCL;
    }

    return mode;
}

int open(const char* path, int flags, ...) {
    return errno_from_raw(sys_open_mode(path, open_flags_to_mode(flags)));
}

int creat(const char* path, unsigned mode) {
    return open(path, O_CREAT | O_TRUNC | O_WRONLY, mode);
}

int fcntl(int fd, int cmd, ...) {
    __builtin_va_list ap;
    uint32_t arg = 0;

    if (cmd == F_SETFL || cmd == F_SETFD ||
        cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        __builtin_va_start(ap, cmd);
        arg = (uint32_t)__builtin_va_arg(ap, int);
        __builtin_va_end(ap);
    }

    if (cmd == F_GETFD || cmd == F_SETFD || cmd == F_GETFL || cmd == F_SETFL ||
        cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        return errno_from_raw(sys_fcntl(fd, cmd, arg));
    }

    set_errno(EINVAL);
    return -1;
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

int link(const char* oldpath, const char* newpath) {
    (void)oldpath;
    (void)newpath;
    set_errno(ENOSYS);
    return -1;
}

int symlink(const char* target, const char* linkpath) {
    (void)target;
    (void)linkpath;
    set_errno(ENOSYS);
    return -1;
}

ssize_t readlink(const char* path, char* buf, size_t bufsiz) {
    if (!path || !buf || bufsiz == 0u) {
        set_errno(EINVAL);
        return -1;
    }
    set_errno(ENOSYS);
    return -1;
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

static int path_exists_for_metadata(const char* path) {
    struct stat st;
    return stat(path, &st);
}

static void stat_fill_timespecs(struct stat* st) {
    st->st_atim.tv_sec = st->st_atime;
    st->st_atim.tv_nsec = 0;
    st->st_mtim.tv_sec = st->st_mtime;
    st->st_mtim.tv_nsec = 0;
    st->st_ctim.tv_sec = st->st_ctime;
    st->st_ctim.tv_nsec = 0;
}

int chmod(const char* path, mode_t mode) {
    (void)mode;
    return path_exists_for_metadata(path);
}

int chown(const char* path, uid_t owner, gid_t group) {
    (void)owner;
    (void)group;
    return path_exists_for_metadata(path);
}

int lchown(const char* path, uid_t owner, gid_t group) {
    return chown(path, owner, group);
}

int fchmod(int fd, mode_t mode) {
    struct stat st;
    (void)mode;
    return fstat(fd, &st);
}

int fchown(int fd, uid_t owner, gid_t group) {
    struct stat st;
    (void)owner;
    (void)group;
    return fstat(fd, &st);
}

mode_t umask(mode_t mask) {
    static mode_t current = 0022;
    mode_t old = current;

    current = mask & 0777;
    return old;
}

int utimes(const char* path, const struct timeval times[2]) {
    (void)times;
    return path_exists_for_metadata(path);
}

int utimensat(int dirfd, const char* pathname, const struct timespec times[2], int flags) {
    (void)times;
    if (dirfd != AT_FDCWD) {
        set_errno(ENOSYS);
        return -1;
    }
    if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0) {
        set_errno(EINVAL);
        return -1;
    }
    return path_exists_for_metadata(pathname);
}

int futimens(int fd, const struct timespec times[2]) {
    struct stat st;
    (void)times;
    return fstat(fd, &st);
}

int mknod(const char* path, mode_t mode, dev_t dev) {
    (void)path;
    (void)mode;
    (void)dev;
    set_errno(ENOSYS);
    return -1;
}

int mkfifo(const char* path, mode_t mode) {
    (void)path;
    (void)mode;
    set_errno(ENOSYS);
    return -1;
}

int stat(const char* path, struct stat* st) {
    sys_stat_info_t info;
    if (!st) {
        set_errno(EFAULT);
        return -1;
    }
    if (errno_from_raw(sys_stat_full(path, &info)) < 0) {
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_dev = info.dev;
    st->st_ino = info.ino;
    st->st_nlink = info.nlink;
    st->st_mode = info.mode;
    st->st_uid = info.uid;
    st->st_gid = info.gid;
    st->st_rdev = info.rdev;
    st->st_size = (long)info.size;
    st->st_blksize = (long)info.blksize;
    st->st_blocks = (long)info.blocks;
    st->st_atime = (time_t)info.atime;
    st->st_mtime = (time_t)info.mtime;
    st->st_ctime = (time_t)info.ctime;
    stat_fill_timespecs(st);
    return 0;
}

int fstat(int fd, struct stat* st) {
    sys_stat_info_t info;

    if (!st) {
        set_errno(EFAULT);
        return -1;
    }
    memset(st, 0, sizeof(*st));
    if (errno_from_raw(sys_fstat_full(fd, &info)) < 0) {
        return -1;
    }
    st->st_dev = info.dev;
    st->st_ino = info.ino;
    st->st_nlink = info.nlink;
    st->st_mode = info.mode;
    st->st_uid = info.uid;
    st->st_gid = info.gid;
    st->st_rdev = info.rdev;
    st->st_size = (long)info.size;
    st->st_blksize = (long)info.blksize;
    st->st_blocks = (long)info.blocks;
    st->st_atime = (time_t)info.atime;
    st->st_mtime = (time_t)info.mtime;
    st->st_ctime = (time_t)info.ctime;
    stat_fill_timespecs(st);
    return 0;
}

int lstat(const char* path, struct stat* st) {
    return stat(path, st);
}

int access(const char* path, int mode) {
    uint32_t size = 0;
    int is_dir = 0;
    if ((mode & ~(R_OK | W_OK | X_OK)) != 0) {
        set_errno(EINVAL);
        return -1;
    }
    return errno_from_raw(sys_stat(path, &size, &is_dir));
}

int remove(const char* path) {
    return unlink(path);
}

int pipe(int fds[2]) {
    return errno_from_raw(sys_pipe(fds));
}

int pipe2(int fds[2], int flags) {
    return errno_from_raw(sys_pipe2(fds, flags));
}

int dup(int oldfd) {
    return errno_from_raw(sys_dup(oldfd));
}

int dup2(int oldfd, int newfd) {
    return errno_from_raw(sys_dup2(oldfd, newfd));
}

int dup3(int oldfd, int newfd, int flags) {
    return errno_from_raw(sys_dup3(oldfd, newfd, flags));
}

pid_t fork(void) {
    return (pid_t)errno_from_raw(sys_fork());
}

pid_t vfork(void) {
    return fork();
}

int execve(const char* path, char* const argv[], char* const envp[]) {
    return errno_from_raw(sys_execve(path, argv, envp));
}

int execv(const char* path, char* const argv[]) {
    return execve(path, argv, environ);
}

static int path_has_sep_user(const char* path) {
    if (!path) return 0;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') return 1;
    }
    return 0;
}

static int join_exec_path(char* out, unsigned int out_size, const char* dir, const char* file) {
    unsigned int pos = 0;
    if (!out || !dir || !file || out_size == 0) return 0;
    while (dir[pos]) {
        if (pos + 1 >= out_size) return 0;
        out[pos] = dir[pos];
        pos++;
    }
    if (pos > 0 && out[pos - 1] != '/') {
        if (pos + 1 >= out_size) return 0;
        out[pos++] = '/';
    }
    for (unsigned int i = 0; file[i]; i++) {
        if (pos + 1 >= out_size) return 0;
        out[pos++] = file[i];
    }
    out[pos] = '\0';
    return 1;
}

int execvp(const char* file, char* const argv[]) {
    const char* path_env = getenv("PATH");
    char path[128];
    int last_errno = ENOENT;

    if (!file || !file[0]) {
        set_errno(ENOENT);
        return -1;
    }
    if (path_has_sep_user(file)) {
        return execve(file, argv, 0);
    }
    if (!path_env || !path_env[0]) {
        path_env = ":/bin:/usr/bin:/usr/sbin";
    }

    while (1) {
        char dir[64];
        unsigned int dlen = 0;

        while (path_env[dlen] && path_env[dlen] != ':') {
            if (dlen + 1 >= sizeof(dir)) {
                set_errno(ENAMETOOLONG);
                return -1;
            }
            dir[dlen] = path_env[dlen];
            dlen++;
        }
        dir[dlen] = '\0';

        if (dir[0] == '\0') {
            if (strlen(file) + 1u > sizeof(path)) {
                set_errno(ENAMETOOLONG);
                return -1;
            }
            strcpy(path, file);
        } else if (!join_exec_path(path, sizeof(path), dir, file)) {
            set_errno(ENAMETOOLONG);
            return -1;
        }
        execve(path, argv, environ);
        last_errno = errno;
        if (last_errno != ENOENT) break;
        if (path_env[dlen] == '\0') break;
        path_env += dlen + 1;
    }
    set_errno(last_errno);
    return -1;
}

int execlp(const char* file, const char* arg, ...) {
    char* argv[32];
    unsigned int argc = 0;
    const char* current = arg;
    va_list ap;

    va_start(ap, arg);
    while (current) {
        if (argc + 1u >= sizeof(argv) / sizeof(argv[0])) {
            va_end(ap);
            set_errno(EINVAL);
            return -1;
        }
        argv[argc++] = (char*)current;
        current = va_arg(ap, const char*);
    }
    va_end(ap);
    argv[argc] = 0;
    return execvp(file, argv);
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
    return errno_from_raw(sys_accept4(fd, addr, addrlen, 0));
}

int accept4(int fd, struct sockaddr* addr, socklen_t* addrlen, int flags) {
    return errno_from_raw(sys_accept4(fd, addr, addrlen, flags));
}

int connect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    return errno_from_raw(sys_connect(fd, addr, addrlen));
}

int send(int fd, const void* buf, size_t len, int flags) {
    (void)flags;
    return errno_from_raw(sys_send(fd, buf, len));
}

int sendto(int fd, const void* buf, size_t len, int flags,
           const struct sockaddr* dest_addr, socklen_t addrlen) {
    (void)dest_addr;
    (void)addrlen;
    return send(fd, buf, len, flags);
}

int recv(int fd, void* buf, size_t len, int flags) {
    (void)flags;
    return errno_from_raw(sys_recv(fd, buf, len));
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    return errno_from_raw(sys_poll(fds, nfds, timeout));
}

int usleep(unsigned int usec) {
    const unsigned int usec_per_tick = SMALLOS_US_PER_SECOND / SMALLOS_TIMER_HZ;
    unsigned int ticks;

    if (usec == 0u) {
        return 0;
    }

    ticks = usec / usec_per_tick;
    if ((usec % usec_per_tick) != 0u) {
        ticks++;
    }
    if (ticks == 0u) {
        ticks = 1u;
    }
    return errno_from_raw(sys_sleep(ticks));
}

unsigned int sleep(unsigned int seconds) {
    if (seconds == 0) {
        return 0;
    }
    if (usleep(seconds * SMALLOS_US_PER_SECOND) < 0) {
        return seconds;
    }
    return 0;
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    unsigned int usec;

    if (!req || req->tv_sec < 0 || req->tv_nsec < 0 ||
        req->tv_nsec >= 1000000000L) {
        set_errno(EINVAL);
        return -1;
    }
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    usec = (unsigned int)req->tv_sec * SMALLOS_US_PER_SECOND;
    usec += (unsigned int)((req->tv_nsec + 999L) / 1000L);
    return usleep(usec);
}

unsigned int alarm(unsigned int seconds) {
    (void)seconds;
    return 0;
}

char* realpath(const char* path, char* resolved_path) {
    char cwd[128];
    char comps[16][32];
    const char* sources[2];
    int source_count = 0;
    int count = 0;
    unsigned int pos = 0;

    if (!path || !*path) {
        set_errno(EINVAL);
        return 0;
    }
    if (!resolved_path) {
        resolved_path = (char*)malloc(128);
        if (!resolved_path) {
            set_errno(ENOMEM);
            return 0;
        }
    }

    if (path[0] != '/') {
        if (!getcwd(cwd, sizeof(cwd))) {
            return 0;
        }
        sources[source_count++] = cwd;
    }
    sources[source_count++] = path;

    for (int s = 0; s < source_count; s++) {
        const char* cursor = sources[s];
        while (*cursor) {
            char component[32];
            int len = 0;

            while (*cursor == '/' || *cursor == '\\') cursor++;
            if (*cursor == '\0') break;

            while (cursor[len] && cursor[len] != '/' && cursor[len] != '\\') {
                if (len >= 31) {
                    set_errno(ENAMETOOLONG);
                    return 0;
                }
                component[len] = cursor[len];
                len++;
            }
            component[len] = '\0';
            cursor += len;

            if (strcmp(component, ".") == 0) {
                continue;
            }
            if (strcmp(component, "..") == 0) {
                if (count > 0) count--;
                continue;
            }
            if (count >= 16) {
                set_errno(ENAMETOOLONG);
                return 0;
            }
            strcpy(comps[count++], component);
        }
    }

    resolved_path[pos++] = '/';
    for (int i = 0; i < count; i++) {
        unsigned int len = strlen(comps[i]);
        if (pos + len + (i + 1 < count ? 1u : 0u) + 1u > 128u) {
            set_errno(ENAMETOOLONG);
            return 0;
        }
        memcpy(resolved_path + pos, comps[i], len);
        pos += len;
        if (i + 1 < count) {
            resolved_path[pos++] = '/';
        }
    }
    resolved_path[pos] = '\0';
    return resolved_path;
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

int fchdir(int fd) {
    (void)fd;
    set_errno(ENOSYS);
    return -1;
}

int chroot(const char* path) {
    (void)path;
    set_errno(ENOSYS);
    return -1;
}

int fsync(int fd) {
    return errno_from_raw(sys_fsync(fd));
}

int ftruncate(int fd, off_t length) {
    struct stat st;

    if (length < 0) {
        set_errno(EINVAL);
        return -1;
    }
    if (fstat(fd, &st) < 0) {
        return -1;
    }
    if (length > st.st_size) {
        set_errno(ENOSYS);
        return -1;
    }
    return 0;
}

int isatty(int fd) {
    uint32_t rows = 0;
    uint32_t cols = 0;

    if (fd < 0 || fd > 2) {
        set_errno(ENOTTY);
        return 0;
    }
    if (sys_terminal_size(&rows, &cols) < 0 || rows == 0 || cols == 0) {
        set_errno(ENOTTY);
        return 0;
    }
    return 1;
}

int ioctl(int fd, unsigned long request, ...) {
    __builtin_va_list ap;
    void* arg = 0;

    __builtin_va_start(ap, request);
    arg = __builtin_va_arg(ap, void*);
    __builtin_va_end(ap);

    if (request == TIOCGWINSZ) {
        struct winsize* ws = (struct winsize*)arg;
        uint32_t rows = 0;
        uint32_t cols = 0;

        if (!ws) {
            set_errno(EFAULT);
            return -1;
        }
        if (fd < 0 || fd > 2 || sys_terminal_size(&rows, &cols) < 0 ||
            rows == 0 || cols == 0) {
            set_errno(ENOTTY);
            return -1;
        }
        memset(ws, 0, sizeof(*ws));
        ws->ws_row = (unsigned short)rows;
        ws->ws_col = (unsigned short)cols;
        return 0;
    }

    set_errno(ENOTTY);
    return -1;
}

int setsockopt(int fd, int level, int optname, const void* optval, unsigned int optlen) {
    return errno_from_raw(sys_setsockopt(fd, level, optname, optval, (socklen_t)optlen));
}

int getsockname(int fd, struct sockaddr* addr, socklen_t* addrlen) {
    return errno_from_raw(sys_getsockname(fd, addr, addrlen));
}

int getpeername(int fd, struct sockaddr* addr, socklen_t* addrlen) {
    return errno_from_raw(sys_getpeername(fd, addr, addrlen));
}

int shutdown(int fd, int how) {
    return errno_from_raw(sys_shutdown(fd, how));
}

void* mmap(void* addr, unsigned int length, int prot, int flags, int fd, int offset) {
    int raw = sys_mmap(addr, length, prot, flags, fd, (uint32_t)offset);
    if (raw < 0) {
        errno = -raw;
        return MAP_FAILED;
    }
    return (void*)raw;
}

int munmap(void* addr, unsigned int length) {
    return errno_from_raw(sys_munmap(addr, length));
}

int mprotect(void* addr, unsigned int length, int prot) {
    return errno_from_raw(sys_mprotect(addr, length, prot));
}

int epoll_create1(int flags) {
    return errno_from_raw(sys_epoll_create(flags));
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
    return errno_from_raw(sys_epoll_ctl(epfd, op, fd, event));
}

int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
    return errno_from_raw(sys_epoll_wait(epfd, events, maxevents, timeout));
}

int timerfd_create(int clockid, int flags) {
    return errno_from_raw(sys_timerfd_create(clockid, flags));
}

int timerfd_settime(int fd, int flags,
                    const struct itimerspec* new_value,
                    struct itimerspec* old_value) {
    return errno_from_raw(sys_timerfd_settime(fd, flags, new_value, old_value));
}

int signalfd(int fd, const sigset_t* mask, int flags) {
    return errno_from_raw(sys_signalfd(fd, mask, flags));
}

int sigemptyset(sigset_t* set) {
    if (!set) {
        set_errno(EFAULT);
        return -1;
    }
    *set = 0;
    return 0;
}

int sigaddset(sigset_t* set, int signum) {
    if (!set || signum <= 0 || signum >= 32) {
        set_errno(EINVAL);
        return -1;
    }
    *set |= (1u << (unsigned int)signum);
    return 0;
}

int sigfillset(sigset_t* set) {
    if (!set) {
        set_errno(EFAULT);
        return -1;
    }
    *set = 0xFFFFFFFEu;
    return 0;
}

int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    static sigset_t current_mask = 0;
    if (oldset) {
        *oldset = current_mask;
    }
    if (set) {
        if (how == SIG_BLOCK) {
            current_mask |= *set;
        } else if (how == SIG_UNBLOCK) {
            current_mask &= ~(*set);
        } else if (how == SIG_SETMASK) {
            current_mask = *set;
        } else {
            set_errno(EINVAL);
            return -1;
        }
    }
    return 0;
}

int sigsuspend(const sigset_t* mask) {
    (void)mask;
    set_errno(EINTR);
    return -1;
}

sighandler_t signal(int signum, sighandler_t handler) {
    sighandler_t old;

    if (signum <= 0 || signum >= 32) {
        set_errno(EINVAL);
        return (sighandler_t)-1;
    }

    old = s_signal_handlers[signum] ? s_signal_handlers[signum] : SIG_DFL;
    s_signal_handlers[signum] = handler;
    return old;
}

int raise(int signum) {
    sighandler_t handler;

    if (signum <= 0 || signum >= 32) {
        set_errno(EINVAL);
        return -1;
    }
    handler = s_signal_handlers[signum];
    if (handler && handler != SIG_IGN && handler != SIG_DFL) {
        handler(signum);
    }
    return 0;
}

int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact) {
    static struct sigaction actions[32];

    if (signum <= 0 || signum >= 32) {
        set_errno(EINVAL);
        return -1;
    }
    if (oldact) {
        *oldact = actions[signum];
    }
    if (act) {
        actions[signum] = *act;
        (void)signal(signum, act->sa_handler);
    }
    return 0;
}

pid_t getpid(void) {
    return (pid_t)sys_getpid();
}

pid_t getppid(void) {
    return 1;
}

pid_t setsid(void) {
    return getpid();
}

pid_t getsid(pid_t pid) {
    if (pid < 0) {
        set_errno(ESRCH);
        return -1;
    }
    if (pid == 0 || pid == getpid()) {
        return getpid();
    }
    if (pid == 1 || pid == getppid()) {
        return pid;
    }
    set_errno(ESRCH);
    return -1;
}

int getgroups(int size, gid_t list[]) {
    if (size < 0) {
        set_errno(EINVAL);
        return -1;
    }
    if (size == 0) {
        return 1;
    }
    if (!list) {
        set_errno(EFAULT);
        return -1;
    }
    list[0] = 0;
    return 1;
}

void sync(void) {
}

long sysconf(int name) {
    if (name == _SC_CLK_TCK) {
        return (long)SMALLOS_TIMER_HZ;
    }
    if (name == _SC_PAGESIZE || name == _SC_PAGE_SIZE) {
        return 4096;
    }
    set_errno(EINVAL);
    return -1;
}

int prctl(int option, unsigned long arg2, unsigned long arg3,
          unsigned long arg4, unsigned long arg5) {
    static char comm[16] = "busybox";
    char* out;
    const char* in;
    unsigned int i;

    (void)arg3;
    (void)arg4;
    (void)arg5;
    if (option == PR_GET_NAME) {
        out = (char*)arg2;
        if (!out) {
            set_errno(EFAULT);
            return -1;
        }
        for (i = 0; i < sizeof(comm); i++) {
            out[i] = comm[i];
            if (!comm[i]) {
                return 0;
            }
        }
        out[sizeof(comm) - 1u] = '\0';
        return 0;
    }
    if (option == PR_SET_NAME) {
        in = (const char*)arg2;
        if (!in) {
            set_errno(EFAULT);
            return -1;
        }
        for (i = 0; i + 1u < sizeof(comm) && in[i]; i++) {
            comm[i] = in[i];
        }
        comm[i] = '\0';
        return 0;
    }
    set_errno(ENOSYS);
    return -1;
}

uid_t getuid(void) {
    return 0;
}

uid_t geteuid(void) {
    return 0;
}

gid_t getgid(void) {
    return 0;
}

gid_t getegid(void) {
    return 0;
}

int setuid(uid_t uid) {
    if (uid == 0) {
        return 0;
    }
    set_errno(ENOSYS);
    return -1;
}

int setgid(gid_t gid) {
    if (gid == 0) {
        return 0;
    }
    set_errno(ENOSYS);
    return -1;
}

int seteuid(uid_t euid) {
    return setuid(euid);
}

int setegid(gid_t egid) {
    return setgid(egid);
}

pid_t waitpid(pid_t pid, int* status, int options) {
    return (pid_t)errno_from_raw(sys_waitpid((int)pid, status, options));
}

pid_t wait(int* status) {
    return waitpid((pid_t)-1, status, 0);
}

int kill(int pid, int signum) {
    return errno_from_raw(sys_kill(pid, signum));
}

ssize_t writev(int fd, const struct iovec* iov, int iovcnt) {
    ssize_t total = 0;

    if (!iov || iovcnt < 0) {
        set_errno(EINVAL);
        return -1;
    }

    for (int i = 0; i < iovcnt; i++) {
        const char* base = (const char*)iov[i].iov_base;
        size_t len = iov[i].iov_len;
        size_t done = 0;

        while (done < len) {
            int n = write(fd, base + done, (unsigned int)(len - done));
            if (n < 0) {
                return total > 0 ? total : -1;
            }
            if (n == 0) {
                return total;
            }
            done += (size_t)n;
            total += n;
        }
    }

    return total;
}

ssize_t sendfile(int out_fd, int in_fd, off_t* offset, size_t count) {
    char buf[1024];
    size_t total = 0;
    off_t pos = 0;

    if (offset) {
        pos = *offset;
        if (lseek(in_fd, pos, SEEK_SET) < 0) {
            return -1;
        }
    }

    while (total < count) {
        size_t want = count - total;
        if (want > sizeof(buf)) want = sizeof(buf);

        int nread = read(in_fd, buf, (unsigned int)want);
        if (nread < 0) {
            return total > 0 ? (ssize_t)total : -1;
        }
        if (nread == 0) {
            break;
        }

        size_t written = 0;
        while (written < (size_t)nread) {
            int nw = write(out_fd, buf + written, (unsigned int)((size_t)nread - written));
            if (nw < 0) {
                if (offset) *offset = pos + (off_t)total;
                return total > 0 ? (ssize_t)total : -1;
            }
            if (nw == 0) {
                if (offset) *offset = pos + (off_t)total;
                return (ssize_t)total;
            }
            written += (size_t)nw;
            total += (size_t)nw;
        }
    }

    if (offset) {
        *offset = pos + (off_t)total;
    }
    return (ssize_t)total;
}

time_t time(time_t* out) {
    struct timespec ts;
    time_t now;

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        now = (time_t)(sys_get_ticks() / SMALLOS_TIMER_HZ);
    } else {
        now = ts.tv_sec;
    }
    if (out) {
        *out = now;
    }
    return now;
}

struct tm* localtime(const time_t* timep) {
    static struct tm t;
    time_t v = timep ? *timep : time(0);
    gmtime_r(&v, &t);
    return &t;
}

struct tm* localtime_r(const time_t* timep, struct tm* result) {
    time_t v;

    if (!result) {
        set_errno(EFAULT);
        return 0;
    }
    v = timep ? *timep : time(0);
    return gmtime_r(&v, result);
}

int gettimeofday(struct timeval* tv, struct timezone* tz) {
    struct timespec ts;

    if (!tv) {
        set_errno(EFAULT);
        return -1;
    }
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        return -1;
    }
    tv->tv_sec = (long)ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000L;
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}

int clock_gettime(int clock_id, struct timespec* ts) {
    if (!ts) {
        set_errno(EFAULT);
        return -1;
    }
    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) {
        set_errno(EINVAL);
        return -1;
    }

    return errno_from_raw(sys_clock_gettime(clock_id, ts)) < 0 ? -1 : 0;
}

int clock_settime(int clock_id, const struct timespec* ts) {
    if (!ts) {
        set_errno(EFAULT);
        return -1;
    }
    if (clock_id != CLOCK_REALTIME) {
        set_errno(EINVAL);
        return -1;
    }
    return errno_from_raw(sys_clock_settime(clock_id, ts)) < 0 ? -1 : 0;
}

int settimeofday(const struct timeval* tv, const struct timezone* tz) {
    struct timespec ts;

    (void)tz;
    if (!tv) {
        set_errno(EFAULT);
        return -1;
    }
    ts.tv_sec = (time_t)tv->tv_sec;
    ts.tv_nsec = tv->tv_usec * 1000L;
    return clock_settime(CLOCK_REALTIME, &ts);
}

char* basename(char* path) {
    static char dot[] = ".";
    char* start = path;
    char* end;
    char* base;

    if (!path || !*path) {
        return dot;
    }
    end = path + strlen(path);
    while (end > start + 1 && end[-1] == '/') {
        *--end = '\0';
    }
    if (start[0] == '/' && start[1] == '\0') {
        return start;
    }
    base = end;
    while (base > start && base[-1] != '/') {
        base--;
    }
    return base;
}

char* dirname(char* path) {
    static char dot[] = ".";
    static char slash[] = "/";
    char* end;
    char* p;

    if (!path || !*path) {
        return dot;
    }
    end = path + strlen(path);
    while (end > path + 1 && end[-1] == '/') {
        *--end = '\0';
    }
    p = end;
    while (p > path && p[-1] != '/') {
        p--;
    }
    if (p == path) {
        return dot;
    }
    while (p > path + 1 && p[-1] == '/') {
        p--;
    }
    if (p == path + 1 && path[0] == '/') {
        path[1] = '\0';
        return slash;
    }
    *p = '\0';
    return path;
}

static int fn_ascii_tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int fn_char_equal(int a, int b, int flags) {
    if (flags & FNM_CASEFOLD) {
        a = fn_ascii_tolower(a);
        b = fn_ascii_tolower(b);
    }
    return a == b;
}

static int fn_is_slash(int c) {
    return c == '/';
}

static int fn_bracket_match(const char* pattern, char c, int flags,
                            const char** out_after) {
    const char* p = pattern + 1;
    int negate = 0;
    int matched = 0;
    int last = -1;

    if (*p == '!' || *p == '^') {
        negate = 1;
        p++;
    }
    if (*p == ']') {
        if (fn_char_equal(']', c, flags)) matched = 1;
        last = ']';
        p++;
    }

    while (*p && *p != ']') {
        int first = (unsigned char)*p++;
        int next;

        if (first == '\\' && !(flags & FNM_NOESCAPE) && *p) {
            first = (unsigned char)*p++;
        }

        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            next = (unsigned char)*p++;
            if (next == '\\' && !(flags & FNM_NOESCAPE) && *p) {
                next = (unsigned char)*p++;
            }
            if (flags & FNM_CASEFOLD) {
                first = fn_ascii_tolower(first);
                next = fn_ascii_tolower(next);
                c = fn_ascii_tolower((unsigned char)c);
            }
            if (first <= (unsigned char)c && (unsigned char)c <= next) {
                matched = 1;
            }
            last = next;
        } else {
            if (fn_char_equal(first, (unsigned char)c, flags)) {
                matched = 1;
            }
            last = first;
        }
    }

    (void)last;
    if (*p != ']') {
        return -1;
    }
    *out_after = p + 1;
    return negate ? !matched : matched;
}

static int fnmatch_inner(const char* pattern, const char* string,
                         int flags, int segment_start) {
    const char* p = pattern;
    const char* s = string;

    while (*p) {
        char pc = *p;

        if (pc == '?') {
            if (!*s || ((flags & FNM_PATHNAME) && fn_is_slash(*s))) {
                return FNM_NOMATCH;
            }
            if ((flags & FNM_PERIOD) && segment_start && *s == '.') {
                return FNM_NOMATCH;
            }
            segment_start = (flags & FNM_PATHNAME) && fn_is_slash(*s);
            p++;
            s++;
            continue;
        }

        if (pc == '*') {
            while (*p == '*') p++;
            if ((flags & FNM_PERIOD) && segment_start && *s == '.') {
                return FNM_NOMATCH;
            }
            if (!*p) {
                if ((flags & FNM_PATHNAME) && strchr(s, '/')) {
                    return (flags & FNM_LEADING_DIR) ? 0 : FNM_NOMATCH;
                }
                return 0;
            }
            if (fnmatch_inner(p, s, flags, segment_start) == 0) {
                return 0;
            }
            while (*s && (!((flags & FNM_PATHNAME) && fn_is_slash(*s)))) {
                s++;
                if (fnmatch_inner(p, s, flags, 0) == 0) {
                    return 0;
                }
            }
            return FNM_NOMATCH;
        }

        if (pc == '[') {
            const char* after = p;
            int ok;

            if (!*s || ((flags & FNM_PATHNAME) && fn_is_slash(*s))) {
                return FNM_NOMATCH;
            }
            if ((flags & FNM_PERIOD) && segment_start && *s == '.') {
                return FNM_NOMATCH;
            }
            ok = fn_bracket_match(p, *s, flags, &after);
            if (ok < 0) {
                pc = '[';
            } else {
                if (!ok) return FNM_NOMATCH;
                segment_start = 0;
                p = after;
                s++;
                continue;
            }
        }

        if (pc == '\\' && !(flags & FNM_NOESCAPE) && p[1]) {
            p++;
            pc = *p;
        }
        if (!*s || !fn_char_equal((unsigned char)pc, (unsigned char)*s, flags)) {
            return FNM_NOMATCH;
        }
        segment_start = (flags & FNM_PATHNAME) && fn_is_slash(*s);
        p++;
        s++;
    }

    if (*s == '\0') {
        return 0;
    }
    if ((flags & FNM_LEADING_DIR) && *s == '/') {
        return 0;
    }
    return FNM_NOMATCH;
}

int fnmatch(const char* pattern, const char* string, int flags) {
    if (!pattern || !string) {
        set_errno(EINVAL);
        return FNM_NOMATCH;
    }
    return fnmatch_inner(pattern, string, flags, 1);
}

int getrlimit(int resource, struct rlimit* rlim) {
    (void)resource;
    (void)rlim;
    set_errno(ENOSYS);
    return -1;
}

int setrlimit(int resource, const struct rlimit* rlim) {
    (void)resource;
    (void)rlim;
    set_errno(ENOSYS);
    return -1;
}

int getrusage(int who, struct rusage* usage) {
    (void)who;
    (void)usage;
    set_errno(ENOSYS);
    return -1;
}

static void uts_copy(char* dst, const char* src) {
    size_t i = 0;
    while (src[i] && i < 64u) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

int uname(struct utsname* name) {
    if (!name) {
        set_errno(EFAULT);
        return -1;
    }
    uts_copy(name->sysname, "SmallOS");
    uts_copy(name->nodename, "smallos");
    uts_copy(name->release, "0.1");
    uts_copy(name->version, "SmallOS syscall ABI v1");
    uts_copy(name->machine, "i386");
    uts_copy(name->domainname, "local");
    return 0;
}

struct hostent* gethostbyname(const char* name) {
    (void)name;
    h_errno = HOST_NOT_FOUND;
    set_errno(ENOSYS);
    return 0;
}

struct servent* getservbyname(const char* name, const char* proto) {
    (void)name;
    (void)proto;
    set_errno(ENOENT);
    return 0;
}

unsigned int if_nametoindex(const char* ifname) {
    (void)ifname;
    set_errno(ENOSYS);
    return 0;
}

char* if_indextoname(unsigned int ifindex, char* ifname) {
    (void)ifindex;
    (void)ifname;
    set_errno(ENOSYS);
    return 0;
}

const char* hstrerror(int err) {
    switch (err) {
        case 0: return "resolver success";
        case HOST_NOT_FOUND: return "host not found";
        case TRY_AGAIN: return "try again";
        case NO_RECOVERY: return "non-recoverable resolver error";
        case NO_DATA: return "no address data";
        default: return "resolver error";
    }
}

int getaddrinfo(const char* node, const char* service,
                const struct addrinfo* hints, struct addrinfo** res) {
    (void)node;
    (void)service;
    (void)hints;
    if (res) {
        *res = 0;
    }
    return EAI_NONAME;
}

int getnameinfo(const struct sockaddr* sa, socklen_t salen,
                char* host, socklen_t hostlen,
                char* serv, socklen_t servlen, int flags) {
    const struct sockaddr_in* in;
    unsigned int port;

    (void)flags;
    if (!sa) {
        return EAI_FAIL;
    }
    if (sa->sa_family == AF_INET) {
        if (salen < sizeof(struct sockaddr_in)) {
            return EAI_FAIL;
        }
        in = (const struct sockaddr_in*)sa;
        if (host && hostlen > 0u) {
            if (!inet_ntop(AF_INET, &in->sin_addr, host, hostlen)) {
                return EAI_FAIL;
            }
        }
        if (serv && servlen > 0u) {
            port = (unsigned int)ntohs(in->sin_port);
            snprintf(serv, servlen, "%u", port);
        }
        return 0;
    }
    if (sa->sa_family == AF_UNIX || sa->sa_family == AF_LOCAL) {
        if (host && hostlen > 0u) {
            snprintf(host, hostlen, "localhost");
        }
        if (serv && servlen > 0u) {
            serv[0] = '\0';
        }
        return 0;
    }
    return EAI_FAMILY;
}

void freeaddrinfo(struct addrinfo* res) {
    (void)res;
}

const char* gai_strerror(int errcode) {
    switch (errcode) {
        case 0: return "success";
        case EAI_NONAME: return "name not known";
        case EAI_SERVICE: return "service not available";
        case EAI_SYSTEM: return "system error";
        default: return "address lookup failed";
    }
}

static char* s_root_members[] = { "root", 0 };

static struct passwd s_root_passwd = {
    "root",
    "x",
    0,
    0,
    "root",
    "/",
    "/bin/sh"
};

static struct group s_root_group = {
    "root",
    "x",
    0,
    s_root_members
};

struct passwd* getpwuid(uid_t uid) {
    if (uid == 0) {
        return &s_root_passwd;
    }
    set_errno(ENOENT);
    return 0;
}

struct passwd* getpwnam(const char* name) {
    if (name && strcmp(name, "root") == 0) {
        return &s_root_passwd;
    }
    set_errno(ENOENT);
    return 0;
}

void endpwent(void) {
}

struct group* getgrgid(gid_t gid) {
    if (gid == 0) {
        return &s_root_group;
    }
    set_errno(ENOENT);
    return 0;
}

struct group* getgrnam(const char* name) {
    if (name && strcmp(name, "root") == 0) {
        return &s_root_group;
    }
    set_errno(ENOENT);
    return 0;
}

int getgrouplist(const char* user, gid_t group, gid_t* groups, int* ngroups) {
    (void)group;
    if (!ngroups) {
        set_errno(EFAULT);
        return -1;
    }
    if (!user || strcmp(user, "root") == 0) {
        if (*ngroups < 1) {
            *ngroups = 1;
            return -1;
        }
        if (!groups) {
            set_errno(EFAULT);
            return -1;
        }
        groups[0] = 0;
        *ngroups = 1;
        return 1;
    }
    *ngroups = 0;
    return 0;
}

int initgroups(const char* user, gid_t group) {
    if (group == 0 && (!user || strcmp(user, "root") == 0)) {
        return 0;
    }
    set_errno(ENOSYS);
    return -1;
}

void endgrent(void) {
}

int tcgetattr(int fd, struct termios* termios_p) {
    if (!termios_p) {
        set_errno(EFAULT);
        return -1;
    }
    if (!isatty(fd)) {
        set_errno(ENOTTY);
        return -1;
    }
    memset(termios_p, 0, sizeof(*termios_p));
    termios_p->c_lflag = ECHO | ICANON | ISIG;
    termios_p->c_oflag = OPOST;
    return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios* termios_p) {
    (void)optional_actions;
    if (!termios_p) {
        set_errno(EFAULT);
        return -1;
    }
    if (!isatty(fd)) {
        set_errno(ENOTTY);
        return -1;
    }
    return 0;
}

int tcflush(int fd, int queue_selector) {
    if (queue_selector != TCIFLUSH && queue_selector != TCOFLUSH &&
        queue_selector != TCIOFLUSH) {
        set_errno(EINVAL);
        return -1;
    }
    if (!isatty(fd)) {
        set_errno(ENOTTY);
        return -1;
    }
    return 0;
}

static int fill_statfs(struct statfs* buf) {
    sys_fsinfo_t fs;

    if (!buf) {
        set_errno(EFAULT);
        return -1;
    }
    if (sys_fsinfo(&fs) < 0) {
        set_errno(EIO);
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->f_type = 0xEF53;
    buf->f_bsize = fs.cluster_bytes ? (long)fs.cluster_bytes : 1024L;
    buf->f_frsize = buf->f_bsize;
    buf->f_blocks = (fs.cluster_bytes ? fs.total_clusters : fs.total_bytes / 1024u);
    buf->f_bfree = (fs.cluster_bytes ? fs.free_clusters : fs.free_bytes / 1024u);
    buf->f_bavail = buf->f_bfree;
    buf->f_files = 1024;
    buf->f_ffree = 512;
    buf->f_namelen = 255;
    return 0;
}

int statfs(const char* path, struct statfs* buf) {
    struct stat st;

    if (!path) {
        set_errno(EFAULT);
        return -1;
    }
    if (stat(path, &st) < 0) {
        return -1;
    }
    return fill_statfs(buf);
}

int fstatfs(int fd, struct statfs* buf) {
    struct stat st;

    if (fstat(fd, &st) < 0) {
        return -1;
    }
    return fill_statfs(buf);
}

static void statfs_to_statvfs(const struct statfs* src, struct statvfs* dst) {
    memset(dst, 0, sizeof(*dst));
    dst->f_bsize = (unsigned long)src->f_bsize;
    dst->f_frsize = (unsigned long)(src->f_frsize ? src->f_frsize : src->f_bsize);
    dst->f_blocks = src->f_blocks;
    dst->f_bfree = src->f_bfree;
    dst->f_bavail = src->f_bavail;
    dst->f_files = src->f_files;
    dst->f_ffree = src->f_ffree;
    dst->f_favail = src->f_ffree;
    dst->f_fsid = (unsigned long)src->f_fsid;
    dst->f_flag = (unsigned long)src->f_flags;
    dst->f_namemax = (unsigned long)src->f_namelen;
}

int statvfs(const char* path, struct statvfs* buf) {
    struct statfs fs;

    if (!buf) {
        set_errno(EFAULT);
        return -1;
    }
    if (statfs(path, &fs) < 0) return -1;
    statfs_to_statvfs(&fs, buf);
    return 0;
}

int fstatvfs(int fd, struct statvfs* buf) {
    struct statfs fs;

    if (!buf) {
        set_errno(EFAULT);
        return -1;
    }
    if (fstatfs(fd, &fs) < 0) return -1;
    statfs_to_statvfs(&fs, buf);
    return 0;
}

int sysinfo(struct sysinfo* info) {
    sys_meminfo_t mem;
    sys_procinfo_t proc;

    if (!info) {
        set_errno(EFAULT);
        return -1;
    }
    if (sys_meminfo(&mem) < 0) {
        set_errno(EIO);
        return -1;
    }
    memset(info, 0, sizeof(*info));
    info->uptime = (long)(sys_get_ticks() / SMALLOS_TIMER_HZ);
    info->totalram = mem.pmm_total_frames * 4096u;
    info->freeram = mem.pmm_free_frames * 4096u;
    info->mem_unit = 1;
    if (sys_procinfo(&proc) == 0) {
        info->procs = (unsigned short)proc.total_count;
    }
    return 0;
}

FILE* setmntent(const char* filename, const char* type) {
    FILE* fp;

    (void)type;
    if (!filename) filename = MOUNTED;
    fp = fopen(filename, "r");
    if (!fp && strcmp(filename, MOUNTED) != 0) {
        fp = fopen(MOUNTED, "r");
    }
    return fp;
}

struct mntent* getmntent(FILE* stream) {
    static struct mntent entries[] = {
        { "rootfs", "/", "ext2", "rw", 0, 0 },
        { "proc", "/proc", "proc", "rw", 0, 0 },
        { "dev", "/dev", "devtmpfs", "rw", 0, 0 },
    };
    static unsigned int index = 0;

    if (!stream) {
        index = 0;
        return 0;
    }
    if (index >= sizeof(entries) / sizeof(entries[0])) {
        index = 0;
        return 0;
    }
    return &entries[index++];
}

int endmntent(FILE* stream) {
    if (stream) fclose(stream);
    (void)getmntent(0);
    return 1;
}

char* hasmntopt(const struct mntent* mnt, const char* opt) {
    if (!mnt || !mnt->mnt_opts || !opt) return 0;
    return strstr(mnt->mnt_opts, opt);
}

static int regex_ch_eq(int a, int b, int icase) {
    if (icase) {
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
    }
    return a == b;
}

static int regex_atom_match(char p, char s, int icase) {
    return p == '.' || regex_ch_eq((unsigned char)p, (unsigned char)s, icase);
}

static int regex_match_here(const char* pat, const char* text, int icase) {
    if (!*pat) return 1;
    if (pat[0] == '$' && pat[1] == '\0') return *text == '\0';
    if (pat[0] && pat[1] == '*') {
        const char* t = text;
        do {
            if (regex_match_here(pat + 2, t, icase)) return 1;
        } while (*t && regex_atom_match(pat[0], *t++, icase));
        return 0;
    }
    if (*text && regex_atom_match(*pat, *text, icase)) {
        return regex_match_here(pat + 1, text + 1, icase);
    }
    return 0;
}

int regcomp(regex_t* preg, const char* regex, int cflags) {
    size_t groups = 0;

    if (!preg || !regex) return REG_BADPAT;
    preg->pattern = strdup(regex);
    if (!preg->pattern) return REG_ESPACE;
    for (size_t i = 0; regex[i]; i++) {
        if (regex[i] == '(') groups++;
    }
    preg->re_nsub = groups;
    preg->cflags = cflags;
    return 0;
}

int regexec(const regex_t* preg, const char* string, size_t nmatch,
            regmatch_t pmatch[], int eflags) {
    const char* pat;
    const char* s;
    int icase;

    (void)eflags;
    if (!preg || !preg->pattern || !string) return REG_NOMATCH;
    pat = preg->pattern;
    icase = (preg->cflags & REG_ICASE) != 0;

    if (pat[0] == '^') {
        int ok = regex_match_here(pat + 1, string, icase);
        if (ok && nmatch && pmatch) {
            pmatch[0].rm_so = 0;
            pmatch[0].rm_eo = (regoff_t)strlen(string);
        }
        return ok ? 0 : REG_NOMATCH;
    }

    for (s = string; ; s++) {
        if (regex_match_here(pat, s, icase)) {
            if (nmatch && pmatch) {
                pmatch[0].rm_so = (regoff_t)(s - string);
                pmatch[0].rm_eo = (regoff_t)strlen(string);
            }
            return 0;
        }
        if (!*s) break;
    }
    return REG_NOMATCH;
}

size_t regerror(int errcode, const regex_t* preg, char* errbuf, size_t errbuf_size) {
    const char* msg;
    size_t len;

    (void)preg;
    switch (errcode) {
        case 0: msg = "success"; break;
        case REG_NOMATCH: msg = "no match"; break;
        case REG_ESPACE: msg = "out of memory"; break;
        default: msg = "bad pattern"; break;
    }
    len = strlen(msg);
    if (errbuf && errbuf_size > 0u) {
        size_t n = len;
        if (n + 1u > errbuf_size) n = errbuf_size - 1u;
        memcpy(errbuf, msg, n);
        errbuf[n] = '\0';
    }
    return len + 1u;
}

void regfree(regex_t* preg) {
    if (!preg) return;
    if (preg->pattern) free(preg->pattern);
    preg->pattern = 0;
    preg->re_nsub = 0;
    preg->cflags = 0;
}
