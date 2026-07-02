#include "stdio.h"
#include "string.h"
#include "errno.h"
#include "unistd.h"
#include "stdlib.h"
#include "stddef.h"
#include "fcntl.h"
#include "dirent.h"
#include "byteswap.h"
#include "paths.h"
#include "libgen.h"
#include "fnmatch.h"
#include "netdb.h"
#include "pwd.h"
#include "grp.h"
#include "termios.h"
#include "signal.h"
#include "sys/ioctl.h"
#include "sys/param.h"
#include "sys/resource.h"
#include "sys/stat.h"
#include "sys/statvfs.h"
#include "sys/sysinfo.h"
#include "sys/sysmacros.h"
#include "sys/time.h"
#include "sys/types.h"
#include "sys/vfs.h"

static int g_failures = 0;

static void check(const char* name, int ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        g_failures++;
    }
}

static void probe_paths(void) {
    char b1[] = "/usr/bin/busybox";
    char d1[] = "/usr/bin/busybox";
    char b2[] = "/";
    char d2[] = "/";

    check("basename path", strcmp(basename(b1), "busybox") == 0);
    check("dirname path", strcmp(dirname(d1), "/usr/bin") == 0);
    check("basename root", strcmp(basename(b2), "/") == 0);
    check("dirname root", strcmp(dirname(d2), "/") == 0);
}

static void probe_fnmatch(void) {
    check("fnmatch star", fnmatch("usr/*/busybox", "usr/bin/busybox", FNM_PATHNAME) == 0);
    check("fnmatch question", fnmatch("busybo?", "busybox", 0) == 0);
    check("fnmatch bracket", fnmatch("busybox-[0-9]", "busybox-1", 0) == 0);
    check("fnmatch neg bracket", fnmatch("busybox-[!a-z]", "busybox-1", 0) == 0);
    check("fnmatch slash", fnmatch("usr/*", "usr/bin/busybox", FNM_PATHNAME) == FNM_NOMATCH);
}

static void probe_terminal(void) {
    struct winsize ws;

    check("isatty stdout", isatty(STDOUT_FILENO) == 1);
    memset(&ws, 0, sizeof(ws));
    check("ioctl winsize", ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
                             ws.ws_row > 0 && ws.ws_col > 0);
    errno = 0;
    check("ioctl enotty", ioctl(99, TIOCGWINSZ, &ws) < 0 && errno == ENOTTY);
}

static void probe_metadata(void) {
    struct timeval tv[2];
    struct timespec ts[2];
    struct stat st;
    char buf[16];
    int fd;

    memset(tv, 0, sizeof(tv));
    memset(ts, 0, sizeof(ts));
    check("stat absolute dir", stat("/etc", &st) == 0 && S_ISDIR(st.st_mode));
    check("stat timespec", st.st_mtim.tv_sec == st.st_mtime &&
                           st.st_mtim.tv_nsec == 0);
    check("stat interp", stat("/lib/ld-smallos.so", &st) == 0 && S_ISREG(st.st_mode));
    fd = open("/usr/bin/hello", O_RDONLY);
    check("open hello", fd >= 0);
    if (fd >= 0) {
        check("read hello", read(fd, buf, sizeof(buf)) > 0);
        check("futimens noop", futimens(fd, ts) == 0);
        close(fd);
    }
    check("mkdir tmp compat", mkdir("/tmp/compatprobe-dir", 0777) == 0);
    check("rmdir tmp compat", rmdir("/tmp/compatprobe-dir") == 0);
    fd = open("/tmp/compatprobe-file", O_RDWR | O_CREAT, 0666);
    check("open create tmp", fd >= 0);
    if (fd >= 0) {
        close(fd);
        check("unlink tmp file", unlink("/tmp/compatprobe-file") == 0);
    }
    check("chmod noop", chmod("/usr/bin/hello", 0755) == 0);
    check("chown noop", chown("/usr/bin/hello", 0, 0) == 0);
    check("utimes noop", utimes("/usr/bin/hello", tv) == 0);
    check("utimensat noop", utimensat(AT_FDCWD, "/usr/bin/hello", ts, 0) == 0);
    errno = 0;
    check("chmod missing", chmod("/no/such/file", 0755) < 0 && errno == ENOENT);
    errno = 0;
    check("utimensat missing", utimensat(AT_FDCWD, "/no/such/file", ts, 0) < 0 &&
                                errno == ENOENT);
    errno = 0;
    check("mknod enosys", mknod("/tmp/compat-node", S_IFCHR | 0600, makedev(1, 3)) < 0 &&
                          errno == ENOSYS);
    errno = 0;
    check("mkfifo enosys", mkfifo("/tmp/compat-fifo", 0600) < 0 && errno == ENOSYS);
}

static int read_file_contains(const char* path, const char* needle) {
    char buf[256];
    int fd;
    int n;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = read(fd, buf, sizeof(buf) - 1u);
    close(fd);
    if (n < 0) {
        return 0;
    }
    buf[n] = '\0';
    return strstr(buf, needle) != 0;
}

