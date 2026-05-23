#ifndef USER_SOUND_H
#define USER_SOUND_H

#include <stdint.h>
#include "uapi_sound.h"
#include "uapi_syscall.h"

static inline int smallos_sound_syscall3(int num, uint32_t arg1,
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

static inline int sound_tone(unsigned int frequency_hz,
                             unsigned int duration_ms) {
    return smallos_sound_syscall3(SYS_SOUND_OP, SYS_SOUND_OP_TONE,
                                  frequency_hz, duration_ms);
}

static inline int sound_pit_divisor(unsigned int divisor,
                                    unsigned int duration_ms) {
    return smallos_sound_syscall3(SYS_SOUND_OP, SYS_SOUND_OP_PIT_DIVISOR,
                                  divisor, duration_ms);
}

static inline int sound_pit_sequence(const unsigned char* samples,
                                     unsigned int count,
                                     unsigned int sample_hz,
                                     unsigned int divisor_scale) {
    sys_sound_pit_sequence_t req;

    req.samples = samples;
    req.count = count;
    req.sample_hz = sample_hz;
    req.divisor_scale = divisor_scale;
    return smallos_sound_syscall3(SYS_SOUND_OP, SYS_SOUND_OP_PIT_SEQUENCE,
                                  (uint32_t)&req, 0u);
}

static inline int sound_pcm_u8(const unsigned char* samples,
                               unsigned int count,
                               unsigned int sample_hz) {
    sys_sound_pcm_u8_t req;

    req.samples = samples;
    req.count = count;
    req.sample_hz = sample_hz;
    return smallos_sound_syscall3(SYS_SOUND_OP, SYS_SOUND_OP_PCM_U8,
                                  (uint32_t)&req, 0u);
}

static inline int sound_pcm_u8_legacy(const unsigned char* samples,
                                      unsigned int count,
                                      unsigned int sample_hz) {
    sys_sound_pcm_u8_t req;

    req.samples = samples;
    req.count = count;
    req.sample_hz = sample_hz;
    return smallos_sound_syscall3(SYS_SOUND_OP, SYS_SOUND_OP_PCM_U8_LEGACY,
                                  (uint32_t)&req, 0u);
}

static inline int sound_pcm_u8_sb16_8(const unsigned char* samples,
                                      unsigned int count,
                                      unsigned int sample_hz) {
    sys_sound_pcm_u8_t req;

    req.samples = samples;
    req.count = count;
    req.sample_hz = sample_hz;
    return smallos_sound_syscall3(SYS_SOUND_OP, SYS_SOUND_OP_PCM_U8_SB16_8,
                                  (uint32_t)&req, 0u);
}

static inline int sound_caps(void) {
    return smallos_sound_syscall3(SYS_SOUND_OP, SYS_SOUND_OP_CAPS, 0u, 0u);
}

static inline int sound_opl_write(unsigned int reg, unsigned int value) {
    return smallos_sound_syscall3(SYS_SOUND_OP, SYS_SOUND_OP_OPL_WRITE,
                                  reg, value);
}

static inline int sound_opl_reset(void) {
    return smallos_sound_syscall3(SYS_SOUND_OP, SYS_SOUND_OP_OPL_RESET,
                                  0u, 0u);
}

static inline int sound_status(sys_sound_status_t* out) {
    return smallos_sound_syscall3(SYS_SOUND_OP, SYS_SOUND_OP_STATUS,
                                  (uint32_t)out, 0u);
}

static inline int sound_stop(void) {
    return smallos_sound_syscall3(SYS_SOUND_OP, SYS_SOUND_OP_STOP, 0u, 0u);
}

#endif /* USER_SOUND_H */
