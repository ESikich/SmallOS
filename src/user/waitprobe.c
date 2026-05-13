#include "user_lib.h"
#include "unistd.h"
#include "signal.h"
#include "sys/wait.h"
#include "errno.h"

static int failures = 0;

static void check(const char* label, int ok) {
    u_puts(label);
    u_puts(ok ? ": PASS\n" : ": FAIL\n");
    if (!ok) failures++;
}

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u_puts("waitprobe start\n");
    check("getpid positive", getpid() > 0);

    {
        char* av[] = { "usr/bin/hello", "waitprobe", 0 };
        int status = -1;
        int pid = sys_exec("usr/bin/hello", 2, av);
        int waited;

        check("sys_exec pid", pid > 0);
        waited = waitpid(pid, &status, 0);
        check("waitpid child", waited == pid);
        check("waitpid exited", WIFEXITED(status));
        check("waitpid status", WEXITSTATUS(status) == 0);
    }

    {
        int status = -1;
        errno = 0;
        check("waitpid no child", waitpid(-1, &status, WNOHANG) == -1);
        check("waitpid no child errno", errno == ECHILD);
    }

    {
        char* av[] = { "usr/libexec/tests/sleep_test", 0 };
        int status = -1;
        int pid = sys_exec("usr/libexec/tests/sleep_test", 1, av);
        int waited;

        check("kill child spawn", pid > 0);
        waited = waitpid(pid, &status, WNOHANG);
        check("waitpid wnohang", waited == 0);
        check("kill child", kill(pid, SIGTERM) == 0);
        waited = waitpid(pid, &status, 0);
        check("waitpid killed child", waited == pid);
        check("waitpid signaled", WIFSIGNALED(status));
        check("waitpid termsig", WTERMSIG(status) == SIGTERM);
    }

    if (failures == 0) {
        u_puts("waitprobe PASS\n");
    } else {
        u_puts("waitprobe FAIL\n");
    }
    sys_exit(failures == 0 ? 0 : 1);
}
