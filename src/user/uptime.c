#include "user_lib.h"

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u_puts("Ticks: ");
    u_put_uint(sys_get_ticks());
    u_putc('\n');
    u_puts("Seconds: ");
    u_put_uint((uint32_t)(sys_get_ticks() / SMALLOS_TIMER_HZ));
    u_putc('\n');
    sys_exit(0);
}
