#include "user_lib.h"

static int g_failures = 0;

static void check_true(const char* label, int cond) {
    u_puts(label);
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

    u_puts("heapprobe start\n");

    unsigned char* a = (unsigned char*)malloc(24);
    unsigned char* b = (unsigned char*)malloc(48);
    unsigned char* c = (unsigned char*)calloc(4, 8);

    check_true("malloc a", a != 0);
    check_true("malloc b", b != 0);
    check_true("calloc c", c != 0);

    if (a) {
        for (unsigned int i = 0; i < 24; i++) {
            a[i] = (unsigned char)(i + 1);
        }
    }

    if (b) {
        for (unsigned int i = 0; i < 48; i++) {
            b[i] = (unsigned char)(0xA0u + i);
        }
    }

    if (c) {
        unsigned char zeros[8] = { 0 };
        check_true("calloc zeroed", memcmp(c, zeros, 8) == 0);
    }

    a = (unsigned char*)realloc(a, 64);
    check_true("realloc a", a != 0);
    if (a) {
        check_true("realloc preserved", a[0] == 1 && a[23] == 24);
    }

    free(b);
    free(c);
    free(a);

    {
        unsigned char* d = (unsigned char*)malloc(32);
        check_true("reuse after free", d != 0);
        free(d);
    }

    if (g_failures == 0) {
        u_puts("heapprobe PASS\n");
        sys_exit(0);
    } else {
        u_puts("heapprobe FAIL\n");
        sys_exit(1);
    }
}
