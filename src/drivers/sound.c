#include "sound.h"
#include "pci.h"
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
#define OPL_REG_CHAR 0x20u
#define OPL_REG_SCALE 0x40u
#define OPL_REG_ATTACK 0x60u
#define OPL_REG_SUS 0x80u
#define OPL_REG_FREQ_L 0xa0u
#define OPL_REG_FREQ_H 0xb0u
#define OPL_REG_FEED_CON 0xc0u
#define OPL_REG_WAVE 0xe0u

#define SB_PCM_MODE_16BIT 0
#define SB_PCM_MODE_LEGACY_8BIT 1
#define SB_PCM_MODE_SB16_8BIT 2
#define SB_PCM_STREAM_CHUNK_SAMPLES 384u
#define SOUND_PCM_BACKEND_NONE 0u
#define SOUND_PCM_BACKEND_SB16 1u
#define SOUND_PCM_BACKEND_AC97 2u
#define AC97_VENDOR_INTEL 0x8086u
#define AC97_DEVICE_ICH 0x2415u
#define AC97_PCI_COMMAND_IO 0x0001u
#define AC97_PCI_COMMAND_BUS_MASTER 0x0004u
#define AC97_BAR_IO_MASK 0xfffffffcu
#define AC97_MIX_MASTER_VOLUME 0x02u
#define AC97_MIX_PCM_OUT_VOLUME 0x18u
#define AC97_MIX_POWERDOWN 0x26u
#define AC97_MIX_EXT_AUDIO_CTRL 0x2au
#define AC97_MIX_PCM_FRONT_DAC_RATE 0x2cu
#define AC97_EACS_VRA 0x0001u
#define AC97_PO_BDBAR 0x10u
#define AC97_PO_LVI 0x15u
#define AC97_PO_SR 0x16u
#define AC97_PO_CR 0x1bu
#define AC97_SR_FIFOE 0x10u
#define AC97_SR_BCIS 0x08u
#define AC97_SR_LVBCI 0x04u
#define AC97_CR_RR 0x02u
#define AC97_CR_RPBM 0x01u
#define AC97_BD_COUNT 32u
#define AC97_BD_MAX_WORDS 0x8000u

typedef struct {
    unsigned int addr;
    unsigned int ctl_len;
} ac97_buffer_descriptor_t;

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
static unsigned char s_sound_ac97_pcm_buf[SYS_SOUND_PCM_MAX_SAMPLES * 4u]
    __attribute__((aligned(4096)));
static ac97_buffer_descriptor_t s_sound_ac97_bd[AC97_BD_COUNT]
    __attribute__((aligned(256)));
static volatile unsigned int s_sound_pcm_active;
static unsigned int s_sound_pcm_deadline_tick;
static unsigned int s_sound_pcm_rate_hz;
static unsigned int s_sound_pcm_stream_pos;
static unsigned int s_sound_pcm_stream_len;
static unsigned int s_sound_pcm_stream_hz;
static int s_sound_pcm_streaming;
static unsigned int s_sound_pcm_backend;
static int s_sound_sb_probe_done;
static int s_sound_sb_present;
static int s_sound_sb_speaker_on;
static int s_sound_ac97_probe_done;
static int s_sound_ac97_present;
static unsigned int s_sound_ac97_nam;
static unsigned int s_sound_ac97_nabm;
static unsigned int s_sound_pcm_irq_count;
static unsigned int s_sound_pcm_timeout_count;
static unsigned int s_sound_pcm_error_count;
static unsigned int s_sound_pcm_last_count;
static unsigned int s_sound_pcm_last_hz;
static int s_sound_opl_probe_done;
static int s_sound_opl_present;
static const unsigned char s_sound_opl_carriers[9] =
    { 3, 4, 5, 11, 12, 13, 19, 20, 21 };
static const unsigned char s_sound_opl_modifiers[9] =
    { 0, 1, 2, 8, 9, 10, 16, 17, 18 };
