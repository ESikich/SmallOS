#ifndef TIMER_H
#define TIMER_H

void timer_init(unsigned int frequency);
void timer_handle_irq(void);

unsigned int timer_get_ticks(void);
unsigned int timer_get_seconds(void);

#endif