#include "sound.h"
#include "../kernel/klib.h"
#include "../kernel/ports.h"
#include "../kernel/timer.h"
#include "../kernel/uapi_errno.h"
#include "../kernel/uapi_sound.h"

#define PIT_BASE_HZ 1193182u
#define PIT_MODE_PORT 0x43
#define PIT_SPEAKER_PORT 0x42
#define PC_SPEAKER_PORT 0x61
#define SB_BASE_PORT 0x220u
#define SB_RESET_PORT (SB_BASE_PORT + 0x06u)
#define SB_READ_PORT (SB_BASE_PORT + 0x0au)
#define SB_WRITE_PORT (SB_BASE_PORT + 0x0cu)
#define SB_WRITE_STATUS_PORT (SB_BASE_PORT + 0x0cu)
#define SB_DATA_AVAIL_PORT (SB_BASE_PORT + 0x0eu)
#define SB_MIXER_ADDR_PORT (SB_BASE_PORT + 0x04u)
#define SB_MIXER_DATA_PORT (SB_BASE_PORT + 0x05u)
#define SB_MIXER_IRQ_SELECT 0x80u
#define SB_MIXER_DMA_SELECT 0x81u
#define SB_MIXER_IRQ5 0x02u
#define SB_MIXER_DMA1_AND_5 0x22u
#define SB_DMA_CHANNEL 1u
#define SB_DMA_MASK_PORT 0x0au
#define SB_DMA_FLIPFLOP_PORT 0x0cu
#define SB_DMA_MODE_PORT 0x0bu
#define SB_DMA_ADDR_PORT 0x02u
#define SB_DMA_COUNT_PORT 0x03u
#define SB_DMA_PAGE_PORT 0x83u
#define SB_DMA16_CHANNEL 5u
#define SB_DMA16_INDEX 1u
#define SB_DMA16_MASK_PORT 0xd4u
#define SB_DMA16_FLIPFLOP_PORT 0xd8u
#define SB_DMA16_MODE_PORT 0xd6u
#define SB_DMA16_ADDR_PORT 0xc4u
#define SB_DMA16_COUNT_PORT 0xc6u
#define SB_DMA16_PAGE_PORT 0x8bu
#define SB_DMA_MODE_SINGLE_PLAY 0x49u
#define SB_CMD_SPEAKER_ON 0xd1u
#define SB_CMD_SPEAKER_OFF 0xd3u
#define SB_CMD_DMA_PAUSE 0xd0u
#define SB_CMD_DMA16_PAUSE 0xd5u
#define SB_CMD_SET_TIME_CONSTANT 0x40u
#define SB_CMD_SET_OUTPUT_RATE 0x41u
#define SB_CMD_SINGLE_CYCLE_8BIT 0x14u
#define SB_CMD_SINGLE_CYCLE_16BIT_OUTPUT 0xb0u
#define SB_CMD_SINGLE_CYCLE_8BIT_OUTPUT 0xc0u
#define SB_MODE_UNSIGNED_MONO 0x00u
#define SB_MODE_SIGNED_MONO 0x10u
#define SB_IO_RETRY 4096u
#define OPL_STATUS_PORT 0x388u
#define OPL_ADDR_PORT 0x388u
#define OPL_DATA_PORT 0x389u
#define OPL_REG_TIMER1 0x02u
#define OPL_REG_TIMER_CTRL 0x04u
#define OPL_REG_WAVEFORM 0x01u
#define OPL_REG_CSM_SEL 0x08u

#define SB_PCM_MODE_16BIT 0
#define SB_PCM_MODE_LEGACY_8BIT 1
#define SB_PCM_MODE_SB16_8BIT 2
#define SB_PCM_STREAM_CHUNK_SAMPLES 384u

