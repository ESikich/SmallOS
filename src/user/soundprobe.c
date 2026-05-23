#include "include/sound.h"
#include "stdint.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "time.h"
#include "unistd.h"

#define PROBE_LOW_HZ 7000u
#define PROBE_HIGH_HZ 22050u
#define PROBE_MS 700u
#define PROBE_TONE_HZ 440u
#define PROBE_MAX_COUNT ((PROBE_HIGH_HZ * PROBE_MS) / 1000u)

static unsigned char sample[PROBE_MAX_COUNT];
static const unsigned char sine64[64] = {
    128, 137, 146, 155, 163, 171, 179, 186,
    193, 199, 204, 209, 213, 216, 218, 220,
    220, 220, 218, 216, 213, 209, 204, 199,
    193, 186, 179, 171, 163, 155, 146, 137,
    128, 119, 110, 101,  93,  85,  77,  70,
     63,  57,  52,  47,  43,  40,  38,  36,
     36,  36,  38,  40,  43,  47,  52,  57,
     63,  70,  77,  85,  93, 101, 110, 119,
};

static void fill_sine(unsigned int sample_hz, unsigned int count) {
    uint32_t phase = 0;
    uint32_t step = (PROBE_TONE_HZ * 64u * 65536u) / sample_hz;

    for (unsigned int i = 0; i < count; i++) {
        sample[i] = sine64[(phase >> 16) & 63u];
        phase += step;
    }
}

static void fill_silence(unsigned int count) {
    for (unsigned int i = 0; i < count; i++) {
        sample[i] = 0x80u;
    }
}

static void print_status(const char* tag) {
    sys_sound_status_t status;
    int rc = sound_status(&status);

    if (rc < 0) {
        printf("%s: sound_status rc=%d\n", tag, rc);
        return;
    }

    printf("%s: caps=0x%x active=%u irq=%u timeout=%u err=%u last=%u@%u\n",
           tag, status.caps, status.pcm_active, status.pcm_irq_count,
           status.pcm_timeout_count, status.pcm_error_count,
           status.pcm_last_count, status.pcm_last_hz);
}

static int run_probe(const char* mode, unsigned int sample_hz, int path) {
    unsigned int count = (sample_hz * PROBE_MS) / 1000u;
    int rc;

    printf("soundprobe: %s %uHz tone at %uHz sample rate\n",
           mode, PROBE_TONE_HZ, sample_hz);
    print_status("before");

    fill_sine(sample_hz, count);
    if (path == 1) {
        rc = sound_pcm_u8_legacy(sample, count, sample_hz);
    } else if (path == 2) {
        rc = sound_pcm_u8_sb16_8(sample, count, sample_hz);
    } else {
        rc = sound_pcm_u8(sample, count, sample_hz);
    }
    printf("play rc=%d count=%u hz=%u\n", rc, count, sample_hz);

    usleep((PROBE_MS + 250u) * 1000u);
    print_status("after");
    sound_stop();
    usleep(250000u);
    return rc;
}

static int run_silence(unsigned int sample_hz) {
    unsigned int count = (sample_hz * PROBE_MS) / 1000u;
    int rc;

    puts("soundprobe: sb16 digital silence");
    print_status("before");

    fill_silence(count);
    rc = sound_pcm_u8(sample, count, sample_hz);
    printf("play rc=%d count=%u hz=%u\n", rc, count, sample_hz);

    usleep((PROBE_MS + 250u) * 1000u);
    print_status("after");
    sound_stop();
    usleep(250000u);
    return rc;
}

static int run_opl_probe(void) {
    int rc;

    puts("soundprobe: adlib opl2 440Hz tone");
    print_status("before");

    rc = sound_opl_reset();
    if (rc < 0) {
        printf("opl reset rc=%d\n", rc);
        return rc;
    }

    (void)sound_opl_write(0x20u, 0x21u);
    (void)sound_opl_write(0x23u, 0x01u);
    (void)sound_opl_write(0x40u, 0x10u);
    (void)sound_opl_write(0x43u, 0x00u);
    (void)sound_opl_write(0x60u, 0xf0u);
    (void)sound_opl_write(0x63u, 0xf0u);
    (void)sound_opl_write(0x80u, 0x77u);
    (void)sound_opl_write(0x83u, 0x77u);
    (void)sound_opl_write(0xc0u, 0x00u);
    (void)sound_opl_write(0xa0u, 0x57u);
    rc = sound_opl_write(0xb0u, 0x31u);
    printf("opl play rc=%d\n", rc);
    usleep(PROBE_MS * 1000u);
    (void)sound_opl_write(0xb0u, 0x00u);
    (void)sound_opl_reset();
    print_status("after");
    usleep(250000u);
    return rc;
}

void _start(int argc, char** argv) {
    int rc0;
    int rc1;
    int rc2;
    int rc3;
    int rc4;
    int rc5;

    (void)argc;
    (void)argv;

    rc0 = run_silence(PROBE_HIGH_HZ);
    rc1 = run_probe("sb16-16dma", PROBE_LOW_HZ, 0);
    rc2 = run_probe("sb16-16dma", PROBE_HIGH_HZ, 0);
    rc3 = run_probe("sb16-8dma", PROBE_LOW_HZ, 2);
    rc4 = run_probe("legacy-8dma", PROBE_LOW_HZ, 1);
    rc5 = run_opl_probe();
    exit((rc0 < 0 || rc1 < 0 || rc2 < 0 || rc3 < 0 || rc4 < 0 ||
          rc5 < 0) ? 1 : 0);
}
