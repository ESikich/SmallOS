#include "user_lib.h"

/*
 * spinwkr — helper for the preemption regression.
 *
 * Mode "early" writes its start tick immediately, then spins.
 * Mode "late" spins first, then writes both its start and end ticks.
 * The parent compares the resulting files to prove that the two
 * runnable tasks overlapped instead of running strictly one-at-a-time.
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

static uint32_t parse_u32(const char* s) {
    uint32_t n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10u + (uint32_t)(*s - '0');
        s++;
    }
    return n;
}

static void write_start_report(const char* file_name,
                               const char* mode,
                               uint32_t start) {
    char buf[64];
    unsigned int pos = 0;

    append_str(buf, &pos, "mode=");
    append_str(buf, &pos, mode);
    append_str(buf, &pos, " start=");
    append_uint(buf, &pos, start);
    append_str(buf, &pos, "\n");

    if (sys_writefile(file_name, buf, pos) < 0) {
        u_puts("spinwkr: writefile failed\n");
    }
}

static void write_full_report(const char* file_name,
                              const char* mode,
                              uint32_t start,
                              uint32_t end) {
    char buf[64];
    unsigned int pos = 0;

    append_str(buf, &pos, "mode=");
    append_str(buf, &pos, mode);
    append_str(buf, &pos, " start=");
    append_uint(buf, &pos, start);
    append_str(buf, &pos, " end=");
    append_uint(buf, &pos, end);
    append_str(buf, &pos, "\n");

    if (sys_writefile(file_name, buf, pos) < 0) {
        u_puts("spinwkr: writefile failed\n");
    }
}

void _start(int argc, char** argv) {
    if (argc < 4) {
        u_puts("spinwkr usage: <early|late> <file> <spin_ticks>\n");
        sys_exit(1);
    }

    const char* mode = argv[1];
    const char* file_name = argv[2];
    uint32_t spin_ticks = parse_u32(argv[3]);
    if (spin_ticks == 0) {
        spin_ticks = 1;
    }

    uint32_t start = sys_get_ticks();

    if (streq(mode, "early")) {
        write_start_report(file_name, mode, start);
    }

    /*
     * Busy-spin on ticks instead of yielding.  The test depends on the
     * timer interrupt preempting this process while another runnable
     * task is waiting.
     */
    while ((uint32_t)(sys_get_ticks() - start) < spin_ticks) {
        /* intentional busy loop */
    }

    uint32_t end = sys_get_ticks();
    if (streq(mode, "late")) {
        write_full_report(file_name, mode, start, end);
    }

    u_puts("spinwkr ");
    u_puts(mode);
    u_puts(" PASS\n");
    sys_exit(0);
}
