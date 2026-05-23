#ifndef DRIVERS_SOUND_H
#define DRIVERS_SOUND_H

#include "../kernel/uapi_sound.h"

int sound_tone(unsigned int frequency_hz, unsigned int duration_ms);
int sound_pit_divisor(unsigned int divisor, unsigned int duration_ms);
int sound_pit_sequence(const unsigned char* samples, unsigned int count,
                       unsigned int sample_hz, unsigned int divisor_scale);
int sound_pcm_u8(const unsigned char* samples, unsigned int count,
                 unsigned int sample_hz);
int sound_pcm_u8_legacy(const unsigned char* samples, unsigned int count,
                        unsigned int sample_hz);
int sound_pcm_u8_sb16_8(const unsigned char* samples, unsigned int count,
                        unsigned int sample_hz);
unsigned int sound_caps(void);
void sound_stop(void);
void sound_status(sys_sound_status_t* out);
void sound_irq_handler(void);
void sound_timer_tick(void);

#endif /* DRIVERS_SOUND_H */
