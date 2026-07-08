#include "random.h"
#include "klib.h"
#include "timer.h"
#include "vfs.h"
#include "../drivers/nic.h"
#include "../drivers/net.h"

typedef struct random_state {
    u32 key[8];
    u32 counter;
    u32 nonce[3];
    unsigned int initialized;
} random_state_t;

static random_state_t s_rng;

static u32 random_rotl32(u32 x, unsigned int n) {
    return (x << n) | (x >> (32u - n));
}

static unsigned long long random_rdtsc(void) {
    u32 lo;
    u32 hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

static int random_cpu_has_rdrand(void) {
    u32 eax = 1u;
    u32 ebx;
    u32 ecx;
    u32 edx;

    __asm__ __volatile__("cpuid"
                         : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         :
                         : "cc");
    (void)ebx;
    (void)edx;
    return (ecx & (1u << 30)) != 0u;
}

static int random_rdrand32(u32* out) {
    unsigned char ok;
    u32 value;

    if (!out) return 0;
    __asm__ __volatile__(".byte 0x0f,0xc7,0xf0; setc %1"
                         : "=a"(value), "=qm"(ok)
                         :
                         : "cc");
    if (!ok) return 0;
    *out = value;
    return 1;
}

static void random_quarter(u32 x[16], int a, int b, int c, int d) {
    x[a] += x[b]; x[d] ^= x[a]; x[d] = random_rotl32(x[d], 16);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = random_rotl32(x[b], 12);
    x[a] += x[b]; x[d] ^= x[a]; x[d] = random_rotl32(x[d], 8);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = random_rotl32(x[b], 7);
}

static void random_chacha_block(u8 out[64]) {
    u32 state[16];
    u32 work[16];
    static const u32 constants[4] = {
        0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u
    };

    state[0] = constants[0];
    state[1] = constants[1];
    state[2] = constants[2];
    state[3] = constants[3];
    for (unsigned int i = 0; i < 8u; i++) state[4u + i] = s_rng.key[i];
    state[12] = s_rng.counter++;
    state[13] = s_rng.nonce[0];
    state[14] = s_rng.nonce[1];
    state[15] = s_rng.nonce[2];

    for (unsigned int i = 0; i < 16u; i++) work[i] = state[i];
    for (unsigned int i = 0; i < 10u; i++) {
        random_quarter(work, 0, 4, 8, 12);
        random_quarter(work, 1, 5, 9, 13);
        random_quarter(work, 2, 6, 10, 14);
        random_quarter(work, 3, 7, 11, 15);
        random_quarter(work, 0, 5, 10, 15);
        random_quarter(work, 1, 6, 11, 12);
        random_quarter(work, 2, 7, 8, 13);
        random_quarter(work, 3, 4, 9, 14);
    }
    for (unsigned int i = 0; i < 16u; i++) {
        u32 word = work[i] + state[i];
        out[i * 4u + 0u] = (u8)(word & 0xFFu);
        out[i * 4u + 1u] = (u8)((word >> 8) & 0xFFu);
        out[i * 4u + 2u] = (u8)((word >> 16) & 0xFFu);
        out[i * 4u + 3u] = (u8)((word >> 24) & 0xFFu);
    }
}

void random_mix(const void* data, u32 len) {
    const u8* bytes = (const u8*)data;
    u32 h = 0x811C9DC5u ^ (u32)random_rdtsc() ^ timer_get_ticks();

    if (!bytes && len != 0u) return;
    if (!s_rng.initialized) random_init_early();

    for (u32 i = 0; i < len; i++) {
        h ^= bytes[i];
        h *= 0x01000193u;
        h ^= h >> 16;
        s_rng.key[i & 7u] ^= h + 0x9E3779B9u + (s_rng.key[(i + 3u) & 7u] << 6);
    }
    s_rng.nonce[0] ^= h;
    s_rng.nonce[1] += len ^ (u32)random_rdtsc();
    s_rng.nonce[2] ^= timer_get_ticks() + 0xA5A5A5A5u;
}

void random_mix_u32(u32 value) {
    random_mix(&value, sizeof(value));
}

void random_init_early(void) {
    unsigned long long t = random_rdtsc();
    u32 rdrand_value;
    const u8* mac;
    const net_ipv4_config_t* cfg;

    if (s_rng.initialized) return;
    k_memset(&s_rng, 0, sizeof(s_rng));
    s_rng.key[0] = 0x243F6A88u ^ (u32)t;
    s_rng.key[1] = 0x85A308D3u ^ (u32)(t >> 32);
    s_rng.key[2] = 0x13198A2Eu ^ timer_get_ticks();
    s_rng.key[3] = 0x03707344u ^ (u32)(unsigned int)&s_rng;
    s_rng.key[4] = 0xA4093822u;
    s_rng.key[5] = 0x299F31D0u;
    s_rng.key[6] = 0x082EFA98u;
    s_rng.key[7] = 0xEC4E6C89u;
    s_rng.nonce[0] = (u32)t;
    s_rng.nonce[1] = (u32)(t >> 32);
    s_rng.nonce[2] = timer_get_ticks();
    s_rng.counter = 1u;
    s_rng.initialized = 1u;

    if (random_cpu_has_rdrand() && random_rdrand32(&rdrand_value)) {
        random_mix_u32(rdrand_value);
    }
    mac = nic_mac();
    if (mac) random_mix(mac, 6u);
    cfg = net_ipv4_config();
    if (cfg) random_mix(cfg, sizeof(*cfg));
}

void random_mix_seed_file(const char* path) {
    u32 size = 0;
    const u8* seed;

    if (!path) return;
    seed = vfs_load_file(path, &size);
    if (seed && size != 0u) random_mix(seed, size);
}

void random_get_bytes(void* out, u32 len) {
    u8* dst = (u8*)out;
    u8 block[64];

    if (!dst && len != 0u) return;
    if (!s_rng.initialized) random_init_early();
    random_mix_u32(timer_get_ticks());
    while (len != 0u) {
        u32 take = len > sizeof(block) ? sizeof(block) : len;
        random_chacha_block(block);
        k_memcpy(dst, block, take);
        random_mix(block, sizeof(block));
        dst += take;
        len -= take;
    }
    k_memset(block, 0, sizeof(block));
}
