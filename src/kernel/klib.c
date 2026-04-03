#include "klib.h"

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

void k_memcpy(void* dst, const void* src, k_size_t n) {
    unsigned char*       d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (k_size_t i = 0; i < n; i++) d[i] = s[i];
}

void k_memset(void* dst, unsigned char val, k_size_t n) {
    unsigned char* d = (unsigned char*)dst;
    for (k_size_t i = 0; i < n; i++) d[i] = val;
}

/* ------------------------------------------------------------------ */
/* String                                                              */
/* ------------------------------------------------------------------ */

int k_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

int k_strcmp(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

void k_strncpy(char* dst, const char* src, k_size_t n) {
    k_size_t i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

int k_starts_with(const char* s, const char* prefix) {
    int i = 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}