static volatile unsigned int s_sound_active;
static volatile unsigned int s_sound_deadline_tick;
static unsigned char s_sound_seq[SYS_SOUND_SEQUENCE_MAX_SAMPLES];
static volatile unsigned int s_sound_seq_active;
static unsigned int s_sound_seq_len;
static unsigned int s_sound_seq_pos;
static unsigned int s_sound_seq_accum;
static unsigned int s_sound_seq_sample_hz;
static unsigned int s_sound_seq_timer_hz;
static unsigned int s_sound_seq_divisor_scale;
static unsigned int s_sound_seq_last_sample;
static unsigned char s_sound_pcm_buf[SYS_SOUND_PCM_MAX_SAMPLES]
    __attribute__((aligned(65536)));
static unsigned char s_sound_pcm16_buf[SYS_SOUND_PCM_MAX_SAMPLES * 2u]
    __attribute__((aligned(131072)));
static volatile unsigned int s_sound_pcm_active;
static unsigned int s_sound_pcm_deadline_tick;
static unsigned int s_sound_pcm_rate_hz;
static unsigned int s_sound_pcm_stream_pos;
static unsigned int s_sound_pcm_stream_len;
static unsigned int s_sound_pcm_stream_hz;
static int s_sound_pcm_streaming;
static int s_sound_sb_probe_done;
static int s_sound_sb_present;
static int s_sound_sb_speaker_on;
static unsigned int s_sound_pcm_irq_count;
static unsigned int s_sound_pcm_timeout_count;
static unsigned int s_sound_pcm_error_count;
static unsigned int s_sound_pcm_last_count;
static unsigned int s_sound_pcm_last_hz;
static int s_sound_opl_probe_done;
static int s_sound_opl_present;

