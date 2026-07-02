#include "errno.h"
#include "fcntl.h"
#include "stdio.h"
#include "sys/resource.h"
#include "unistd.h"

static int failures = 0;

static void check(const char* name, int ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void close_opened(int* fds, int count) {
    for (int i = 0; i < count; i++) {
        if (fds[i] >= 0) close(fds[i]);
    }
}

int main(void) {
    struct rlimit nofile;
    struct rlimit saved_nofile;
    struct rlimit lim;
    struct rusage usage;
    int fds[64];
    int opened = 0;

    puts("rsrcprobe start");

    for (int i = 0; i < (int)(sizeof(fds) / sizeof(fds[0])); i++) {
        fds[i] = -1;
    }

    check("getrlimit nofile", getrlimit(RLIMIT_NOFILE, &nofile) == 0 &&
                              nofile.rlim_cur >= 16 &&
                              nofile.rlim_max >= nofile.rlim_cur);
    saved_nofile = nofile;

    lim.rlim_cur = 20;
    lim.rlim_max = nofile.rlim_max;
    check("setrlimit nofile lower", setrlimit(RLIMIT_NOFILE, &lim) == 0);
    check("getrlimit nofile lowered", getrlimit(RLIMIT_NOFILE, &nofile) == 0 &&
                                      nofile.rlim_cur == 20 &&
                                      nofile.rlim_max >= nofile.rlim_cur);

    while (opened < (int)(sizeof(fds) / sizeof(fds[0]))) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) break;
        fds[opened++] = fd;
    }
    check("nofile enforced", opened == 17);
    errno = 0;
    check("nofile open errno", open("/dev/null", O_RDONLY) < 0 && errno == ENFILE);
    close_opened(fds, opened);

    errno = 0;
    lim.rlim_cur = nofile.rlim_max + 1;
    lim.rlim_max = nofile.rlim_max + 1;
    check("setrlimit nofile hard fail", setrlimit(RLIMIT_NOFILE, &lim) < 0 &&
                                          errno == EINVAL);
    check("setrlimit nofile restore", setrlimit(RLIMIT_NOFILE, &saved_nofile) == 0);

    check("getrlimit as", getrlimit(RLIMIT_AS, &lim) == 0 &&
                          lim.rlim_cur > 0 && lim.rlim_max == lim.rlim_cur);
    check("getrlimit data", getrlimit(RLIMIT_DATA, &lim) == 0 &&
                            lim.rlim_cur > 0 && lim.rlim_max == lim.rlim_cur);
    check("getrlimit stack", getrlimit(RLIMIT_STACK, &lim) == 0 &&
                             lim.rlim_cur > 0 && lim.rlim_max == lim.rlim_cur);
    check("getrlimit cpu", getrlimit(RLIMIT_CPU, &lim) == 0 &&
                           lim.rlim_cur == RLIM_INFINITY &&
                           lim.rlim_max == RLIM_INFINITY);

    errno = 0;
    check("getrlimit invalid", getrlimit(999, &lim) < 0 && errno == EINVAL);

    check("getrusage self", getrusage(RUSAGE_SELF, &usage) == 0 &&
                            usage.ru_utime.tv_sec >= 0 &&
                            usage.ru_utime.tv_usec >= 0 &&
                            usage.ru_maxrss >= 0);
    check("getrusage children", getrusage(RUSAGE_CHILDREN, &usage) == 0 &&
                                usage.ru_utime.tv_sec == 0 &&
                                usage.ru_stime.tv_sec == 0);
    errno = 0;
    check("getrusage invalid", getrusage(1234, &usage) < 0 && errno == EINVAL);

    printf("rsrcprobe %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
