#include "user_lib.h"

/*
 * sleep_test — end-to-end test for SYS_SLEEP.
 */
void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    const unsigned int sleep_ticks = 5;

    u_puts("sleep_test start\n");

    unsigned int before = sys_get_ticks();
    u_puts("before=");
    u_put_uint(before);
    u_putc('\n');

    if (sys_sleep(sleep_ticks) < 0) {
        u_puts("sys_sleep failed\n");
        sys_exit(1);
    }

    unsigned int after = sys_get_ticks();
    unsigned int delta = after - before;

    u_puts("after=");
    u_put_uint(after);
    u_puts(" delta=");
    u_put_uint(delta);
    u_putc('\n');

    if (delta < sleep_ticks) {
        u_puts("sleep_test FAIL\n");
        sys_exit(1);
    }

    u_puts("sleep_test PASS\n");
    sys_exit(0);
}
