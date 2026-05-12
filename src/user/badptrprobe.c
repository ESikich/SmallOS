#include "user_lib.h"
#include "errno.h"
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

void _start(int argc, char** argv) {
    char* bad = (char*)0x80000000u;
    uint32_t size = 0;
    int is_dir = 0;
    uint32_t rows = 0;
    uint32_t cols = 0;
    struct pollfd pfd;
    int epfd;
    struct epoll_event event;

    (void)argc;
    (void)argv;

    u_puts("badptrprobe start\n");

    check_int("write unmapped buffer", -EFAULT, sys_write(bad, 1));
    check_int("open unmapped path", -EFAULT, sys_open(bad));
    check_int("exec unmapped argv", -EFAULT,
              sys_exec("apps/demo/hello.elf", 1, (char**)bad));

    {
        int fd = sys_open("apps/demo/hello.elf");
        check_int("open fixture", 0, fd >= 0 ? 0 : fd);
        if (fd >= 0) {
            check_int("fread unmapped buffer", -EFAULT, sys_fread(fd, bad, 1));
            check_int("close fixture", 0, sys_close(fd));
        }
    }

    check_int("stat unmapped size out", -EFAULT,
              sys_stat("apps", (uint32_t*)bad, &is_dir));
    check_int("stat unmapped dir out", -EFAULT,
              sys_stat("apps", &size, (int*)bad));
    check_int("terminal size bad rows", -EFAULT,
              sys_terminal_size((uint32_t*)bad, &cols));
    check_int("terminal size bad cols", -EFAULT,
              sys_terminal_size(&rows, (uint32_t*)bad));

    pfd.fd = -1;
    pfd.events = POLLIN;
    pfd.revents = 0;
    check_int("poll overflow nfds", -EINVAL,
              sys_poll(&pfd, 0x40000000u, 0));

    epfd = sys_epoll_create(0);
    check_int("epoll create", 0, epfd >= 0 ? 0 : epfd);
    if (epfd >= 0) {
        check_int("epoll wait too many", -EINVAL,
                  sys_epoll_wait(epfd, &event, 65, 0));
        check_int("epoll wait bad events", -EFAULT,
                  sys_epoll_wait(epfd, (struct epoll_event*)bad, 1, 0));
        check_int("close epoll", 0, sys_close(epfd));
    }

    if (failures == 0) {
        u_puts("badptrprobe PASS\n");
    } else {
        u_puts("badptrprobe FAIL\n");
    }
    sys_exit(failures == 0 ? 0 : 1);
}
