#include "stdio.h"
#include "stdlib.h"

void __assert_fail(const char* expr, const char* file, unsigned int line, const char* func) {
    fprintf(stderr, "%s:%u: %s: assertion failed: %s\n",
            file ? file : "(unknown)",
            line,
            func ? func : "(unknown)",
            expr ? expr : "(unknown)");
    exit(134);
}