static void probe_unix_layer(void) {
    struct statfs sfs;
    struct statvfs svfs;
    struct sysinfo si;
    struct stat st;
    DIR* dir;
    struct dirent* ent;
    int saw_meminfo = 0;
    int saw_null = 0;
    int fd;
    int fd2;
    char buf[8];

    check("statfs root", statfs("/", &sfs) == 0 && sfs.f_bsize > 0 &&
                          sfs.f_blocks >= sfs.f_bfree);
    check("statfs proc", statfs("/proc", &sfs) == 0 && sfs.f_bsize > 0);
    check("statvfs root", statvfs("/", &svfs) == 0 && svfs.f_bsize > 0);
    check("sysinfo basic", sysinfo(&si) == 0 && si.mem_unit == 1 && si.totalram > 0);
    check("stat bin sh", stat("/bin/sh", &st) == 0 && S_ISREG(st.st_mode));
    check("stat proc dir", stat("/proc", &st) == 0 && S_ISDIR(st.st_mode));
    check("stat dev null", stat("/dev/null", &st) == 0 && S_ISCHR(st.st_mode));
    check("proc meminfo read", read_file_contains("/proc/meminfo", "MemTotal"));
    check("proc uptime read", read_file_contains("/proc/uptime", " "));
    check("proc self status", read_file_contains("/proc/self/status", "Name:"));
    check("proc self cmdline", read_file_contains("/proc/self/cmdline", "compatprobe"));

    dir = opendir("/proc");
    check("opendir proc", dir != 0);
    if (dir) {
        while ((ent = readdir(dir)) != 0) {
            if (strcmp(ent->d_name, "meminfo") == 0) {
                saw_meminfo = 1;
            }
        }
        closedir(dir);
    }
    check("proc lists meminfo", saw_meminfo);

    dir = opendir("/dev");
    check("opendir dev", dir != 0);
    if (dir) {
        while ((ent = readdir(dir)) != 0) {
            if (strcmp(ent->d_name, "null") == 0) {
                saw_null = 1;
            }
        }
        closedir(dir);
    }
    check("dev lists null", saw_null);

    fd = open("/dev/null", O_RDWR);
    check("open dev null", fd >= 0);
    if (fd >= 0) {
        check("write dev null", write(fd, "x", 1) == 1);
        check("read dev null eof", read(fd, buf, sizeof(buf)) == 0);
        fd2 = fcntl(fd, F_DUPFD, 3);
        check("fcntl dupfd", fd2 >= 3);
        if (fd2 >= 0) {
            close(fd2);
        }
        close(fd);
    }

    memset(buf, 1, sizeof(buf));
    fd = open("/dev/zero", O_RDONLY);
    check("open dev zero", fd >= 0);
    if (fd >= 0) {
        check("read dev zero", read(fd, buf, sizeof(buf)) == (int)sizeof(buf) &&
                               buf[0] == 0 && buf[sizeof(buf) - 1u] == 0);
        close(fd);
    }

    fd = open("/dev/fd/1", O_WRONLY);
    check("open dev fd 1", fd >= 0);
    if (fd >= 0) {
        check("write dev fd 1", write(fd, "", 0) == 0);
        close(fd);
    }
}

static void probe_dirent(void) {
    DIR* dir = opendir("/");
    struct dirent* ent;
    int saw_tmp = 0;

    check("opendir root", dir != 0);
    if (!dir) {
        return;
    }
    while ((ent = readdir(dir)) != 0) {
        if (strcmp(ent->d_name, "tmp") == 0) {
            saw_tmp = ent->d_is_dir;
        }
        check("dirent no slash", ent->d_name[0] == '\0' ||
                                  ent->d_name[strlen(ent->d_name) - 1u] != '/');
    }
    closedir(dir);
    check("dirent tmp dir", saw_tmp == 1);
}

static void probe_stubs(void) {
    struct rlimit lim;
    struct passwd* pw;
    struct group* gr;
    gid_t groups[2];
    int ngroups;
    sigset_t set;
    struct sigaction sa;

    errno = 0;
    check("getrlimit enosys", getrlimit(RLIMIT_NOFILE, &lim) < 0 && errno == ENOSYS);
    check("getaddrinfo noname", getaddrinfo("smallos.invalid", 0, 0, 0) == EAI_NONAME);
    errno = 0;
    check("gethostbyname enosys", gethostbyname("smallos.invalid") == 0 && errno == ENOSYS);

    pw = getpwuid(0);
    check("root passwd uid", pw && strcmp(pw->pw_name, "root") == 0);
    pw = getpwnam("root");
    check("root passwd name", pw && pw->pw_uid == 0);
    gr = getgrgid(0);
    check("root group gid", gr && strcmp(gr->gr_name, "root") == 0);
    gr = getgrnam("root");
    check("root group name", gr && gr->gr_gid == 0);
    ngroups = 2;
    check("getgrouplist root", getgrouplist("root", 0, groups, &ngroups) == 1 &&
                                ngroups == 1 && groups[0] == 0);

    check("getsid self", getsid(0) == getpid());
    sync();
    check("sync noop", 1);
    check("sigfillset", sigfillset(&set) == 0 && (set & (1u << SIGTERM)) != 0);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    check("sigaction stub", sigaction(SIGPIPE, &sa, 0) == 0);
}

int main(void) {
    printf("compatprobe start\n");
    check("offsetof", offsetof(struct winsize, ws_col) == sizeof(unsigned short));
    check("exit macros", EXIT_SUCCESS == 0 && EXIT_FAILURE != 0);
    check("byteswap", bswap_16(0x1234) == 0x3412);
    setbuf(stdout, 0);
    check("setbuf noop", 1);
    check("mode helpers", S_ISCHR(S_IFCHR) && S_ISBLK(S_IFBLK) &&
                          S_ISFIFO(S_IFIFO) && S_ISSOCK(S_IFSOCK));
    probe_paths();
    probe_fnmatch();
    probe_terminal();
    probe_metadata();
    probe_unix_layer();
    probe_dirent();
    probe_stubs();
    printf("compatprobe %s\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
