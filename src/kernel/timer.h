#ifndef TIMER_H
#define TIMER_H

#include "uapi_time.h"

void timer_init(unsigned int frequency);
void timer_handle_irq(void);

unsigned int timer_get_ticks(void);
unsigned int timer_get_seconds(void);
unsigned int timer_get_hz(void);
unsigned int timer_ms_to_ticks_round_up(unsigned int ms);

#endif
