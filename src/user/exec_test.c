#include "user_lib.h"
#include "errno.h"

/*
 * exec_test — diagnostic SYS_EXEC spawn/error test.
 *
 * sys_exec("usr/bin/hello", ...) validates user pointers, copies argv into
 * kernel-owned storage, enqueues the child, returns its pid, and leaves it
 * waitable. This probe checks the success path plus negative errno returns
 * for bad names and invalid argument counts.
 */

static void append_char(char* buf, uint32_t* pos, char ch) {
    buf[*pos] = ch;
    *pos = *pos + 1u;
}

static void append_cstr(char* buf, uint32_t* pos, const char* s) {
    while (*s) {
        append_char(buf, pos, *s++);
    }
}

static void append_uint(char* buf, uint32_t* pos, uint32_t value) {
    char tmp[16];
    uint32_t n = 0;

    if (value == 0u) {
        append_char(buf, pos, '0');
        return;
    }

    while (value > 0u) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (n > 0u) {
        append_char(buf, pos, tmp[--n]);
    }
}

static void write_result_line(const char* prefix, int value) {
    char buf[96];
    uint32_t pos = 0;

    append_cstr(buf, &pos, prefix);
    if (value < 0) {
        append_char(buf, &pos, '-');
        append_uint(buf, &pos, (uint32_t)(-value));
    } else {
        append_uint(buf, &pos, (uint32_t)value);
    }
    append_char(buf, &pos, '\n');
    sys_write(buf, pos);
}

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    int ok = 1;

    u_puts("[1] exec_test alive\n");

    u_puts("[2] calling sys_exec usr/bin/hello\n");
    char* av[] = { "usr/bin/hello", 0 };
    int r = sys_exec("usr/bin/hello", 1, av);
    write_result_line("[3] sys_exec returned pid ", r);
    if (r <= 0) {
        ok = 0;
    } else {
        int status = -1;
        int wr = sys_waitpid(r, &status, 0);
        write_result_line("[3b] sys_waitpid returned ", wr);
        write_result_line("[3c] child status ", status);
        if (wr != r || status != 0) {
            ok = 0;
        }
    }

    u_puts("[4] calling sys_exec with bad name\n");
    int r2 = sys_exec("no_such_prog", 0, 0);
    write_result_line("[5] bad name returned ", r2);
    if (r2 != -ENOENT) {
        ok = 0;
    }

    u_puts("[6] calling sys_exec with too many args\n");
    int r3 = sys_exec("usr/bin/hello", 17, av);
    write_result_line("[7] too many args returned ", r3);
    if (r3 != -EINVAL) {
        ok = 0;
    }

    u_puts("[8] exec_test done\n");
    sys_exit(ok ? 0 : 1);
}
