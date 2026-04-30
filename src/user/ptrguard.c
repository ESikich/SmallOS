#include "user_lib.h"

static int g_failures = 0;
static int g_checks = 0;

static void check_int(const char* name, int expected, int actual) {
    g_checks++;
    u_puts("[");
    u_put_uint((unsigned int)g_checks);
    u_puts("] ");
    u_puts(name);
    u_puts(": ");

    if (expected == actual) {
        u_puts("PASS");
    } else {
        u_puts("FAIL");
        g_failures++;
    }

    u_puts(" (expected=");
    if (expected < 0) {
        u_putc('-');
        u_put_uint((unsigned int)(-expected));
    } else {
        u_put_uint((unsigned int)expected);
    }
    u_puts(", actual=");
    if (actual < 0) {
        u_putc('-');
        u_put_uint((unsigned int)(-actual));
    } else {
        u_put_uint((unsigned int)actual);
    }
    u_puts(")\n");
}

static void check_true(const char* name, int cond) {
    g_checks++;
    u_puts("[");
    u_put_uint((unsigned int)g_checks);
    u_puts("] ");
    u_puts(name);
    u_puts(": ");

    if (cond) {
        u_puts("PASS\n");
    } else {
        u_puts("FAIL\n");
        g_failures++;
    }
}

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    static const char ok_msg[] = "ptrguard: start\n";
    u_puts(ok_msg);

    check_int("sys_write invalid buf",
              -1,
              sys_write((const char*)0x1234, 4));

    check_int("sys_read invalid buf",
              -1,
              sys_read((char*)0x1234, 4));

    check_int("sys_open invalid name",
              -1,
              sys_open((const char*)0x1234));

    check_int("sys_writefile invalid name",
              -1,
              sys_writefile((const char*)0x1234, "x", 1));

    {
        int fd = sys_open("hello.elf");
        check_true("sys_open hello", fd >= 3);
        if (fd >= 3) {
            check_int("sys_fread invalid buf",
                      -1,
                      sys_fread(fd, (char*)0x1234, 4));
            sys_close(fd);
        }
    }

    {
        char* bad_argv[] = { (char*)0x1234, 0 };
        check_int("sys_exec invalid argv",
                  -1,
                  sys_exec("hello", 1, bad_argv));
    }

    u_puts("checks = ");
    u_put_uint((unsigned int)g_checks);
    u_puts(", failures = ");
    u_put_uint((unsigned int)g_failures);
    u_putc('\n');

    if (g_failures == 0) {
        u_puts("=== ptrguard PASS ===\n");
        sys_exit(0);
    } else {
        u_puts("=== ptrguard FAIL ===\n");
        sys_exit(1);
    }
}
