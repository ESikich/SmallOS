#ifndef KLIB_H
#define KLIB_H

/*
 * klib.h — Freestanding kernel utility library
 *
 * Provides the basic string and memory primitives used across the kernel,
 * shell, and exec layers.  No libc dependency — all functions are defined
 * in klib.c and compiled as part of the kernel.
 *
 * Naming convention: k_ prefix to avoid collisions with any future libc
 * wrappers and to make call sites clearly identify their provenance.
 */

typedef unsigned int  k_size_t;

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

/*
 * k_memcpy(dst, src, n)
 *
 * Copy n bytes from src to dst.  Regions must not overlap.
 */
void k_memcpy(void* dst, const void* src, k_size_t n);

/*
 * k_memset(dst, val, n)
 *
 * Fill n bytes at dst with the byte value val.
 */
void k_memset(void* dst, unsigned char val, k_size_t n);

/* ------------------------------------------------------------------ */
/* String                                                              */
/* ------------------------------------------------------------------ */

/*
 * k_strlen(s)
 *
 * Return the number of characters in s before the null terminator.
 */
int k_strlen(const char* s);

/*
 * k_strcmp(a, b)
 *
 * Return 1 if a and b are identical (same length, same bytes), 0 otherwise.
 * This is an equality check — not a three-way comparison.
 */
int k_strcmp(const char* a, const char* b);

/*
 * k_strncpy(dst, src, n)
 *
 * Copy at most n-1 characters from src into dst and null-terminate.
 * dst is always null-terminated when n > 0.
 */
void k_strncpy(char* dst, const char* src, k_size_t n);

/*
 * k_starts_with(s, prefix)
 *
 * Return 1 if s begins with prefix, 0 otherwise.
 */
int k_starts_with(const char* s, const char* prefix);

#endif /* KLIB_H */