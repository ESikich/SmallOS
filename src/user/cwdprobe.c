#include "user_lib.h"
#include "stdlib.h"
#include "unistd.h"

static int streq(const char* a, const char* b) {
    unsigned int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    char cwd[64];

    u_puts("cwdprobe start\n");

    if (u_getcwd(cwd, sizeof(cwd)) < 0) {
        u_puts("cwdprobe getcwd root: FAIL\n");
        sys_exit(1);
    }
    u_puts("cwdprobe cwd=");
    u_puts(cwd);
    u_putc('\n');
    if (!streq(cwd, "/")) {
        u_puts("cwdprobe root cwd mismatch\n");
        sys_exit(1);
    }

    if (u_chdir("apps/demo") < 0) {
        u_puts("cwdprobe chdir: FAIL\n");
        sys_exit(1);
    }

    if (u_getcwd(cwd, sizeof(cwd)) < 0) {
        u_puts("cwdprobe getcwd demo: FAIL\n");
        sys_exit(1);
    }
    u_puts("cwdprobe cwd=");
    u_puts(cwd);
    u_putc('\n');
    if (!streq(cwd, "/apps/demo")) {
        u_puts("cwdprobe demo cwd mismatch\n");
        sys_exit(1);
    }

    {
        int fd = sys_open("hello.elf");
        if (fd < 0) {
            u_puts("cwdprobe open relative: FAIL\n");
            sys_exit(1);
        }
        sys_close(fd);
    }
    u_puts("cwdprobe open relative: PASS\n");

    {
        char resolved[128];
        if (!realpath("../demo/./hello.elf", resolved) || !streq(resolved, "/apps/demo/hello.elf")) {
            u_puts("cwdprobe realpath relative: FAIL\n");
            sys_exit(1);
        }
        u_puts("cwdprobe realpath relative: PASS\n");
    }

    if (access("hello.elf", R_OK) != 0 || access("missing.elf", F_OK) == 0) {
        u_puts("cwdprobe access relative: FAIL\n");
        sys_exit(1);
    }
    u_puts("cwdprobe access relative: PASS\n");

    {
        int fd = u_open_write("cwdprobe.txt");
        if (fd < 0) {
            u_puts("cwdprobe open_write relative: FAIL\n");
            sys_exit(1);
        }

        static const char msg[] = "cwdprobe wrote this\n";
        if (u_writefd(fd, msg, sizeof(msg) - 1u) < 0) {
            u_puts("cwdprobe write relative: FAIL\n");
            sys_close(fd);
            sys_exit(1);
        }

        if (sys_close(fd) < 0) {
            u_puts("cwdprobe close: FAIL\n");
            sys_exit(1);
        }
    }
    u_puts("cwdprobe write relative: PASS\n");

    u_puts("cwdprobe PASS\n");
    sys_exit(0);
}