static sys_sound_opl_event_t s_sound_opl_seq[SYS_SOUND_OPL_SEQUENCE_MAX_EVENTS];
static volatile unsigned int s_sound_opl_seq_active;
static unsigned int s_sound_opl_seq_len;
static unsigned int s_sound_opl_seq_pos;
static unsigned int s_sound_opl_seq_accum;
static unsigned int s_sound_opl_seq_timer_hz;
static unsigned int s_sound_opl_seq_delay;
static unsigned int s_sound_opl_seq_flags;
static unsigned char s_sound_opl_effect_samples[SYS_SOUND_OPL_EFFECT_MAX_SAMPLES];
static volatile unsigned int s_sound_opl_effect_active;
static unsigned int s_sound_opl_effect_len;
static unsigned int s_sound_opl_effect_pos;
static unsigned int s_sound_opl_effect_accum;
static unsigned int s_sound_opl_effect_sample_hz;
static unsigned int s_sound_opl_effect_timer_hz;
static unsigned int s_sound_opl_effect_channel;
static unsigned int s_sound_opl_effect_block;

static unsigned int irq_save(void) {
    unsigned int flags;

    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(unsigned int flags) {
    __asm__ __volatile__("push %0; popf" :: "r"(flags) : "memory", "cc");
}

static void sound_dma_fence(void) {
    __asm__ __volatile__("" ::: "memory");
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

static void sound_opl_sequence_clear(void) {
    s_sound_opl_seq_active = 0u;
    s_sound_opl_seq_len = 0u;
    s_sound_opl_seq_pos = 0u;
    s_sound_opl_seq_accum = 0u;
    s_sound_opl_seq_timer_hz = 0u;
    s_sound_opl_seq_delay = 0u;
    s_sound_opl_seq_flags = 0u;
}

static void sound_opl_effect_clear(void) {
    s_sound_opl_effect_active = 0u;
    s_sound_opl_effect_len = 0u;
    s_sound_opl_effect_pos = 0u;
    s_sound_opl_effect_accum = 0u;
    s_sound_opl_effect_sample_hz = 0u;
    s_sound_opl_effect_timer_hz = 0u;
    s_sound_opl_effect_channel = 0u;
    s_sound_opl_effect_block = 0u;
}

static void sound_opl_effect_key_off(void) {
    if (s_sound_opl_effect_channel < 9u) {
        sound_opl_write_raw(OPL_REG_FREQ_H + s_sound_opl_effect_channel, 0u);
    }
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

static unsigned int sound_ac97_io_bar(const pci_device_t* dev,
                                      unsigned char offset) {
    unsigned int bar = pci_read_config_dword(dev->bus, dev->slot, dev->func,
                                             offset);

    if ((bar & 1u) == 0u) {
        return 0u;
    }
    return bar & AC97_BAR_IO_MASK;
}

static void sound_ac97_stop(void) {
    if (!s_sound_ac97_present) {
        return;
    }

    outb((unsigned short)(s_sound_ac97_nabm + AC97_PO_CR), 0u);
    outb((unsigned short)(s_sound_ac97_nabm + AC97_PO_CR), AC97_CR_RR);
    outb((unsigned short)(s_sound_ac97_nabm + AC97_PO_CR), 0u);
    outw((unsigned short)(s_sound_ac97_nabm + AC97_PO_SR),
         AC97_SR_FIFOE | AC97_SR_BCIS | AC97_SR_LVBCI);
}

static int sound_ac97_probe(void) {
    pci_device_t dev;
    unsigned short command;

    if (s_sound_ac97_probe_done) {
        return s_sound_ac97_present;
    }
    s_sound_ac97_probe_done = 1;
    s_sound_ac97_present = 0;

    if (!pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_ICH, &dev)) {
        return 0;
    }

    s_sound_ac97_nam = sound_ac97_io_bar(&dev, 0x10u);
    s_sound_ac97_nabm = sound_ac97_io_bar(&dev, 0x14u);
    if (!s_sound_ac97_nam || !s_sound_ac97_nabm) {
        return 0;
    }

    command = pci_read_config_word(dev.bus, dev.slot, dev.func, 0x04u);
    command |= (unsigned short)(AC97_PCI_COMMAND_IO |
                                AC97_PCI_COMMAND_BUS_MASTER);
    pci_write_config_word(dev.bus, dev.slot, dev.func, 0x04u, command);

    outw((unsigned short)(s_sound_ac97_nam + AC97_MIX_MASTER_VOLUME), 0x0000u);
    outw((unsigned short)(s_sound_ac97_nam + AC97_MIX_PCM_OUT_VOLUME), 0x0000u);
    outw((unsigned short)(s_sound_ac97_nam + AC97_MIX_POWERDOWN), 0x0000u);
    outw((unsigned short)(s_sound_ac97_nam + AC97_MIX_EXT_AUDIO_CTRL),
         (unsigned short)(inw((unsigned short)(s_sound_ac97_nam +
                                               AC97_MIX_EXT_AUDIO_CTRL)) |
                          AC97_EACS_VRA));
    s_sound_ac97_present = 1;
    sound_ac97_stop();
    return 1;
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

static void sound_ac97_prepare_pcm(const unsigned char* samples,
                                   unsigned int count) {
    for (unsigned int i = 0; i < count; i++) {
        int value = ((int)samples[i] - 128) << 8;
        unsigned char lo = (unsigned char)(value & 0xff);
        unsigned char hi = (unsigned char)(((unsigned int)value >> 8) & 0xffu);
        unsigned int offset = i * 4u;

        s_sound_ac97_pcm_buf[offset] = lo;
        s_sound_ac97_pcm_buf[offset + 1u] = hi;
        s_sound_ac97_pcm_buf[offset + 2u] = lo;
        s_sound_ac97_pcm_buf[offset + 3u] = hi;
    }
}

static int sound_ac97_pcm_u8(const unsigned char* samples, unsigned int count,
                             unsigned int sample_hz, int* attempted) {
    unsigned int remaining_words;
    unsigned int byte_offset = 0u;
    unsigned int bd_count = 0u;
    unsigned int flags;

    if (attempted) {
        *attempted = 0;
    }
    if (!sound_ac97_probe()) {
        return 0;
    }
    if (attempted) {
        *attempted = 1;
    }

    if (!samples || count == 0u || count > SYS_SOUND_PCM_MAX_SAMPLES) {
        s_sound_pcm_error_count++;
        return -EINVAL;
    }
    if (sample_hz < SYS_SOUND_PCM_MIN_HZ || sample_hz > SYS_SOUND_PCM_MAX_HZ) {
        s_sound_pcm_error_count++;
        return -EINVAL;
    }

    flags = irq_save();
    s_sound_active = 0u;
    s_sound_seq_active = 0u;
    if (s_sound_pcm_backend == SOUND_PCM_BACKEND_SB16 && s_sound_pcm_active) {
        sound_sb_stop();
    }
    if (s_sound_pcm_backend == SOUND_PCM_BACKEND_AC97 && s_sound_pcm_active) {
        sound_ac97_stop();
    }
    s_sound_pcm_active = 0u;
    s_sound_pcm_streaming = 0;
    s_sound_pcm_stream_pos = 0u;
    s_sound_pcm_stream_len = 0u;
    s_sound_pcm_stream_hz = 0u;
    s_sound_pcm_backend = SOUND_PCM_BACKEND_NONE;
    irq_restore(flags);

    sound_ac97_prepare_pcm(samples, count);
    remaining_words = count * 2u;
    while (remaining_words && bd_count < AC97_BD_COUNT) {
        unsigned int words = remaining_words;

        if (words > AC97_BD_MAX_WORDS) {
            words = AC97_BD_MAX_WORDS;
        }
        s_sound_ac97_bd[bd_count].addr =
            (unsigned int)(s_sound_ac97_pcm_buf + byte_offset);
        s_sound_ac97_bd[bd_count].ctl_len = words;
        remaining_words -= words;
        byte_offset += words * 2u;
        bd_count++;
    }
    if (remaining_words || bd_count == 0u) {
        s_sound_pcm_error_count++;
        return -EIO;
    }
    sound_dma_fence();

    flags = irq_save();
    sound_speaker_off();
    sound_ac97_stop();
    outw((unsigned short)(s_sound_ac97_nam + AC97_MIX_MASTER_VOLUME), 0x0000u);
    outw((unsigned short)(s_sound_ac97_nam + AC97_MIX_PCM_OUT_VOLUME), 0x0000u);
    outw((unsigned short)(s_sound_ac97_nam + AC97_MIX_EXT_AUDIO_CTRL),
         (unsigned short)(inw((unsigned short)(s_sound_ac97_nam +
                                               AC97_MIX_EXT_AUDIO_CTRL)) |
                          AC97_EACS_VRA));
    outw((unsigned short)(s_sound_ac97_nam + AC97_MIX_PCM_FRONT_DAC_RATE),
         (unsigned short)sample_hz);
    outl((unsigned short)(s_sound_ac97_nabm + AC97_PO_BDBAR),
         (unsigned int)s_sound_ac97_bd);
    outb((unsigned short)(s_sound_ac97_nabm + AC97_PO_LVI),
         (unsigned char)(bd_count - 1u));
    outw((unsigned short)(s_sound_ac97_nabm + AC97_PO_SR),
         AC97_SR_FIFOE | AC97_SR_BCIS | AC97_SR_LVBCI);
    outb((unsigned short)(s_sound_ac97_nabm + AC97_PO_CR), AC97_CR_RPBM);
    s_sound_pcm_deadline_tick =
        timer_get_ticks() + sound_pcm_deadline_ticks(count, sample_hz);
    s_sound_pcm_backend = SOUND_PCM_BACKEND_AC97;
    s_sound_pcm_active = 1u;
    s_sound_pcm_last_count = count;
    s_sound_pcm_last_hz = sample_hz;
    irq_restore(flags);
    return 0;
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
    s_sound_pcm_backend = SOUND_PCM_BACKEND_SB16;
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
    if (s_sound_pcm_backend == SOUND_PCM_BACKEND_AC97) {
        sound_ac97_stop();
    }
    s_sound_pcm_backend = SOUND_PCM_BACKEND_SB16;
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
        s_sound_pcm_backend = SOUND_PCM_BACKEND_SB16;
        s_sound_pcm_active = 1u;
    }
    s_sound_pcm_last_count = count;
    s_sound_pcm_last_hz = sample_hz;
    irq_restore(flags);
    return 0;
}

int sound_pcm_u8(const unsigned char* samples, unsigned int count,
                 unsigned int sample_hz) {
    int attempted = 0;
    int rc = sound_ac97_pcm_u8(samples, count, sample_hz, &attempted);

    if (attempted) {
        return rc;
    }
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
    unsigned int flags;

    if (reg > 0xffu || value > 0xffu) {
        return -EINVAL;
    }
    if (!sound_opl_probe()) {
        return -EIO;
    }
    flags = irq_save();
    sound_opl_write_raw(reg, value);
    irq_restore(flags);
    return 0;
}

static void sound_opl_sequence_emit_budget(unsigned int budget) {
    unsigned int timer_hz = timer_get_hz();

    if (!s_sound_opl_seq_active || timer_hz == 0u) {
        return;
    }

    while (s_sound_opl_seq_active && budget--) {
        if (s_sound_opl_seq_delay) {
            unsigned int needed;

            if (s_sound_opl_seq_delay > 0xFFFFFFFFu / timer_hz) {
                sound_opl_sequence_clear();
                return;
            }
            needed = s_sound_opl_seq_delay * timer_hz;
            if (s_sound_opl_seq_accum < needed) {
                return;
            }
            s_sound_opl_seq_accum -= needed;
            s_sound_opl_seq_delay = 0u;
        }

        if (s_sound_opl_seq_pos >= s_sound_opl_seq_len) {
            if (s_sound_opl_seq_flags & SYS_SOUND_OPL_SEQUENCE_FLAG_LOOP) {
                s_sound_opl_seq_pos = 0u;
            } else {
                sound_opl_sequence_clear();
                return;
            }
        }

        {
            sys_sound_opl_event_t ev = s_sound_opl_seq[s_sound_opl_seq_pos++];
            sound_opl_write_raw(ev.reg_value & 0xffu, ev.reg_value >> 8);
            s_sound_opl_seq_delay = ev.delay;
        }
    }
}

int sound_opl_sequence(const sys_sound_opl_event_t* events,
                       unsigned int count,
                       unsigned int timer_hz,
                       unsigned int flags) {
    unsigned int saved_flags;

    if (!events || count == 0u ||
        count > SYS_SOUND_OPL_SEQUENCE_MAX_EVENTS ||
        timer_hz == 0u ||
        (flags & ~SYS_SOUND_OPL_SEQUENCE_FLAG_LOOP) != 0u) {
        return -EINVAL;
    }
    if (!sound_opl_probe()) {
        return -EIO;
    }

    saved_flags = irq_save();
    sound_opl_sequence_clear();
    for (unsigned int i = 0; i < count; i++) {
        s_sound_opl_seq[i] = events[i];
    }
    s_sound_opl_seq_len = count;
    s_sound_opl_seq_timer_hz = timer_hz;
    s_sound_opl_seq_flags = flags;
    s_sound_opl_seq_accum = 0u;
    s_sound_opl_seq_delay = 0u;
    s_sound_opl_seq_pos = 0u;
    s_sound_opl_seq_active = 1u;
    sound_opl_sequence_emit_budget(64u);
    irq_restore(saved_flags);
    return 0;
}

void sound_opl_sequence_stop(void) {
    unsigned int flags = irq_save();
    sound_opl_sequence_clear();
    irq_restore(flags);
}

static void sound_opl_effect_emit_sample(unsigned char sample) {
    if (s_sound_opl_effect_channel >= 9u) {
        sound_opl_effect_clear();
        return;
    }
    if (!sample) {
        sound_opl_write_raw(OPL_REG_FREQ_H + s_sound_opl_effect_channel, 0u);
        return;
    }
    sound_opl_write_raw(OPL_REG_FREQ_L + s_sound_opl_effect_channel, sample);
    sound_opl_write_raw(OPL_REG_FREQ_H + s_sound_opl_effect_channel,
                        s_sound_opl_effect_block);
}

static void sound_opl_effect_emit_budget(unsigned int budget) {
    unsigned int timer_hz = timer_get_hz();

    if (!s_sound_opl_effect_active || timer_hz == 0u) {
        return;
    }

    while (s_sound_opl_effect_active &&
           s_sound_opl_effect_accum >= s_sound_opl_effect_timer_hz &&
           budget--) {
        s_sound_opl_effect_accum -= s_sound_opl_effect_timer_hz;
        if (s_sound_opl_effect_pos >= s_sound_opl_effect_len) {
            sound_opl_effect_key_off();
            sound_opl_effect_clear();
            return;
        }
        sound_opl_effect_emit_sample(
            s_sound_opl_effect_samples[s_sound_opl_effect_pos++]);
    }
}

static void sound_opl_effect_program_instrument(
    const sys_sound_opl_effect_t* effect) {
    unsigned int channel = effect->channel;
    unsigned int m = s_sound_opl_modifiers[channel];
    unsigned int c = s_sound_opl_carriers[channel];

    sound_opl_write_raw(m + OPL_REG_CHAR, effect->m_char);
    sound_opl_write_raw(m + OPL_REG_SCALE, effect->m_scale);
    sound_opl_write_raw(m + OPL_REG_ATTACK, effect->m_attack);
    sound_opl_write_raw(m + OPL_REG_SUS, effect->m_sus);
    sound_opl_write_raw(m + OPL_REG_WAVE, effect->m_wave);
    sound_opl_write_raw(c + OPL_REG_CHAR, effect->c_char);
    sound_opl_write_raw(c + OPL_REG_SCALE, effect->c_scale);
    sound_opl_write_raw(c + OPL_REG_ATTACK, effect->c_attack);
    sound_opl_write_raw(c + OPL_REG_SUS, effect->c_sus);
    sound_opl_write_raw(c + OPL_REG_WAVE, effect->c_wave);
    sound_opl_write_raw(channel + OPL_REG_FEED_CON, effect->feedback);
}

int sound_opl_effect(const sys_sound_opl_effect_t* effect) {
    unsigned int saved_flags;
    unsigned int timer_hz = timer_get_hz();

    if (!effect ||
        !effect->samples ||
        effect->count == 0u ||
        effect->count > SYS_SOUND_OPL_EFFECT_MAX_SAMPLES ||
        effect->sample_hz == 0u ||
        effect->sample_hz > SYS_SOUND_MAX_HZ ||
        effect->channel >= 9u ||
        timer_hz == 0u) {
        return -EINVAL;
    }
    if (!sound_opl_probe()) {
        return -EIO;
    }

    saved_flags = irq_save();
    if (s_sound_opl_effect_active) {
        sound_opl_effect_key_off();
    }
    sound_opl_effect_clear();
    irq_restore(saved_flags);

    for (unsigned int i = 0; i < effect->count; i++) {
        s_sound_opl_effect_samples[i] = effect->samples[i];
    }

    saved_flags = irq_save();
    sound_opl_effect_program_instrument(effect);
    s_sound_opl_effect_len = effect->count;
    s_sound_opl_effect_pos = 0u;
    s_sound_opl_effect_accum = 0u;
    s_sound_opl_effect_sample_hz = effect->sample_hz;
    s_sound_opl_effect_timer_hz = timer_hz;
    s_sound_opl_effect_channel = effect->channel;
    s_sound_opl_effect_block = effect->block;
    s_sound_opl_effect_active = 1u;
    sound_opl_effect_emit_sample(
        s_sound_opl_effect_samples[s_sound_opl_effect_pos++]);
    irq_restore(saved_flags);
    return 0;
}

void sound_opl_effect_stop(void) {
    unsigned int flags = irq_save();
    if (s_sound_opl_effect_active) {
        sound_opl_effect_key_off();
    }
    sound_opl_effect_clear();
    irq_restore(flags);
}

int sound_opl_reset(void) {
    if (!sound_opl_probe()) {
        return -EIO;
    }
    sound_opl_sequence_stop();
    sound_opl_effect_stop();
    sound_opl_reset_raw();
    return 0;
}

unsigned int sound_caps(void) {
    unsigned int caps = SYS_SOUND_CAP_PC_SPEAKER;

    if (sound_ac97_probe() || sound_sb_probe()) {
        caps |= SYS_SOUND_CAP_PCM_U8;
    }
    if (sound_opl_probe()) {
        caps |= SYS_SOUND_CAP_ADLIB;
    }
    return caps;
}

void sound_stop(void) {
    sound_speaker_off();
    if (s_sound_ac97_present) {
        sound_ac97_stop();
    }
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
    s_sound_pcm_backend = SOUND_PCM_BACKEND_NONE;
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
    s_sound_pcm_backend = SOUND_PCM_BACKEND_NONE;
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
    out->opl_sequence_active = s_sound_opl_seq_active;
    out->opl_effect_active = s_sound_opl_effect_active;
}

void sound_timer_tick(void) {
    if (s_sound_opl_seq_active) {
        s_sound_opl_seq_accum += s_sound_opl_seq_timer_hz;
        sound_opl_sequence_emit_budget(512u);
    }
    if (s_sound_opl_effect_active) {
        s_sound_opl_effect_accum += s_sound_opl_effect_sample_hz;
        sound_opl_effect_emit_budget(512u);
    }

    if (s_sound_pcm_active &&
        (int)(timer_get_ticks() - s_sound_pcm_deadline_tick) >= 0) {
        if (s_sound_pcm_backend == SOUND_PCM_BACKEND_AC97) {
            sound_ac97_stop();
        } else {
            sound_sb_ack_irq();
            s_sound_pcm_timeout_count++;
        }
        s_sound_pcm_active = 0u;
        s_sound_pcm_streaming = 0;
        s_sound_pcm_backend = SOUND_PCM_BACKEND_NONE;
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
