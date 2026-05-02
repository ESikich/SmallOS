#include "user_lib.h"
#include "errno.h"

/*
 * exec_test — diagnostic SYS_EXEC spawn/error test.
 *
 * sys_exec("apps/demo/hello", ...) validates user pointers, copies argv into
 * kernel-owned storage, enqueues the child, and returns immediately.  This
 * probe checks the success path plus negative errno returns for bad names and
 * invalid argument counts.
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

    u_puts("[6] calling sys_exec with too many args\n");
    int r3 = sys_exec("apps/demo/hello", 17, av);
    u_puts("[7] too many args returned ");
    if (r3 < 0) {
        u_putc('-');
        u_put_uint((uint32_t)(-r3));
    } else {
        u_put_uint((uint32_t)r3);
    }
    u_putc('\n');
    if (r3 != -EINVAL) {
        ok = 0;
    }

    u_puts("[8] exec_test done\n");
    sys_exit(ok ? 0 : 1);
}
