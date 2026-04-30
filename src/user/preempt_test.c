#include "user_lib.h"

/*
 * preempt_test — regression for timer-driven preemption.
 *
 * The test launches two background workers:
 *   - one captures its start tick immediately
 *   - the other spins first and only records its end tick later
 *
 * If the scheduler is truly preemptive, the second worker should be
 * able to start before the first worker finishes spinning.  The parent
 * proves this by reading the workers' report files and checking that
 * the later start tick is still earlier than the earliest end tick.
 */

static void append_str(char* buf, unsigned int* pos, const char* s) {
    while (*s != '\0') {
        buf[(*pos)++] = *s++;
    }
}

static void append_uint(char* buf, unsigned int* pos, uint32_t value) {
    char tmp[16];
    unsigned int n = 0;

    if (value == 0) {
        buf[(*pos)++] = '0';
        return;
    }

    while (value > 0) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (n > 0) {
        buf[(*pos)++] = tmp[--n];
    }
}

static int streq(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int parse_field(const char* buf, const char* key, uint32_t* out) {
    const char* p = buf;
    while (*p != '\0') {
        const char* a = p;
        const char* b = key;
        while (*a != '\0' && *b != '\0' && *a == *b) {
            a++;
            b++;
        }
        if (*b == '\0') {
            uint32_t n = 0;
            int found = 0;
            while (*a >= '0' && *a <= '9') {
                n = n * 10u + (uint32_t)(*a - '0');
                a++;
                found = 1;
            }
            if (!found) return 0;
            *out = n;
            return 1;
        }
        p++;
    }
    return 0;
}

static void wait_for_report(const char* name, char* buf, uint32_t buf_len) {
    for (int i = 0; i < 200; i++) {
        int fd = sys_open(name);
        if (fd >= 0) {
            int n = sys_fread(fd, buf, buf_len - 1);
            sys_close(fd);
            if (n > 0) {
                buf[n] = '\0';
                return;
            }
        }
        sys_sleep(1);
    }
    buf[0] = '\0';
}

static void print_report(const char* label, uint32_t start, uint32_t end) {
    u_puts(label);
    u_puts(" start=");
    u_put_uint(start);
    u_puts(" end=");
    u_put_uint(end);
    u_putc('\n');
}

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    static const char a_file[] = "PREEMPA.TXT";
    static const char b_file[] = "PREEMPB.TXT";

    u_puts("preempt_test start\n");

    char* a_argv[] = { "spinwkr", "late", (char*)a_file, "30", 0 };
    char* b_argv[] = { "spinwkr", "early", (char*)b_file, "30", 0 };

    if (sys_exec("spinwkr", 4, a_argv) < 0) {
        u_puts("preempt_test: launch A failed\n");
        sys_exit(1);
    }
    if (sys_exec("spinwkr", 4, b_argv) < 0) {
        u_puts("preempt_test: launch B failed\n");
        sys_exit(1);
    }

    sys_sleep(80);

    char a_buf[64];
    char b_buf[64];
    wait_for_report(a_file, a_buf, sizeof(a_buf));
    wait_for_report(b_file, b_buf, sizeof(b_buf));

    uint32_t a_start = 0, a_end = 0, b_start = 0;
    int ok = 1;

    if (a_buf[0] == '\0' || b_buf[0] == '\0') {
        u_puts("preempt_test: missing report\n");
        ok = 0;
    } else if (!parse_field(a_buf, "start=", &a_start) ||
               !parse_field(a_buf, "end=", &a_end) ||
               !parse_field(b_buf, "start=", &b_start)) {
        u_puts("preempt_test: parse failed\n");
        ok = 0;
    } else {
        print_report("A", a_start, a_end);
        u_puts("B start=");
        u_put_uint(b_start);
        u_putc('\n');

        if (!(b_start < a_end)) {
            u_puts("preempt_test: overlap check failed\n");
            ok = 0;
        }
    }

    if (ok) {
        u_puts("=== preempt_test PASS ===\n");
        sys_exit(0);
    }

    u_puts("=== preempt_test FAIL ===\n");
    sys_exit(1);
}
