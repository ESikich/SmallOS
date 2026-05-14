#include "user_lib.h"
#include "sys/stat.h"
#include "unistd.h"

static void check_stat(const char* label, const char* path) {
    uint32_t size = 0;
    int is_dir = -1;
    int r = u_stat(path, &size, &is_dir);
    u_puts(label);
    u_puts(": ");
    if (r == 0) {
        u_puts("ok size=");
        u_put_uint(size);
        u_puts(" dir=");
        u_put_uint((uint32_t)is_dir);
    } else {
        u_puts("fail");
    }
    u_putc('\n');
}

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u_puts("statprobe start\n");
    check_stat("demo_dir", "usr/bin");
    check_stat("demo_hello", "usr/bin/hello.elf");
    check_stat("tests_dir", "usr/libexec/tests");
    {
        struct stat st;
        if (stat("usr/./libexec/../bin", &st) == 0 && S_ISDIR(st.st_mode)) {
            u_puts("statprobe posix dir: PASS\n");
        } else {
            u_puts("statprobe posix dir: FAIL\n");
            sys_exit(1);
        }
    }
    {
        struct stat st;
        if (stat("usr/bin/hello.elf", &st) == 0 &&
            S_ISREG(st.st_mode) &&
            st.st_ino != 0 &&
            st.st_nlink >= 1 &&
            st.st_blksize == 4096 &&
            st.st_blocks >= 1 &&
            (st.st_mode & 0777) == 0644) {
            u_puts("statprobe metadata: PASS\n");
        } else {
            u_puts("statprobe metadata: FAIL\n");
            sys_exit(1);
        }
    }
    if (access("usr/bin/hello.elf", R_OK) == 0 && access("usr/bin/nope.elf", F_OK) < 0) {
        u_puts("statprobe access: PASS\n");
    } else {
        u_puts("statprobe access: FAIL\n");
        sys_exit(1);
    }
    u_puts("statprobe PASS\n");
    sys_exit(0);
}