static unsigned int irq_save(void) {
    unsigned int flags;

    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(unsigned int flags) {
    __asm__ __volatile__("push %0; popf" :: "r"(flags) : "memory", "cc");
}

static void sound_speaker_off(void) {
    unsigned char speaker = inb(PC_SPEAKER_PORT);

    outb(PC_SPEAKER_PORT, (unsigned char)(speaker & ~0x03u));
}

static int sound_sb_write(unsigned char value) {
    for (unsigned int i = 0; i < SB_IO_RETRY; i++) {
        if ((inb(SB_WRITE_STATUS_PORT) & 0x80u) == 0u) {
            outb(SB_WRITE_PORT, value);
            return 1;
        }
    }
    return 0;
}

static int sound_sb_read(unsigned char* out) {
    for (unsigned int i = 0; i < SB_IO_RETRY; i++) {
        if (inb(SB_DATA_AVAIL_PORT) & 0x80u) {
            *out = inb(SB_READ_PORT);
            return 1;
        }
    }
    return 0;
}

static int sound_sb_probe(void) {
    unsigned char value;

    if (s_sound_sb_probe_done) {
        return s_sound_sb_present;
    }
    s_sound_sb_probe_done = 1;
    s_sound_sb_present = 0;

    outb(SB_RESET_PORT, 1u);
    for (unsigned int i = 0; i < 1000u; i++) {
        io_wait();
    }
    outb(SB_RESET_PORT, 0u);

    if (sound_sb_read(&value) && value == 0xaau) {
        s_sound_sb_present = 1;
        outb(SB_MIXER_ADDR_PORT, SB_MIXER_IRQ_SELECT);
        outb(SB_MIXER_DATA_PORT, SB_MIXER_IRQ5);
        outb(SB_MIXER_ADDR_PORT, SB_MIXER_DMA_SELECT);
        outb(SB_MIXER_DATA_PORT, SB_MIXER_DMA1_AND_5);
    }
    return s_sound_sb_present;
}

static void sound_opl_delay_addr(void) {
    for (unsigned int i = 0; i < 6u; i++) {
        (void)inb(OPL_STATUS_PORT);
    }
}

static void sound_opl_delay_data(void) {
    for (unsigned int i = 0; i < 35u; i++) {
        (void)inb(OPL_STATUS_PORT);
    }
}

static unsigned char sound_opl_read_status(void) {
    return inb(OPL_STATUS_PORT);
}

static void sound_opl_write_raw(unsigned int reg, unsigned int value) {
    outb(OPL_ADDR_PORT, (unsigned char)(reg & 0xffu));
    sound_opl_delay_addr();
    outb(OPL_DATA_PORT, (unsigned char)(value & 0xffu));
    sound_opl_delay_data();
}

static void sound_opl_reset_raw(void) {
    for (unsigned int reg = 1u; reg <= 0xf5u; reg++) {
        sound_opl_write_raw(reg, 0u);
    }
    sound_opl_write_raw(OPL_REG_WAVEFORM, 0x20u);
    sound_opl_write_raw(OPL_REG_CSM_SEL, 0u);
}

static int sound_opl_probe(void) {
    unsigned char status1;
    unsigned char status2;

    if (s_sound_opl_probe_done) {
        return s_sound_opl_present;
    }
    s_sound_opl_probe_done = 1;
    s_sound_opl_present = 0;

    sound_opl_write_raw(OPL_REG_TIMER_CTRL, 0x60u);
    sound_opl_write_raw(OPL_REG_TIMER_CTRL, 0x80u);
    status1 = sound_opl_read_status();
    sound_opl_write_raw(OPL_REG_TIMER1, 0xffu);
    sound_opl_write_raw(OPL_REG_TIMER_CTRL, 0x21u);
    for (unsigned int i = 0; i < 200u; i++) {
        (void)inb(OPL_STATUS_PORT);
    }
    status2 = sound_opl_read_status();
    sound_opl_write_raw(OPL_REG_TIMER_CTRL, 0x60u);
    sound_opl_write_raw(OPL_REG_TIMER_CTRL, 0x80u);

    if (((status1 & 0xe0u) == 0x00u) && ((status2 & 0xe0u) == 0xc0u)) {
        s_sound_opl_present = 1;
        sound_opl_reset_raw();
    }
    return s_sound_opl_present;
}

static void sound_sb_ack_irq(void) {
    (void)inb(SB_DATA_AVAIL_PORT);
    (void)inb(SB_BASE_PORT + 0x0fu);
}

static int sound_sb_set_rate_sb16(unsigned int sample_hz) {
    if (s_sound_pcm_rate_hz == sample_hz) {
        return 1;
    }

    if (!sound_sb_write(SB_CMD_SET_OUTPUT_RATE) ||
        !sound_sb_write((unsigned char)((sample_hz >> 8) & 0xffu)) ||
        !sound_sb_write((unsigned char)(sample_hz & 0xffu))) {
        return 0;
    }
    s_sound_pcm_rate_hz = sample_hz;
    return 1;
}

static int sound_sb_set_rate_legacy(unsigned int sample_hz) {
    unsigned char time_constant;

    if (s_sound_pcm_rate_hz == sample_hz) {
        return 1;
    }

    time_constant = (unsigned char)(256u - (1000000u / sample_hz));
    if (!sound_sb_write(SB_CMD_SET_TIME_CONSTANT) ||
        !sound_sb_write(time_constant)) {
        return 0;
    }
    s_sound_pcm_rate_hz = sample_hz;
    return 1;
}

static int sound_sb_speaker_on(void) {
    if (s_sound_sb_speaker_on) {
        return 1;
    }
    if (!sound_sb_write(SB_CMD_SPEAKER_ON)) {
        return 0;
    }
    s_sound_sb_speaker_on = 1;
    return 1;
}

static void sound_sb_stop(void) {
    if (!s_sound_sb_present) {
        return;
    }
    (void)sound_sb_write(SB_CMD_DMA_PAUSE);
    (void)sound_sb_write(SB_CMD_DMA16_PAUSE);
    (void)sound_sb_write(SB_CMD_SPEAKER_OFF);
    sound_sb_ack_irq();
    s_sound_sb_speaker_on = 0;
    s_sound_pcm_rate_hz = 0u;
}

static unsigned int sound_pcm_deadline_ticks(unsigned int count,
                                             unsigned int sample_hz) {
    unsigned int ticks;
    unsigned int slack;

    ticks = (count * timer_get_hz() + sample_hz - 1u) / sample_hz;
    if (ticks == 0u) {
        ticks = 1u;
    }
    slack = timer_get_hz() / 6u;
    if (slack < 4u) {
        slack = 4u;
    }
    return ticks + slack;
}

static int sound_pcm16_start_chunk(unsigned int offset,
                                   unsigned int count) {
    unsigned int phys = (unsigned int)(s_sound_pcm16_buf + offset * 2u);
    unsigned int word_addr = phys >> 1;
    unsigned int length = count - 1u;

    if (count == 0u || phys >= 0x1000000u ||
        ((phys & 0x1ffffu) + count * 2u) > 0x20000u) {
        return 0;
    }

    outb(SB_DMA16_MASK_PORT, (unsigned char)(SB_DMA16_INDEX | 0x04u));
    outb(SB_DMA16_FLIPFLOP_PORT, 0u);
    outb(SB_DMA16_MODE_PORT, SB_DMA_MODE_SINGLE_PLAY);
    outb(SB_DMA16_ADDR_PORT, (unsigned char)(word_addr & 0xffu));
    outb(SB_DMA16_ADDR_PORT, (unsigned char)((word_addr >> 8) & 0xffu));
    outb(SB_DMA16_PAGE_PORT, (unsigned char)((phys >> 16) & 0xffu));
    outb(SB_DMA16_COUNT_PORT, (unsigned char)(length & 0xffu));
    outb(SB_DMA16_COUNT_PORT,
         (unsigned char)((length >> 8) & 0xffu));
    outb(SB_DMA16_MASK_PORT, SB_DMA16_INDEX);

    if (!sound_sb_write(SB_CMD_SINGLE_CYCLE_16BIT_OUTPUT) ||
        !sound_sb_write(SB_MODE_SIGNED_MONO) ||
        !sound_sb_write((unsigned char)(length & 0xffu)) ||
        !sound_sb_write((unsigned char)((length >> 8) & 0xffu))) {
        return 0;
    }

    s_sound_pcm_deadline_tick =
        timer_get_ticks() + sound_pcm_deadline_ticks(count,
                                                     s_sound_pcm_stream_hz);
    s_sound_pcm_active = 1u;
    return 1;
}

static int sound_pcm16_start_next_chunk(void) {
    unsigned int remaining;
    unsigned int chunk;
    unsigned int offset;

    if (s_sound_pcm_stream_pos >= s_sound_pcm_stream_len) {
        s_sound_pcm_streaming = 0;
        s_sound_pcm_active = 0u;
        return 0;
    }

    remaining = s_sound_pcm_stream_len - s_sound_pcm_stream_pos;
    chunk = remaining;
    if (chunk > SB_PCM_STREAM_CHUNK_SAMPLES) {
        chunk = SB_PCM_STREAM_CHUNK_SAMPLES;
    }

    offset = s_sound_pcm_stream_pos;
    s_sound_pcm_stream_pos += chunk;
    if (!sound_pcm16_start_chunk(offset, chunk)) {
        s_sound_pcm_streaming = 0;
        s_sound_pcm_active = 0u;
        s_sound_pcm_error_count++;
        return 0;
    }
    return 1;
}

static void sound_program_divisor(unsigned int divisor) {
    unsigned char speaker;

    outb(PIT_MODE_PORT, 0xb6);
    outb(PIT_SPEAKER_PORT, (unsigned char)(divisor & 0xffu));
    outb(PIT_SPEAKER_PORT, (unsigned char)((divisor >> 8) & 0xffu));

    speaker = inb(PC_SPEAKER_PORT);
    if ((speaker & 0x03u) != 0x03u) {
        outb(PC_SPEAKER_PORT, (unsigned char)(speaker | 0x03u));
    }
}

static void sound_emit_sequence_sample(unsigned char sample) {
    unsigned int divisor;

    if (s_sound_seq_last_sample == sample) {
        return;
    }
    s_sound_seq_last_sample = sample;

    if (!sample) {
        sound_speaker_off();
        return;
    }

    divisor = (unsigned int)sample * s_sound_seq_divisor_scale;
    if (divisor == 0u) {
        divisor = 1u;
    }
    if (divisor > 0xffffu) {
        divisor = 0xffffu;
    }
    sound_program_divisor(divisor);
}

static unsigned int sound_duration_deadline(unsigned int duration_ms) {
    unsigned int ticks;

    if (duration_ms == 0u) {
        return 0u;
    }
    if (duration_ms > SYS_SOUND_MAX_DURATION_MS) {
        duration_ms = SYS_SOUND_MAX_DURATION_MS;
    }
    ticks = timer_ms_to_ticks_round_up(duration_ms);
    if (ticks == 0u) {
        ticks = 1u;
    }
    return timer_get_ticks() + ticks;
}

int sound_pit_divisor(unsigned int divisor, unsigned int duration_ms) {
    if (divisor == 0u || divisor > 0xffffu) {
        return -EINVAL;
    }

    s_sound_pcm_active = 0u;
    s_sound_seq_active = 0u;
    sound_program_divisor(divisor);
    s_sound_deadline_tick = sound_duration_deadline(duration_ms);
    s_sound_active = 1u;
    return 0;
}

int sound_tone(unsigned int frequency_hz, unsigned int duration_ms) {
    unsigned int divisor;

    if (frequency_hz < SYS_SOUND_MIN_HZ || frequency_hz > SYS_SOUND_MAX_HZ) {
        return -EINVAL;
    }

    divisor = PIT_BASE_HZ / frequency_hz;
    if (divisor == 0u) {
        divisor = 1u;
    }
    if (divisor > 0xffffu) {
        divisor = 0xffffu;
    }
    return sound_pit_divisor(divisor, duration_ms);
}

int sound_pit_sequence(const unsigned char* samples, unsigned int count,
                       unsigned int sample_hz, unsigned int divisor_scale) {
    unsigned int timer_hz = timer_get_hz();

    if (!samples || count == 0u || count > SYS_SOUND_SEQUENCE_MAX_SAMPLES) {
        return -EINVAL;
    }
    if (sample_hz == 0u || timer_hz == 0u || divisor_scale == 0u) {
        return -EINVAL;
    }

    for (unsigned int i = 0; i < count; i++) {
        s_sound_seq[i] = samples[i];
    }

    s_sound_pcm_active = 0u;
    s_sound_active = 0u;
    s_sound_deadline_tick = 0u;
    s_sound_seq_len = count;
    s_sound_seq_pos = 0u;
    s_sound_seq_accum = 0u;
    s_sound_seq_sample_hz = sample_hz;
    s_sound_seq_timer_hz = timer_hz;
    s_sound_seq_divisor_scale = divisor_scale;
    s_sound_seq_last_sample = 0x100u;
    s_sound_seq_active = 1u;
    sound_emit_sequence_sample(s_sound_seq[s_sound_seq_pos++]);
    return 0;
}

static int sound_pcm_u8_mode(const unsigned char* samples, unsigned int count,
                             unsigned int sample_hz, int mode) {
    unsigned int phys;
    unsigned int length;
    unsigned int bytes;
    unsigned int flags;
    int use_16bit = mode == SB_PCM_MODE_16BIT;

    if (!samples || count == 0u || count > SYS_SOUND_PCM_MAX_SAMPLES) {
        s_sound_pcm_error_count++;
        return -EINVAL;
    }
    if (sample_hz < SYS_SOUND_PCM_MIN_HZ || sample_hz > SYS_SOUND_PCM_MAX_HZ) {
        s_sound_pcm_error_count++;
        return -EINVAL;
    }
    if (!sound_sb_probe()) {
        s_sound_pcm_error_count++;
        return -EIO;
    }

    if (use_16bit) {
        phys = (unsigned int)s_sound_pcm16_buf;
        bytes = count * 2u;
        if (phys >= 0x1000000u ||
            ((phys & 0x1ffffu) + bytes) > 0x20000u) {
            s_sound_pcm_error_count++;
            return -EIO;
        }
        for (unsigned int i = 0; i < count; i++) {
            int value = ((int)samples[i] - 128) << 8;
            s_sound_pcm16_buf[i * 2u] = (unsigned char)(value & 0xff);
            s_sound_pcm16_buf[i * 2u + 1u] =
                (unsigned char)(((unsigned int)value >> 8) & 0xffu);
        }
    } else {
        phys = (unsigned int)s_sound_pcm_buf;
        bytes = count;
        if (phys >= 0x1000000u || ((phys & 0xffffu) + bytes) > 0x10000u) {
            s_sound_pcm_error_count++;
            return -EIO;
        }
        k_memcpy(s_sound_pcm_buf, samples, count);
    }

    if (count > 0x10000u) {
        s_sound_pcm_error_count++;
        return -EIO;
    }

    length = count - 1u;
    flags = irq_save();
    s_sound_active = 0u;
    s_sound_seq_active = 0u;
    if (s_sound_pcm_active) {
        (void)sound_sb_write(SB_CMD_DMA_PAUSE);
        (void)sound_sb_write(SB_CMD_DMA16_PAUSE);
    }
    s_sound_pcm_active = 0u;
    s_sound_pcm_streaming = 0;
    s_sound_pcm_stream_pos = 0u;
    s_sound_pcm_stream_len = 0u;
    s_sound_pcm_stream_hz = 0u;
    sound_speaker_off();
    sound_sb_ack_irq();
    if (!sound_sb_speaker_on() ||
        !(mode == SB_PCM_MODE_LEGACY_8BIT ?
          sound_sb_set_rate_legacy(sample_hz) :
          sound_sb_set_rate_sb16(sample_hz))) {
        s_sound_pcm_error_count++;
        irq_restore(flags);
        return -EIO;
    }

    if (use_16bit) {
        s_sound_pcm_stream_pos = 0u;
        s_sound_pcm_stream_len = count;
        s_sound_pcm_stream_hz = sample_hz;
        s_sound_pcm_streaming = 1;
        if (!sound_pcm16_start_next_chunk()) {
            s_sound_pcm_active = 0u;
            s_sound_pcm_streaming = 0;
            irq_restore(flags);
            return -EIO;
        }
    } else {
        outb(SB_DMA_MASK_PORT, (unsigned char)(SB_DMA_CHANNEL | 0x04u));
        outb(SB_DMA_FLIPFLOP_PORT, 0u);
        outb(SB_DMA_MODE_PORT, SB_DMA_MODE_SINGLE_PLAY);
        outb(SB_DMA_ADDR_PORT, (unsigned char)(phys & 0xffu));
        outb(SB_DMA_ADDR_PORT, (unsigned char)((phys >> 8) & 0xffu));
        outb(SB_DMA_PAGE_PORT, (unsigned char)((phys >> 16) & 0xffu));
        outb(SB_DMA_COUNT_PORT, (unsigned char)(length & 0xffu));
        outb(SB_DMA_COUNT_PORT, (unsigned char)((length >> 8) & 0xffu));
        outb(SB_DMA_MASK_PORT, SB_DMA_CHANNEL);
    }

    if (mode == SB_PCM_MODE_LEGACY_8BIT) {
        if (!sound_sb_write(SB_CMD_SINGLE_CYCLE_8BIT) ||
            !sound_sb_write((unsigned char)(length & 0xffu)) ||
            !sound_sb_write((unsigned char)((length >> 8) & 0xffu))) {
            s_sound_pcm_active = 0u;
            s_sound_pcm_error_count++;
            irq_restore(flags);
            return -EIO;
        }
    } else if (mode == SB_PCM_MODE_SB16_8BIT &&
               (!sound_sb_write(SB_CMD_SINGLE_CYCLE_8BIT_OUTPUT) ||
                !sound_sb_write(SB_MODE_UNSIGNED_MONO) ||
                !sound_sb_write((unsigned char)(length & 0xffu)) ||
                !sound_sb_write((unsigned char)((length >> 8) & 0xffu)))) {
        s_sound_pcm_active = 0u;
        s_sound_pcm_error_count++;
        irq_restore(flags);
        return -EIO;
    }

    if (!use_16bit) {
        s_sound_pcm_deadline_tick =
            timer_get_ticks() + sound_pcm_deadline_ticks(count, sample_hz);
        s_sound_pcm_active = 1u;
    }
    s_sound_pcm_last_count = count;
    s_sound_pcm_last_hz = sample_hz;
    irq_restore(flags);
    return 0;
}

int sound_pcm_u8(const unsigned char* samples, unsigned int count,
                 unsigned int sample_hz) {
    return sound_pcm_u8_mode(samples, count, sample_hz, SB_PCM_MODE_16BIT);
}

int sound_pcm_u8_legacy(const unsigned char* samples, unsigned int count,
                        unsigned int sample_hz) {
    return sound_pcm_u8_mode(samples, count, sample_hz,
                             SB_PCM_MODE_LEGACY_8BIT);
}

int sound_pcm_u8_sb16_8(const unsigned char* samples, unsigned int count,
                        unsigned int sample_hz) {
    return sound_pcm_u8_mode(samples, count, sample_hz,
                             SB_PCM_MODE_SB16_8BIT);
}

int sound_opl_write(unsigned int reg, unsigned int value) {
    if (reg > 0xffu || value > 0xffu) {
        return -EINVAL;
    }
    if (!sound_opl_probe()) {
        return -EIO;
    }
    sound_opl_write_raw(reg, value);
    return 0;
}

int sound_opl_reset(void) {
    if (!sound_opl_probe()) {
        return -EIO;
    }
    sound_opl_reset_raw();
    return 0;
}

unsigned int sound_caps(void) {
    unsigned int caps = SYS_SOUND_CAP_PC_SPEAKER;

    if (sound_sb_probe()) {
        caps |= SYS_SOUND_CAP_PCM_U8;
    }
    if (sound_opl_probe()) {
        caps |= SYS_SOUND_CAP_ADLIB;
    }
    return caps;
}

void sound_stop(void) {
    sound_speaker_off();
    if (s_sound_pcm_active || s_sound_sb_present) {
        sound_sb_stop();
    }
    s_sound_active = 0u;
    s_sound_deadline_tick = 0u;
    s_sound_seq_active = 0u;
    s_sound_pcm_active = 0u;
    s_sound_pcm_streaming = 0;
    s_sound_pcm_stream_pos = 0u;
    s_sound_pcm_stream_len = 0u;
    s_sound_pcm_stream_hz = 0u;
}

void sound_irq_handler(void) {
    if (s_sound_sb_present) {
        sound_sb_ack_irq();
    }
    s_sound_pcm_irq_count++;
    if (s_sound_pcm_streaming) {
        if (sound_pcm16_start_next_chunk()) {
            return;
        }
    }
    s_sound_pcm_active = 0u;
    s_sound_pcm_streaming = 0;
}

void sound_status(sys_sound_status_t* out) {
    if (!out) {
        return;
    }
    out->caps = sound_caps();
    out->pcm_active = s_sound_pcm_active;
    out->pcm_irq_count = s_sound_pcm_irq_count;
    out->pcm_timeout_count = s_sound_pcm_timeout_count;
    out->pcm_error_count = s_sound_pcm_error_count;
    out->pcm_last_count = s_sound_pcm_last_count;
    out->pcm_last_hz = s_sound_pcm_last_hz;
}

void sound_timer_tick(void) {
    if (s_sound_pcm_active &&
        (int)(timer_get_ticks() - s_sound_pcm_deadline_tick) >= 0) {
        s_sound_pcm_active = 0u;
        s_sound_pcm_streaming = 0;
        s_sound_pcm_timeout_count++;
        sound_sb_ack_irq();
    }

    if (s_sound_seq_active) {
        s_sound_seq_accum += s_sound_seq_sample_hz;
        while (s_sound_seq_active &&
               s_sound_seq_accum >= s_sound_seq_timer_hz) {
            if (s_sound_seq_pos >= s_sound_seq_len) {
                sound_stop();
                return;
            }
            s_sound_seq_accum -= s_sound_seq_timer_hz;
            sound_emit_sequence_sample(s_sound_seq[s_sound_seq_pos++]);
        }
        return;
    }

    if (!s_sound_active || s_sound_deadline_tick == 0u) {
        return;
    }
    if ((int)(timer_get_ticks() - s_sound_deadline_tick) >= 0) {
        sound_stop();
    }
}
