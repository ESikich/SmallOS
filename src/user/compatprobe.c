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
#include "arpa/inet.h"
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
#include "sys/wait.h"

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
    int nullfd;

    check("isatty stdout", isatty(STDOUT_FILENO) == 1);
    memset(&ws, 0, sizeof(ws));
    check("ioctl winsize", ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
                             ws.ws_row > 0 && ws.ws_col > 0);
    nullfd = open("/dev/null", O_RDONLY);
    check("open null tty negative", nullfd >= 0);
    if (nullfd >= 0) {
        errno = 0;
        check("ioctl enotty", ioctl(nullfd, TIOCGWINSZ, &ws) < 0 && errno == ENOTTY);
        close(nullfd);
    }
}

static void probe_metadata(void) {
    struct timeval tv[2];
    struct timespec ts[2];
    struct stat st;
    struct stat lst;
    char buf[16];
    char linkbuf[64];
    int fd;
    int n;

    unlink("/tmp/compatprobe-file");
    unlink("/tmp/compatprobe-hard");
    unlink("/tmp/compatprobe-sym");
    unlink("/tmp/compat-node");
    unlink("/tmp/compat-fifo");
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
        check("write compat tmp", write(fd, "abcdef", 6) == 6);
        check("ftruncate shrink", ftruncate(fd, 3) == 0 &&
                                  fstat(fd, &st) == 0 && st.st_size == 3);
        check("ftruncate grow", ftruncate(fd, 8) == 0 &&
                                fstat(fd, &st) == 0 && st.st_size == 8);
        ts[0].tv_sec = 111;
        ts[0].tv_nsec = 0;
        ts[1].tv_sec = 222;
        ts[1].tv_nsec = 0;
        check("futimens set", futimens(fd, ts) == 0 &&
                              fstat(fd, &st) == 0 && st.st_mtime == 222);
        close(fd);
    }
    check("chmod set", chmod("/tmp/compatprobe-file", 0600) == 0 &&
                       stat("/tmp/compatprobe-file", &st) == 0 &&
                       (st.st_mode & 0777) == 0600);
    check("chown set", chown("/tmp/compatprobe-file", 2, 3) == 0 &&
                       stat("/tmp/compatprobe-file", &st) == 0 &&
                       st.st_uid == 2 && st.st_gid == 3);
    tv[0].tv_sec = 333;
    tv[0].tv_usec = 0;
    tv[1].tv_sec = 444;
    tv[1].tv_usec = 0;
    check("utimes set", utimes("/tmp/compatprobe-file", tv) == 0 &&
                         stat("/tmp/compatprobe-file", &st) == 0 &&
                         st.st_mtime == 444);
    ts[0].tv_sec = 555;
    ts[0].tv_nsec = 0;
    ts[1].tv_sec = 666;
    ts[1].tv_nsec = 0;
    check("utimensat set", utimensat(AT_FDCWD, "/tmp/compatprobe-file", ts, 0) == 0 &&
                           stat("/tmp/compatprobe-file", &st) == 0 &&
                           st.st_mtime == 666);
    check("hard link create", link("/tmp/compatprobe-file", "/tmp/compatprobe-hard") == 0);
    check("hard link nlink", stat("/tmp/compatprobe-file", &st) == 0 &&
                             stat("/tmp/compatprobe-hard", &lst) == 0 &&
                             st.st_ino == lst.st_ino && st.st_nlink >= 2);
    check("hard link unlink one", unlink("/tmp/compatprobe-file") == 0 &&
                                  stat("/tmp/compatprobe-hard", &st) == 0 &&
                                  st.st_size == 8);
    check("symlink create", symlink("/tmp/compatprobe-hard", "/tmp/compatprobe-sym") == 0);
    memset(linkbuf, 0, sizeof(linkbuf));
    n = readlink("/tmp/compatprobe-sym", linkbuf, sizeof(linkbuf));
    if (n >= 0 && n < (int)sizeof(linkbuf)) linkbuf[n] = '\0';
    check("readlink target", n == 21 && strcmp(linkbuf, "/tmp/compatprobe-hard") == 0);
    check("lstat symlink", lstat("/tmp/compatprobe-sym", &lst) == 0 && S_ISLNK(lst.st_mode));
    check("stat symlink follows", stat("/tmp/compatprobe-sym", &st) == 0 &&
                                  S_ISREG(st.st_mode) && st.st_size == 8);
    check("unlink symlink", unlink("/tmp/compatprobe-sym") == 0 &&
                            stat("/tmp/compatprobe-hard", &st) == 0);
    check("unlink hard final", unlink("/tmp/compatprobe-hard") == 0);
    errno = 0;
    check("chmod missing", chmod("/no/such/file", 0755) < 0 && errno == ENOENT);
    errno = 0;
    check("utimensat missing", utimensat(AT_FDCWD, "/no/such/file", ts, 0) < 0 &&
                                errno == ENOENT);
    check("mknod char", mknod("/tmp/compat-node", S_IFCHR | 0600, makedev(1, 3)) == 0 &&
                        lstat("/tmp/compat-node", &st) == 0 && S_ISCHR(st.st_mode));
    check("mkfifo node", mkfifo("/tmp/compat-fifo", 0600) == 0 &&
                         lstat("/tmp/compat-fifo", &st) == 0 && S_ISFIFO(st.st_mode));
    check("unlink node", unlink("/tmp/compat-node") == 0);
    check("unlink fifo", unlink("/tmp/compat-fifo") == 0);
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
    int saw_proc_net = 0;
    int saw_proc_self = 0;
    int saw_proc_net_tcp = 0;
    int saw_null = 0;
    int saw_urandom = 0;
    int saw_fd = 0;
    int saw_fd_0 = 0;
    int saw_pts = 0;
    int fd;
    int fd2;
    int cwd_fd;
    char buf[8];
    char buf2[8];
    char cwd_before[128];
    char cwd_after[128];
    int nonzero;
    int differs;

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
            if (strcmp(ent->d_name, "net") == 0) {
                saw_proc_net = 1;
            }
            if (strcmp(ent->d_name, "self") == 0) {
                saw_proc_self = 1;
            }
        }
        closedir(dir);
    }
    check("proc lists meminfo", saw_meminfo);
    check("proc lists net", saw_proc_net);
    check("proc lists self", saw_proc_self);

    dir = opendir("/proc/net");
    check("opendir proc net", dir != 0);
    if (dir) {
        while ((ent = readdir(dir)) != 0) {
            if (strcmp(ent->d_name, "tcp") == 0) {
                saw_proc_net_tcp = 1;
            }
        }
        closedir(dir);
    }
    check("proc net lists tcp", saw_proc_net_tcp);

    dir = opendir("/dev");
    check("opendir dev", dir != 0);
    if (dir) {
        while ((ent = readdir(dir)) != 0) {
            if (strcmp(ent->d_name, "null") == 0) {
                saw_null = 1;
            }
            if (strcmp(ent->d_name, "urandom") == 0) {
                saw_urandom = 1;
            }
            if (strcmp(ent->d_name, "fd") == 0) {
                saw_fd = 1;
            }
            if (strcmp(ent->d_name, "pts") == 0) {
                saw_pts = 1;
            }
        }
        closedir(dir);
    }
    check("dev lists null", saw_null);
    check("dev lists urandom", saw_urandom);
    check("dev lists fd", saw_fd);
    check("dev lists pts", saw_pts);

    dir = opendir("/dev/fd");
    check("opendir dev fd", dir != 0);
    if (dir) {
        while ((ent = readdir(dir)) != 0) {
            if (strcmp(ent->d_name, "0") == 0) {
                saw_fd_0 = 1;
            }
        }
        closedir(dir);
    }
    check("dev fd lists 0", saw_fd_0);

    dir = opendir("/dev/pts");
    check("opendir dev pts", dir != 0);
    if (dir) {
        closedir(dir);
    }

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

    memset(buf, 0, sizeof(buf));
    memset(buf2, 0, sizeof(buf2));
    fd = open("/dev/urandom", O_RDONLY);
    check("open dev urandom", fd >= 0);
    if (fd >= 0) {
        check("read dev urandom first", read(fd, buf, sizeof(buf)) == (int)sizeof(buf));
        check("read dev urandom second", read(fd, buf2, sizeof(buf2)) == (int)sizeof(buf2));
        nonzero = 0;
        differs = 0;
        for (unsigned int i = 0; i < sizeof(buf); i++) {
            if (buf[i] != 0 || buf2[i] != 0) nonzero = 1;
            if (buf[i] != buf2[i]) differs = 1;
        }
        check("dev urandom nonzero", nonzero);
        check("dev urandom changes", differs);
        close(fd);
    }

    memset(cwd_before, 0, sizeof(cwd_before));
    memset(cwd_after, 0, sizeof(cwd_after));
    check("getcwd before fchdir", getcwd(cwd_before, sizeof(cwd_before)) != 0);
    cwd_fd = open(".", O_RDONLY);
    check("open cwd dir", cwd_fd >= 0);
    if (cwd_fd >= 0) {
        check("chdir tmp", chdir("/tmp") == 0);
        check("fchdir restore", fchdir(cwd_fd) == 0);
        check("getcwd after fchdir", getcwd(cwd_after, sizeof(cwd_after)) != 0 &&
                                      strcmp(cwd_before, cwd_after) == 0);
        close(cwd_fd);
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

static void probe_libc_compat(void) {
    char dst[32];
    char copy_src[] = "hello";
    char rchr_src[] = "abca";
    char sep_src[] = "one,two";
    char* sep = sep_src;
    char tmpl[] = "/tmp/compatXXXXXX";
    char ttybuf[32];
    int fd;
    int child;
    int status = -1;
    struct rusage usage;

    memset(dst, 0, sizeof(dst));
    check("mempcpy helper", mempcpy(dst, "abc", 3) == dst + 3 &&
                            memcmp(dst, "abc", 3) == 0);
    check("memrchr helper", memrchr(rchr_src, 'a', 4) == (void*)(rchr_src + 3));
    check("stpcpy helper", stpcpy(dst, "busy") == dst + 4 &&
                           strcmp(dst, "busy") == 0);
    memset(dst, 'x', sizeof(dst));
    check("stpncpy helper", stpncpy(dst, copy_src, 8) == dst + 5 &&
                            memcmp(dst, "hello\0\0\0", 8) == 0);
    check("strcasestr helper", strcasestr("SmallOS BusyBox", "busy") != 0);
    check("strchrnul helper", strchrnul("abc", 'z') == "abc" + 3);
    check("strsep helper", strcmp(strsep(&sep, ","), "one") == 0 &&
                           sep && strcmp(strsep(&sep, ","), "two") == 0 &&
                           sep == 0);
    check("strsignal term", strcmp(strsignal(SIGTERM), "Terminated") == 0);
    check("strverscmp basic", strverscmp("a9", "a10") < 0);

    fd = open("/tmp/compat-fdatasync", O_CREAT | O_TRUNC | O_RDWR, 0600);
    check("fdatasync wrapper", fd >= 0 && write(fd, "x", 1) == 1 &&
                               fdatasync(fd) == 0);
    if (fd >= 0) {
        close(fd);
        unlink("/tmp/compat-fdatasync");
    }

    check("mkdtemp helper", mkdtemp(tmpl) == tmpl && strncmp(tmpl, "/tmp/compat", 11) == 0);
    rmdir(tmpl);

    check("ttyname_r stdout", ttyname_r(STDOUT_FILENO, ttybuf, sizeof(ttybuf)) == 0 &&
                              strncmp(ttybuf, "/dev/pts/", 9) == 0);

    child = fork();
    if (child == 0) {
        _exit(0);
    }
    memset(&usage, 0xAA, sizeof(usage));
    check("wait3 wrapper", child > 0 && wait3(&status, 0, &usage) == child &&
                           WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                           usage.ru_utime.tv_sec == 0);

    check("setenv before clearenv", setenv("COMPAT_CLEAR", "yes", 1) == 0 &&
                                    getenv("COMPAT_CLEAR") != 0);
    check("clearenv helper", clearenv() == 0 && getenv("COMPAT_CLEAR") == 0);
}

static void probe_stubs(void) {
    struct rlimit lim;
    struct passwd* pw;
    struct group* gr;
    gid_t groups[2];
    int ngroups;
    sigset_t set;
    struct sigaction sa;
    struct hostent* he;
    struct addrinfo* ai = 0;
    struct addrinfo hints;

    check("getrlimit nofile", getrlimit(RLIMIT_NOFILE, &lim) == 0 &&
                              lim.rlim_cur > 0 &&
                              lim.rlim_max >= lim.rlim_cur);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_flags = AI_NUMERICHOST;
    check("getaddrinfo noname", getaddrinfo("smallos.invalid", 0, &hints, &ai) ==
                                  EAI_NONAME);
    errno = 0;
    check("gethostbyname noname", gethostbyname("bad..name") == 0 && errno == ENOENT);
    he = gethostbyname("localhost");
    check("gethostbyname localhost", he && he->h_addrtype == AF_INET &&
                                      he->h_length == 4 &&
                                      *(unsigned int*)he->h_addr == htonl(INADDR_LOOPBACK));
    check("getaddrinfo numeric", getaddrinfo("127.0.0.1", "80", 0, &ai) == 0 &&
                                  ai && ai->ai_family == AF_INET &&
                                  ai->ai_addrlen == sizeof(struct sockaddr_in));

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

    check("getsid self", getsid(0) > 0 && getsid(0) == getsid(getpid()));
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
    probe_libc_compat();
    probe_stubs();
    printf("compatprobe %s\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
