#include "user_lib.h"

static int str_eq(const char* a, const char* b) {
    unsigned int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static void check(const char* name, int ok, int* failures) {
    u_puts(name);
    u_puts(": ");
    if (ok) {
        u_puts("PASS\n");
    } else {
        u_puts("FAIL\n");
        (*failures)++;
    }
}

/*
 * This program intentionally defines main(), not _start(), so the test covers
 * the user_crt0 adapter and verifies that main's return becomes exit status.
 */
int main(int argc, char** argv) {
    int failures = 0;

    u_puts("=== crtprobe begin ===\n");
    check("crtprobe argc", argc == 4, &failures);
    check("crtprobe argv[0]", argc > 0 && str_eq(argv[0], "apps/tests/crtprobe.elf"), &failures);
    check("crtprobe argv[1]", argc > 1 && str_eq(argv[1], "alpha"), &failures);
    check("crtprobe argv[2]", argc > 2 && str_eq(argv[2], "nested/path"), &failures);
    check("crtprobe long arg",
          argc > 3 && str_eq(argv[3], "longish-argument-0123456789abcdef"),
          &failures);
    check("crtprobe argv terminator", argv[argc] == 0, &failures);

    if (failures == 0) {
        u_puts("=== crtprobe PASS ===\n");
        return 7;
    }

    u_puts("=== crtprobe FAIL ===\n");
    return 1;
}
