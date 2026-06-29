#include "internal/user_syscall.h"
#include "sys/wait.h"

static void putstr(const char* s) {
    unsigned int n = 0;
    while (s[n]) n++;
    (void)sys_write(s, n);
}

void _start(int argc, char** argv) {
    const char* target = "usr/libexec/tests/dynhello";
    char* child_argv[2];
    int pid;
    int status = 0;

    if (argc > 1 && argv[1]) target = argv[1];

    putstr("dynfailprobe start\n");
    child_argv[0] = (char*)target;
    child_argv[1] = 0;
    pid = sys_exec_foreground(target, 1, child_argv);
    if (pid < 0) {
        putstr("dynfailprobe launch: FAIL\n");
        sys_exit(1);
        for (;;) {}
    }
    putstr("dynfailprobe launch: PASS\n");

    if (sys_waitpid_foreground(pid, &status) < 0) {
        putstr("dynfailprobe wait: FAIL\n");
        sys_exit(1);
        for (;;) {}
    }
    putstr("dynfailprobe wait: PASS\n");

    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        putstr("dynfailprobe status: PASS\n");
        putstr("dynfailprobe PASS\n");
        sys_exit(0);
    }

    putstr("dynfailprobe status: FAIL\n");
    putstr("dynfailprobe FAIL\n");
    sys_exit(1);
    for (;;) {}
}
