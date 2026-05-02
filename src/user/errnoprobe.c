#include "user_lib.h"
#include "errno.h"
#include "fcntl.h"
#include "unistd.h"
#include "sys/stat.h"

static int failures = 0;

static void print_int(int value) {
    if (value < 0) {
        u_putc('-');
        u_put_uint((uint32_t)(-value));
    } else {
        u_put_uint((uint32_t)value);
    }
}

static void check_int(const char* name, int expected, int actual) {
    u_puts(name);
    u_puts(": ");
    if (expected == actual) {
        u_puts("PASS\n");
    } else {
        u_puts("FAIL expected=");
        print_int(expected);
        u_puts(" actual=");
        print_int(actual);
        u_putc('\n');
        failures++;
    }
}

static void check_errno(const char* name, int expected_errno, int actual) {
    check_int(name, -1, actual);
    check_int("errno", expected_errno, errno);
}

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u_puts("errnoprobe start\n");

    errno = 0;
    check_int("raw sys_open missing", -ENOENT, sys_open("missing.nope"));

    errno = 0;
    check_errno("open missing", ENOENT, open("missing.nope", O_RDONLY));

    errno = 0;
    check_errno("close bad fd", EBADF, close(99));

    errno = 0;
    check_errno("read bad fd", EBADF, read(99, argv, 1));

    errno = 0;
    check_errno("open directory", EISDIR, open("apps", O_RDONLY));

    errno = 0;
    check_errno("chdir file", ENOTDIR, chdir("apps/demo/hello.elf"));

    errno = 0;
    {
        char tiny[1];
        check_int("getcwd tiny result", 0, getcwd(tiny, sizeof(tiny)) == 0 ? 0 : 1);
        check_int("getcwd tiny errno", EINVAL, errno);
    }

    errno = 0;
    check_errno("execvp unsupported", ENOSYS, execvp("hello", 0));

    errno = 0;
    {
        int fds[8];
        int count = 0;
        int fd;
        while (count < 8) {
            fd = open("apps/demo/hello.elf", O_RDONLY);
            if (fd < 0) {
                break;
            }
            fds[count++] = fd;
        }
        check_int("fd exhaustion count", 5, count);
        check_int("fd exhaustion errno", ENFILE, errno);
        for (int i = 0; i < count; i++) {
            close(fds[i]);
        }
    }

    if (failures == 0) {
        u_puts("errnoprobe PASS\n");
    } else {
        u_puts("errnoprobe FAIL\n");
    }
    sys_exit(failures == 0 ? 0 : 1);
}
