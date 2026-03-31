#include "timer.h"
#include "ports.h"

static volatile unsigned int timer_ticks = 0;
static unsigned int timer_hz = 100;

void timer_init(unsigned int frequency) {
    if (frequency == 0) {
        frequency = 100;
    }

    timer_hz = frequency;

    unsigned int divisor = 1193180 / frequency;

    outb(0x43, 0x36);
    outb(0x40, (unsigned char)(divisor & 0xFF));
    outb(0x40, (unsigned char)((divisor >> 8) & 0xFF));
}

void timer_handle_irq(void) {
    timer_ticks++;
}

unsigned int timer_get_ticks(void) {
    return timer_ticks;
}

unsigned int timer_get_seconds(void) {
    if (timer_hz == 0) {
        return 0;
    }
    return timer_ticks / timer_hz;
}