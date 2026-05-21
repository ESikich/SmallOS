#include "stdio.h"
#include "stdlib.h"
#include "time.h"

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    puts("ticks program");
    printf("ticks = %u\n", (unsigned int)clock());

    exit(0);
}
