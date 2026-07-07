#include "crypt.h"
#include "stdint.h"
#include "stddef.h"
#include "string.h"

#define SMALLCRYPT_PREFIX "$smallos-sha256$"
#define SMALLCRYPT_PREFIX_LEN 16u
#define SMALLCRYPT_MAX_SALT 31u
#define SMALLCRYPT_HASH_HEX 64u

typedef struct sha256_ctx {
    uint32_t state[8];
    uint64_t bit_len;
    unsigned char data[64];
    unsigned int data_len;
} sha256_ctx_t;

static const uint32_t s_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static char s_out[SMALLCRYPT_PREFIX_LEN + SMALLCRYPT_MAX_SALT + 1u +
                  SMALLCRYPT_HASH_HEX + 1u];

static uint32_t rotr(uint32_t value, unsigned int bits) {
    return (value >> bits) | (value << (32u - bits));
}

static void sha256_transform(sha256_ctx_t* ctx, const unsigned char data[64]) {
    uint32_t m[64];
    uint32_t a, b, c, d, e, f, g, h;

    for (unsigned int i = 0; i < 16; i++) {
        m[i] = ((uint32_t)data[i * 4u] << 24) |
               ((uint32_t)data[i * 4u + 1u] << 16) |
               ((uint32_t)data[i * 4u + 2u] << 8) |
               (uint32_t)data[i * 4u + 3u];
    }
    for (unsigned int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(m[i - 15u], 7) ^ rotr(m[i - 15u], 18) ^ (m[i - 15u] >> 3);
        uint32_t s1 = rotr(m[i - 2u], 17) ^ rotr(m[i - 2u], 19) ^ (m[i - 2u] >> 10);
        m[i] = m[i - 16u] + s0 + m[i - 7u] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (unsigned int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + s_k[i] + m[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t* ctx) {
    ctx->data_len = 0;
    ctx->bit_len = 0;
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
}

static void sha256_update(sha256_ctx_t* ctx, const unsigned char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ctx->data[ctx->data_len++] = data[i];
        if (ctx->data_len == 64u) {
            sha256_transform(ctx, ctx->data);
            ctx->bit_len += 512u;
            ctx->data_len = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t* ctx, unsigned char hash[32]) {
    unsigned int i = ctx->data_len;

    ctx->data[i++] = 0x80u;
    if (i > 56u) {
        while (i < 64u) ctx->data[i++] = 0;
        sha256_transform(ctx, ctx->data);
        i = 0;
    }
    while (i < 56u) ctx->data[i++] = 0;

    ctx->bit_len += (uint64_t)ctx->data_len * 8u;
    for (unsigned int j = 0; j < 8; j++) {
        ctx->data[63u - j] = (unsigned char)(ctx->bit_len >> (j * 8u));
    }
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4u; i++) {
        for (unsigned int j = 0; j < 8u; j++) {
            hash[j * 4u + i] = (unsigned char)(ctx->state[j] >> (24u - i * 8u));
        }
    }
}

static int valid_salt_char(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
}

static int extract_salt(const char* salt, char* out, unsigned int out_size) {
    unsigned int n = 0;

    if (!salt || strncmp(salt, SMALLCRYPT_PREFIX, SMALLCRYPT_PREFIX_LEN) != 0) {
        return 0;
    }
    salt += SMALLCRYPT_PREFIX_LEN;
    while (salt[n] && salt[n] != '$') {
        if (n + 1u >= out_size || !valid_salt_char(salt[n])) return 0;
        out[n] = salt[n];
        n++;
    }
    if (n == 0 || salt[n] != '$') return 0;
    out[n] = '\0';
    return 1;
}

char* crypt(const char* key, const char* salt) {
    static const char hex[] = "0123456789abcdef";
    char salt_buf[SMALLCRYPT_MAX_SALT + 1u];
    unsigned char digest[32];
    sha256_ctx_t ctx;
    unsigned int pos = 0;

    if (!key || !extract_salt(salt, salt_buf, sizeof(salt_buf))) {
        s_out[0] = '*';
        s_out[1] = '\0';
        return s_out;
    }

    sha256_init(&ctx);
    sha256_update(&ctx, (const unsigned char*)salt_buf, strlen(salt_buf));
    sha256_update(&ctx, (const unsigned char*)":", 1);
    sha256_update(&ctx, (const unsigned char*)key, strlen(key));
    sha256_final(&ctx, digest);

    memcpy(s_out, SMALLCRYPT_PREFIX, SMALLCRYPT_PREFIX_LEN);
    pos = SMALLCRYPT_PREFIX_LEN;
    for (unsigned int i = 0; salt_buf[i]; i++) s_out[pos++] = salt_buf[i];
    s_out[pos++] = '$';
    for (unsigned int i = 0; i < sizeof(digest); i++) {
        s_out[pos++] = hex[digest[i] >> 4];
        s_out[pos++] = hex[digest[i] & 0x0Fu];
    }
    s_out[pos] = '\0';
    return s_out;
}
