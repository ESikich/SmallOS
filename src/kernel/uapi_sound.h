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
#define SYS_SOUND_OP_OPL_SEQUENCE 12u
#define SYS_SOUND_OP_OPL_SEQUENCE_STOP 13u
#define SYS_SOUND_OP_OPL_EFFECT  14u
#define SYS_SOUND_OP_OPL_EFFECT_STOP 15u

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
#define SYS_SOUND_OPL_SEQUENCE_MAX_EVENTS 8192u
#define SYS_SOUND_OPL_EFFECT_MAX_SAMPLES 8192u
#define SYS_SOUND_OPL_SEQUENCE_FLAG_LOOP 0x00000001u

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

typedef struct sys_sound_opl_event {
    unsigned short reg_value;
    unsigned short delay;
} sys_sound_opl_event_t;

typedef struct sys_sound_opl_sequence {
    const sys_sound_opl_event_t* events;
    unsigned int count;
    unsigned int timer_hz;
    unsigned int flags;
} sys_sound_opl_sequence_t;

typedef struct sys_sound_opl_effect {
    const unsigned char* samples;
    unsigned int count;
    unsigned int sample_hz;
    unsigned int channel;
    unsigned char block;
    unsigned char feedback;
    unsigned char m_char;
    unsigned char c_char;
    unsigned char m_scale;
    unsigned char c_scale;
    unsigned char m_attack;
    unsigned char c_attack;
    unsigned char m_sus;
    unsigned char c_sus;
    unsigned char m_wave;
    unsigned char c_wave;
} sys_sound_opl_effect_t;

typedef struct sys_sound_status {
    unsigned int caps;
    unsigned int pcm_active;
    unsigned int pcm_irq_count;
    unsigned int pcm_timeout_count;
    unsigned int pcm_error_count;
    unsigned int pcm_last_count;
    unsigned int pcm_last_hz;
    unsigned int opl_sequence_active;
    unsigned int opl_effect_active;
} sys_sound_status_t;

#endif /* UAPI_SOUND_H */
