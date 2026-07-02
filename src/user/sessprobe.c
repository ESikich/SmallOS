#include "errno.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/wait.h"
#include "termios.h"
#include "unistd.h"
#include "user_syscall.h"

static int failures = 0;

static void check(const char* name, int ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int child_setsid_check(void) {
    pid_t pid = getpid();
    pid_t sid = setsid();

    if (sid != pid) return 10;
    if (getsid(0) != pid) return 11;
    if (getpgid(0) != pid) return 12;
    errno = 0;
    if (setsid() >= 0 || errno != EPERM) return 13;
    return 0;
}

int main(void) {
    pid_t self;
    pid_t sid;
    pid_t pgid;
    pid_t pid;
    int status = -1;
    int fds[2] = { -1, -1 };
    int got_pgrp = -1;

    puts("sessprobe start");

    self = getpid();
    sid = getsid(0);
    pgid = getpgid(0);
    check("self ids", self > 0 && sid > 0 && pgid > 0 && getpgrp() == pgid);
    check("getsid by pid", getsid(self) == sid);
    check("getpgid by pid", getpgid(self) == pgid);

    errno = 0;
    check("getsid invalid", getsid(-1) < 0 && errno == ESRCH);
    errno = 0;
    check("getpgid invalid", getpgid(-1) < 0 && errno == ESRCH);

    check("setpgid self", setpgid(0, 0) == 0 && getpgid(0) == self);
    errno = 0;
    check("setsid leader fail", setsid() < 0 && errno == EPERM);
    errno = 0;
    check("setpgid missing group", setpgid(0, 99999) < 0 && errno == EPERM);

    pid = fork();
    if (pid == 0) {
        exit(child_setsid_check());
    }
    check("fork setsid child", pid > 0);
    if (pid > 0) {
        check("wait setsid child", waitpid(pid, &status, 0) == pid &&
                                    WIFEXITED(status) &&
                                    WEXITSTATUS(status) == 0);
    }

    pid = fork();
    if (pid == 0) {
        sleep(1);
        exit(7);
    }
    check("fork setpgid child", pid > 0);
    if (pid > 0) {
        check("setpgid child", setpgid(pid, pid) == 0 &&
                               getpgid(pid) == pid &&
                               getsid(pid) == getsid(0));
        check("wait setpgid child", waitpid(pid, &status, 0) == pid &&
                                     WIFEXITED(status) &&
                                     WEXITSTATUS(status) == 7);
    }

    check("pty open", sys_pty_open(fds, SYS_FD_FLAG_NONBLOCK) == 0);
    if (fds[1] >= 0) {
        check("tcsetpgrp", tcsetpgrp(fds[1], getpgrp()) == 0);
        got_pgrp = tcgetpgrp(fds[1]);
        check("tcgetpgrp", got_pgrp == getpgrp());
        errno = 0;
        check("tcsetpgrp invalid", tcsetpgrp(fds[1], 99999) < 0 && errno == ESRCH);
        close(fds[1]);
        close(fds[0]);
    }

    printf("sessprobe %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
