#include "stdio.h"
#include "stdlib.h"

void _start(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        fputs(argv[i], stdout);
        if (i != argc - 1) {
            putchar(' ');
        }
    }
    putchar('\n');
    exit(0);
}
