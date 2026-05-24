#ifndef SMALLOS_INPUT_H
#define SMALLOS_INPUT_H

#include <stdint.h>
#include "uapi_input.h"
#include "uapi_syscall.h"

static inline int smallos_input_syscall1(int num, uint32_t arg1) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1)
        : "memory"
    );
    return ret;
}

static inline int smallos_input_syscall3(int num, uint32_t arg1,
                                         uint32_t arg2, uint32_t arg3) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline int smallos_input_read(sys_input_event_t* out_events,
                                     uint32_t max_events,
                                     uint32_t flags) {
    return smallos_input_syscall3(SYS_INPUT_READ, (uint32_t)out_events,
                                  max_events, flags);
}

static inline int smallos_input_wait_until(uint32_t deadline_ticks) {
    return smallos_input_syscall1(SYS_INPUT_WAIT_UNTIL, deadline_ticks);
}

#endif /* SMALLOS_INPUT_H */
