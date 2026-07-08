#ifndef RANDOM_H
#define RANDOM_H

#include "types.h"

void random_init_early(void);
void random_mix(const void* data, u32 len);
void random_mix_u32(u32 value);
void random_mix_seed_file(const char* path);
void random_get_bytes(void* out, u32 len);

#endif /* RANDOM_H */
