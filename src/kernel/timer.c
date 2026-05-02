#include "timer.h"
#include "ports.h"

static volatile unsigned int timer_ticks = 0;
static unsigned int timer_hz = SMALLOS_TIMER_HZ;

void timer_init(unsigned int frequency) {
    if (frequency == 0) {
        frequency = SMALLOS_TIMER_HZ;
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

unsigned int timer_get_hz(void) {
    return timer_hz;
}

unsigned int timer_ms_to_ticks_round_up(unsigned int ms) {
    unsigned int whole_seconds;
    unsigned int rem_ms;
    unsigned int ticks;

    if (ms == 0) {
        return 0;
    }
    if (timer_hz == 0) {
        return 0;
    }

    whole_seconds = ms / SMALLOS_MS_PER_SECOND;
    rem_ms = ms % SMALLOS_MS_PER_SECOND;
    ticks = whole_seconds * timer_hz;
    ticks += (rem_ms * timer_hz + (SMALLOS_MS_PER_SECOND - 1u)) / SMALLOS_MS_PER_SECOND;
    return ticks;
}
