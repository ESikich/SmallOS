#include "user_lib.h"

/*
 * exec_test — diagnostic SYS_EXEC test.
 *
 * Prints a numbered marker before and after every significant event so
 * it is immediately obvious exactly where execution stops.
 *
 * Expected output:
 *
 *   [1] exec_test alive
 *   [2] calling sys_exec hello
 *   hello from elf via int 0x80
 *   argc = 1
 *   argv[0] = "hello"
 *   ticks = ...
 *   [3] sys_exec returned 0
 *   [4] calling sys_exec with bad name
 *   [5] bad name returned -1
 *   [6] exec_test done
 */

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u_puts("[1] exec_test alive\n");

    u_puts("[2] calling sys_exec hello\n");
    char* av[] = { "hello", 0 };
    int r = sys_exec("hello", 1, av);
    u_puts("[3] sys_exec returned ");
    if (r < 0) {
        u_putc('-');
        u_put_uint((uint32_t)(-r));
    } else {
        u_put_uint((uint32_t)r);
    }
    u_putc('\n');

    u_puts("[4] calling sys_exec with bad name\n");
    int r2 = sys_exec("no_such_prog", 0, 0);
    u_puts("[5] bad name returned ");
    if (r2 < 0) {
        u_putc('-');
        u_put_uint((uint32_t)(-r2));
    } else {
        u_put_uint((uint32_t)r2);
    }
    u_putc('\n');

    u_puts("[6] exec_test done\n");
    sys_exit(0);
}