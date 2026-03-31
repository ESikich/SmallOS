#include "system.h"
#include "ports.h"

void system_halt(void) {
    __asm__ __volatile__("cli");

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

void system_reboot(void) {
    unsigned char good = 0x02;

    while (good & 0x02) {
        good = inb(0x64);
    }

    outb(0x64, 0xFE);

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}