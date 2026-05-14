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

static int buffer_contains(const char* haystack, int haystack_len, const char* needle) {
    uint32_t needle_len = str_len(needle);

    if (needle_len == 0u) return 1;
    if (haystack_len < 0 || (uint32_t)haystack_len < needle_len) return 0;

    for (uint32_t i = 0; i + needle_len <= (uint32_t)haystack_len; i++) {
        uint32_t j = 0;
        while (j < needle_len && haystack[i + j] == needle[j]) {
            j++;
        }
        if (j == needle_len) return 1;
    }

    return 0;
}

static int check_foreground_exec_stdout_inheritance(void) {
    const char* path = "var/tmp/exec_stdio.tmp";
    char buf[256];
    char* av[] = { "usr/bin/hello", "stdio-check", 0 };
    int saved_stdout;
    int out;
    int pid = -1;
    int status = -1;
    int ok = 1;
    int n = -1;

    sys_unlink(path);

    saved_stdout = sys_dup(1);
    if (saved_stdout < 0) return 0;

    out = sys_open_mode(path,
                        SYS_OPEN_MODE_WRITE |
                        SYS_OPEN_MODE_CREATE |
                        SYS_OPEN_MODE_TRUNC);
    if (out < 0) {
        sys_close(saved_stdout);
        return 0;
    }

    if (sys_dup2(out, 1) != 1) {
        ok = 0;
    } else {
        pid = sys_exec_foreground("usr/bin/hello", 2, av);
        if (pid <= 0 || sys_waitpid_foreground(pid, &status) != pid || status != 0) {
            ok = 0;
        }
        sys_fsync(1);
    }

    if (sys_dup2(saved_stdout, 1) != 1) {
        ok = 0;
    }
    sys_close(saved_stdout);
    sys_close(out);

    if (ok) {
        int in = sys_open(path);
        if (in >= 0) {
            n = sys_fread(in, buf, sizeof(buf) - 1u);
            sys_close(in);
        }
        if (n < 0) {
            ok = 0;
        } else {
            buf[n] = '\0';
            ok = buffer_contains(buf, n, "hello from elf via int 0x80");
        }
    }

    sys_unlink(path);
    return ok;
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

    u_puts("[8] checking foreground exec stdout inheritance\n");
    if (check_foreground_exec_stdout_inheritance()) {
        u_puts("[8b] foreground exec stdout inheritance PASS\n");
    } else {
        u_puts("[8b] foreground exec stdout inheritance FAIL\n");
        ok = 0;
    }

    u_puts("[8] exec_test done\n");
    sys_exit(ok ? 0 : 1);
}
