#include "stdio.h"
#include "stdlib.h"
#include "time.h"

void _start(int argc, char** argv) {
    puts("hello from user mode via int 0x80");
    printf("argc = %d\n", argc);

    for (int i = 0; i < argc; i++) {
        printf("argv[%d] = \"%s\"\n", i, argv[i]);
    }

    printf("ticks = %u\n", (unsigned int)clock());

    exit(0);
}
