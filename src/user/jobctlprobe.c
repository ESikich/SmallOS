#include "errno.h"
#include "signal.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/wait.h"
#include "unistd.h"

static int failures = 0;

static void check(const char* name, int ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    pid_t pid;
    int status = -1;
    int waited;

    puts("jobctlprobe start");

    pid = fork();
    if (pid == 0) {
        (void)setpgid(0, 0);
        for (;;) {
            sleep(1);
        }
    }

    check("fork child", pid > 0);
    if (pid <= 0) {
        printf("jobctlprobe %s\n", failures ? "FAIL" : "PASS");
        return failures ? 1 : 0;
    }

    errno = 0;
    check("setpgid child", setpgid(pid, pid) == 0);
    check("child pgid", getpgid(pid) == pid);

    check("killpg stop", killpg(pid, SIGTSTP) == 0);
    waited = waitpid(pid, &status, WUNTRACED);
    check("waitpid stopped child", waited == pid);
    check("waitpid stopped", WIFSTOPPED(status));
    check("waitpid stopsig", WSTOPSIG(status) == SIGTSTP);

    status = -1;
    waited = waitpid(pid, &status, WNOHANG | WUNTRACED);
    check("stopped reported once", waited == 0);

    check("killpg cont", killpg(pid, SIGCONT) == 0);
    status = -1;
    waited = waitpid(pid, &status, WNOHANG | WUNTRACED);
    check("continued child running", waited == 0);

    check("kill child term", kill(pid, SIGTERM) == 0);
    waited = waitpid(pid, &status, 0);
    check("waitpid killed child", waited == pid);
    check("waitpid signaled", WIFSIGNALED(status));
    check("waitpid termsig", WTERMSIG(status) == SIGTERM);

    printf("jobctlprobe %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
