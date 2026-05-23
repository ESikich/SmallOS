#ifndef UAPI_SOUND_H
#define UAPI_SOUND_H

#define SYS_SOUND_OP_TONE        1u
#define SYS_SOUND_OP_STOP        2u
#define SYS_SOUND_OP_PIT_DIVISOR 3u
#define SYS_SOUND_OP_PIT_SEQUENCE 4u
#define SYS_SOUND_OP_PCM_U8      5u
#define SYS_SOUND_OP_CAPS        6u
#define SYS_SOUND_OP_STATUS      7u
#define SYS_SOUND_OP_PCM_U8_LEGACY 8u
#define SYS_SOUND_OP_PCM_U8_SB16_8 9u
#define SYS_SOUND_OP_OPL_WRITE   10u
#define SYS_SOUND_OP_OPL_RESET   11u

#define SYS_SOUND_CAP_PC_SPEAKER 0x00000001u
#define SYS_SOUND_CAP_PCM_U8     0x00000002u
#define SYS_SOUND_CAP_ADLIB      0x00000004u

#define SYS_SOUND_MIN_HZ          20u
#define SYS_SOUND_MAX_HZ       20000u
#define SYS_SOUND_MAX_DURATION_MS 10000u
#define SYS_SOUND_SEQUENCE_MAX_SAMPLES 16384u
#define SYS_SOUND_PCM_MAX_SAMPLES 65536u
#define SYS_SOUND_PCM_MIN_HZ      4000u
#define SYS_SOUND_PCM_MAX_HZ     44100u

typedef struct sys_sound_pit_sequence {
    const unsigned char* samples;
    unsigned int count;
    unsigned int sample_hz;
    unsigned int divisor_scale;
} sys_sound_pit_sequence_t;

typedef struct sys_sound_pcm_u8 {
    const unsigned char* samples;
    unsigned int count;
    unsigned int sample_hz;
} sys_sound_pcm_u8_t;

typedef struct sys_sound_status {
    unsigned int caps;
    unsigned int pcm_active;
    unsigned int pcm_irq_count;
    unsigned int pcm_timeout_count;
    unsigned int pcm_error_count;
    unsigned int pcm_last_count;
    unsigned int pcm_last_hz;
} sys_sound_status_t;

#endif /* UAPI_SOUND_H */
