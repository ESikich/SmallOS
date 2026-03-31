#include "user_lib.h"

/*
 * yield_test — exercises SYS_YIELD.
 *
 * Spins in a counted loop, calling sys_yield() on every iteration.
 * Prints a tick-stamped banner at the start and end so you can
 * confirm the process ran to completion and that the scheduler kept
 * working throughout.
 *
 * Expected output (tick values will vary):
 *
 *   yield_test: start  ticks=142
 *   yield_test: iter 0  ticks=142
 *   yield_test: iter 1  ticks=153
 *   ...
 *   yield_test: iter 9  ticks=232
 *   yield_test: done   ticks=243
 *
 * Usage:
 *   runelf yield_test
 */

#define ITERS 10

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u_puts("yield_test: start  ticks=");
    u_put_uint(sys_get_ticks());
    u_puts("\n");

    for (int i = 0; i < ITERS; i++) {
        u_puts("yield_test: iter ");
        u_put_uint((uint32_t)i);
        u_puts("  ticks=");
        u_put_uint(sys_get_ticks());
        u_puts("\n");

        sys_yield();   /* voluntarily give up the remainder of this quantum */
    }

    u_puts("yield_test: done   ticks=");
    u_put_uint(sys_get_ticks());
    u_puts("\n");

    sys_exit(0);
}