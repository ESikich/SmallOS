#include <stdio.h>

int main(void) {
    volatile int* crash = (int*)0;
    printf("tcc run fault before crash\n");
    return *crash;
}
