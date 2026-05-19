#ifndef UAPI_TIME_H
#define UAPI_TIME_H

/*
 * Global scheduler/timer tick rate.
 *
 * 300 Hz gives SmallOS a 3.33 ms tick, exact 60 FPS pacing
 * (5 ticks/frame), and a still-modest IRQ rate while the kernel uses a
 * simple periodic PIT tick for scheduling, sleeps, and wakeups.
 *
 * 1000 Hz may be worth revisiting when the desktop grows more latency
 * sensitive, or if we add better high-resolution/tickless timer support.
 * It gives 1 ms granularity, but costs over 3x as many timer interrupts
 * as 300 Hz.
 */
#define SMALLOS_TIMER_HZ       300u
#define SMALLOS_MS_PER_SECOND  1000u
#define SMALLOS_US_PER_SECOND  1000000u
#define SMALLOS_NS_PER_SECOND  1000000000u

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#endif /* UAPI_TIME_H */
