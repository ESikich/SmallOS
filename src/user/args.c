#include "stdio.h"
#include "stdlib.h"

void _start(int argc, char** argv) {
    printf("argc = %d\n", argc);

    for (int i = 0; i < argc; i++) {
        printf("argv[%d] = %s\n", i, argv[i]);
    }

    exit(0);
}
