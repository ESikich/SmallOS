#include "user_lib.h"

typedef unsigned int u32;

static int g_data_value = 0x12345678;   /* .data */
static int g_bss_value;                 /* .bss should start as 0 */

static int g_failures = 0;
static int g_checks = 0;

static void put_hex_digit(unsigned int v) {
    if (v < 10) {
        u_putc('0' + v);
    } else {
        u_putc('A' + (v - 10));
    }
}

static void u_put_hex32(u32 value) {
    u_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        put_hex_digit((value >> shift) & 0xF);
    }
}

static void u_put_int(int value) {
    if (value < 0) {
        u_putc('-');
        u_put_uint((u32)(-value));
    } else {
        u_put_uint((u32)value);
    }
}

static int str_eq(const char* a, const char* b) {
    unsigned int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static void check_true(const char* name, int cond) {
    g_checks++;
    u_puts("[");
    u_put_uint((u32)g_checks);
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

static void check_u32_eq(const char* name, u32 expected, u32 actual) {
    g_checks++;
    u_puts("[");
    u_put_uint((u32)g_checks);
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
    u_put_hex32(expected);
    u_puts(", actual=");
    u_put_hex32(actual);
    u_puts(")\n");
}

static void check_int_eq(const char* name, int expected, int actual) {
    g_checks++;
    u_puts("[");
    u_put_uint((u32)g_checks);
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
    u_put_int(expected);
    u_puts(", actual=");
    u_put_int(actual);
    u_puts(")\n");
}

static u32 add3(u32 a, u32 b, u32 c) {
    return a + b + c;
}

static void check_argv(int argc, char** argv) {
    check_true("argc >= 1", argc >= 1);

    if (argc >= 1) {
        check_true("argv[0] == runelf_test", str_eq(argv[0], "runelf_test"));
    }

    if (argc >= 2) {
        check_true("argv[1] == alpha", str_eq(argv[1], "alpha"));
    }

    if (argc >= 3) {
        check_true("argv[2] == beta", str_eq(argv[2], "beta"));
    }

    if (argc >= 4) {
        check_true("argv[3] == gamma", str_eq(argv[3], "gamma"));
    }
}

void _start(int argc, char** argv) {
    u_puts("=== runelf test begin ===\n");

    /* argv / argc forwarding */
    check_int_eq("argc", 4, argc);
    check_argv(argc, argv);

    /* .data and .bss correctness after PT_LOAD copy + zero-fill */
    check_u32_eq(".data init", 0x12345678u, (u32)g_data_value);
    check_u32_eq(".bss zero", 0u, (u32)g_bss_value);

    g_data_value += 7;
    g_bss_value = 99;
    check_u32_eq(".data writable", 0x1234567Fu, (u32)g_data_value);
    check_u32_eq(".bss writable", 99u, (u32)g_bss_value);

    /* function calls / stack */
    check_u32_eq("function call result", 60u, add3(10, 20, 30));

    {
        u32 local_a = 11;
        u32 local_b = 22;
        u32 local_c = 33;
        check_u32_eq("stack locals", 66u, local_a + local_b + local_c);
    }

    /* syscall return values */
    {
        static const char msg[] = "write-ok\n";
        int ret = sys_write(msg, (u32)(sizeof(msg) - 1));
        check_int_eq("sys_write return len", (int)(sizeof(msg) - 1), ret);
    }

    check_int_eq("sys_putc return 1", 1, sys_putc('!'));
    u_putc('\n');

    /* ticks should not go backwards */
    {
        u32 t1 = sys_get_ticks();
        u32 t2 = sys_get_ticks();
        check_true("sys_get_ticks monotonic", t2 >= t1);
    }

    u_puts("checks = ");
    u_put_uint((u32)g_checks);
    u_puts(", failures = ");
    u_put_uint((u32)g_failures);
    u_puts("\n");

    if (g_failures == 0) {
        u_puts("=== runelf test PASS ===\n");
        sys_exit(0);
    } else {
        u_puts("=== runelf test FAIL ===\n");
        sys_exit(1);
    }

    /*
    * sys_exit() should not return. If it does, treat that as a failure path.
    */
    u_puts("runelf test: ERROR returned from sys_exit\n");
    for (;; ) { }
}