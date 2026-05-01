#include "user_lib.h"

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
    check_stat("hello", "hello.elf");
    check_stat("demo_dir", "apps/demo");
    check_stat("demo_hello", "apps/demo/hello.elf");
    u_puts("statprobe PASS\n");
    sys_exit(0);
}
