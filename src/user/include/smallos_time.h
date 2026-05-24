#ifndef SMALLOS_TIME_H
#define SMALLOS_TIME_H

#include <stdint.h>
#include "smallos_input.h"
#include "uapi_syscall.h"
#include "uapi_time.h"

typedef struct smallos_tick_clock {
    uint32_t unit_hz;
    uint32_t game_hz;
    uint32_t kernel_hz;
    uint32_t units_per_game_tick;
    uint32_t units_per_kernel_tick;
    uint32_t base_units;
    uint32_t observed_ticks;
    int ready;
} smallos_tick_clock_t;

typedef void (*smallos_tick_clock_hook_t)(uint32_t now_units, void* ctx);

static inline int smallos_time_syscall0(int num) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "memory"
    );
    return ret;
}

static inline uint32_t smallos_ticks_now(void) {
    return (uint32_t)smallos_time_syscall0(SYS_GET_TICKS);
}

static inline int smallos_tick_clock_init(smallos_tick_clock_t* clock,
                                          uint32_t game_hz,
                                          uint32_t unit_hz) {
    if (!clock || game_hz == 0u || unit_hz == 0u ||
        unit_hz < game_hz || unit_hz < SMALLOS_TIMER_HZ ||
        (unit_hz % game_hz) != 0u ||
        (unit_hz % SMALLOS_TIMER_HZ) != 0u) {
        return -1;
    }

    clock->unit_hz = unit_hz;
    clock->game_hz = game_hz;
    clock->kernel_hz = SMALLOS_TIMER_HZ;
    clock->units_per_game_tick = unit_hz / game_hz;
    clock->units_per_kernel_tick = unit_hz / SMALLOS_TIMER_HZ;
    clock->base_units = 0u;
    clock->observed_ticks = 0u;
    clock->ready = 0;
    return 0;
}

static inline uint32_t smallos_tick_clock_now_units(
    const smallos_tick_clock_t* clock) {
    if (!clock || clock->units_per_kernel_tick == 0u) {
        return 0u;
    }
    return smallos_ticks_now() * clock->units_per_kernel_tick;
}

static inline uint32_t smallos_tick_clock_ticks_to_units(
    const smallos_tick_clock_t* clock,
    uint32_t ticks) {
    return clock ? ticks * clock->units_per_game_tick : 0u;
}

static inline uint32_t smallos_tick_clock_units_to_ticks(
    const smallos_tick_clock_t* clock,
    uint32_t units) {
    if (!clock || clock->units_per_game_tick == 0u) {
        return 0u;
    }
    return units / clock->units_per_game_tick;
}

static inline int smallos_tick_clock_sync(smallos_tick_clock_t* clock,
                                          uint32_t* ticks,
                                          uint32_t* out_now_units) {
    uint32_t now;
    uint32_t elapsed_ticks;

    if (!clock || !ticks || clock->units_per_game_tick == 0u ||
        clock->units_per_kernel_tick == 0u) {
        return -1;
    }

    now = smallos_tick_clock_now_units(clock);
    if (out_now_units) {
        *out_now_units = now;
    }

    if (!clock->ready || *ticks != clock->observed_ticks) {
        clock->base_units =
            now - smallos_tick_clock_ticks_to_units(clock, *ticks);
        clock->observed_ticks = *ticks;
        clock->ready = 1;
        return 1;
    }

    elapsed_ticks =
        smallos_tick_clock_units_to_ticks(clock, now - clock->base_units);
    if (elapsed_ticks > *ticks) {
        *ticks = elapsed_ticks;
    }
    clock->observed_ticks = *ticks;
    return 0;
}

static inline int smallos_tick_clock_wait(smallos_tick_clock_t* clock,
                                          uint32_t* ticks,
                                          uint32_t wait_ticks,
                                          smallos_tick_clock_hook_t hook,
                                          void* hook_ctx) {
    uint32_t target;
    uint32_t target_units;
    uint32_t target_kernel_ticks;
    uint32_t now_units;

    if (!clock || !ticks || clock->unit_hz == 0u ||
        clock->units_per_kernel_tick == 0u ||
        clock->units_per_game_tick == 0u) {
        return -1;
    }
    if (!wait_ticks) {
        wait_ticks = 1u;
    }

    (void)smallos_tick_clock_sync(clock, ticks, &now_units);
    if (hook) {
        hook(now_units, hook_ctx);
    }

    target = *ticks + wait_ticks;
    target_units =
        clock->base_units + smallos_tick_clock_ticks_to_units(clock, target);
    target_kernel_ticks =
        (target_units + clock->units_per_kernel_tick - 1u) /
        clock->units_per_kernel_tick;

    while (1) {
        uint32_t now_kernel_ticks = smallos_ticks_now();
        uint32_t remaining;
        uint32_t wait_units;
        uint32_t sleep_ticks;
        uint32_t deadline;

        now_units = now_kernel_ticks * clock->units_per_kernel_tick;
        if ((int32_t)(target_units - now_units) <= 0) {
            break;
        }
        if (hook) {
            hook(now_units, hook_ctx);
        }

        remaining = target_units - now_units;
        if (remaining > clock->unit_hz / 20u) {
            remaining = clock->unit_hz / 20u;
        }
        wait_units = remaining ? remaining : 1u;
        sleep_ticks =
            (wait_units + clock->units_per_kernel_tick - 1u) /
            clock->units_per_kernel_tick;
        if (!sleep_ticks) {
            sleep_ticks = 1u;
        }

        deadline = now_kernel_ticks + sleep_ticks;
        if ((int32_t)(deadline - target_kernel_ticks) > 0) {
            deadline = target_kernel_ticks;
        }
        if ((int32_t)(deadline - now_kernel_ticks) <= 0) {
            deadline = now_kernel_ticks + 1u;
        }
        (void)smallos_input_wait_until(deadline);
    }

    if (hook) {
        hook(now_units, hook_ctx);
    }
    *ticks = target;
    clock->observed_ticks = target;
    clock->ready = 1;
    return 0;
}

#endif /* SMALLOS_TIME_H */
