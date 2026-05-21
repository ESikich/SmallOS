#include "stdio.h"
#include "stdlib.h"
#include "time.h"

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    unsigned int ticks = (unsigned int)clock();
    printf("Ticks: %u\n", ticks);
    printf("Seconds: %u\n", ticks / SMALLOS_TIMER_HZ);
    exit(0);
}
