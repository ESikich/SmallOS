#include "user_lib.h"
#include "errno.h"

/*
 * exec_test — diagnostic SYS_EXEC test for current blocking semantics.
 *
 * sys_exec("apps/demo/hello", ...) runs the child through the foreground
 * run-and-wait path. The parent resumes only after the child exits, so
 * this test expects ordered output around the call.
 */

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    int ok = 1;

    u_puts("[1] exec_test alive\n");

    u_puts("[2] calling sys_exec apps/demo/hello\n");
    char* av[] = { "apps/demo/hello", 0 };
    int r = sys_exec("apps/demo/hello", 1, av);
    u_puts("[3] sys_exec returned ");
    if (r < 0) {
        u_putc('-');
        u_put_uint((uint32_t)(-r));
    } else {
        u_put_uint((uint32_t)r);
    }
    u_putc('\n');
    if (r != 0) {
        ok = 0;
    }

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
    if (r2 != -ENOENT) {
        ok = 0;
    }

    u_puts("[6] exec_test done\n");
    sys_exit(ok ? 0 : 1);
}
