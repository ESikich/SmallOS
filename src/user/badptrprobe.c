#include "user_lib.h"
#include "errno.h"
#include "dirent.h"
#include "sys/timerfd.h"
#include "sys/stat.h"

static int failures = 0;
static uint32_t boundary_page = 0;

#define PAGE_SIZE 4096u

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

static int setup_boundary_page(void) {
    uint32_t cur = sys_brk(0);
    uint32_t page = (cur + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    uint32_t new_brk = page + PAGE_SIZE;

    if (page < USER_HEAP_BASE || new_brk < page) {
        return -ENOMEM;
    }
    if (sys_brk(new_brk) != new_brk) {
        return -ENOMEM;
    }

    boundary_page = page;
    return 0;
}

static void* page_cross_ptr(uint32_t size) {
    return (void*)(boundary_page + PAGE_SIZE - size + 1u);
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

    check_int("boundary heap page", 0, setup_boundary_page());
    if (boundary_page) {
        char* page_end = (char*)(boundary_page + PAGE_SIZE - 1u);
        uint32_t* cross_u32 = (uint32_t*)page_cross_ptr(sizeof(uint32_t));
        sys_fsmap_request_t fsmap_req;

        *page_end = 'x';
        check_int("write page-cross buffer", -EFAULT,
                  sys_write(page_end, 2));
        check_int("read page-cross buffer", -EFAULT,
                  sys_read(page_end, 2));
        check_int("open page-cross path", -EFAULT,
                  sys_open(page_end));
        check_int("writefd page-cross buffer", -EFAULT,
                  sys_writefd(1, page_end, 2));
        check_int("stat page-cross size out", -EFAULT,
                  sys_stat("apps", cross_u32, &is_dir));
        check_int("terminal size page-cross rows", -EFAULT,
                  sys_terminal_size(cross_u32, &cols));
        check_int("dirlist page-cross out", -EFAULT,
                  sys_dirlist("apps", 0, (struct dirent*)page_cross_ptr(sizeof(struct dirent))));
        check_int("display info page-cross out", -EFAULT,
                  sys_display_info((sys_display_info_t*)page_cross_ptr(sizeof(sys_display_info_t))));
        check_int("fsinfo page-cross out", -EFAULT,
                  sys_fsinfo((sys_fsinfo_t*)page_cross_ptr(sizeof(sys_fsinfo_t))));
        check_int("fsmap page-cross request", -EFAULT,
                  sys_fsmap((sys_fsmap_request_t*)page_cross_ptr(sizeof(sys_fsmap_request_t))));

        fsmap_req.start_cluster = 0;
        fsmap_req.max_clusters = 2;
        fsmap_req.states = (unsigned char*)page_end;
        fsmap_req.out_clusters = 0;
        check_int("fsmap page-cross states", -EFAULT,
                  sys_fsmap(&fsmap_req));
    }

    pfd.fd = -1;
    pfd.events = POLLIN;
    pfd.revents = 0;
    check_int("poll overflow nfds", -EINVAL,
              sys_poll(&pfd, 0x40000000u, 0));
    if (boundary_page) {
        check_int("poll page-cross fds", -EFAULT,
                  sys_poll((struct pollfd*)page_cross_ptr(sizeof(struct pollfd)), 1, 0));
    }

    epfd = sys_epoll_create(0);
    check_int("epoll create", 0, epfd >= 0 ? 0 : epfd);
    if (epfd >= 0) {
        check_int("epoll wait too many", -EINVAL,
                  sys_epoll_wait(epfd, &event, 65, 0));
        check_int("epoll wait bad events", -EFAULT,
                  sys_epoll_wait(epfd, (struct epoll_event*)bad, 1, 0));
        if (boundary_page) {
            check_int("epoll ctl page-cross event", -EFAULT,
                      sys_epoll_ctl(epfd, EPOLL_CTL_ADD, 0,
                                    (struct epoll_event*)page_cross_ptr(sizeof(struct epoll_event))));
            check_int("epoll wait page-cross events", -EFAULT,
                      sys_epoll_wait(epfd,
                                     (struct epoll_event*)page_cross_ptr(sizeof(struct epoll_event)),
                                     1, 0));
        }
        check_int("close epoll", 0, sys_close(epfd));
    }

    if (boundary_page) {
        struct itimerspec spec;
        int tfd;

        memset(&spec, 0, sizeof(spec));
        tfd = sys_timerfd_create(CLOCK_MONOTONIC, 0);
        check_int("timerfd create", 0, tfd >= 0 ? 0 : tfd);
        if (tfd >= 0) {
            check_int("timerfd page-cross new", -EFAULT,
                      sys_timerfd_settime(tfd, 0,
                                          page_cross_ptr(sizeof(struct itimerspec)), 0));
            check_int("timerfd page-cross old", -EFAULT,
                      sys_timerfd_settime(tfd, 0, &spec,
                                          page_cross_ptr(sizeof(struct itimerspec))));
            check_int("close timerfd", 0, sys_close(tfd));
        }

        check_int("signalfd page-cross mask", -EFAULT,
                  sys_signalfd(-1, page_cross_ptr(sizeof(uint32_t)), 0));
    }

    if (failures == 0) {
        u_puts("badptrprobe PASS\n");
    } else {
        u_puts("badptrprobe FAIL\n");
    }
    sys_exit(failures == 0 ? 0 : 1);
}
