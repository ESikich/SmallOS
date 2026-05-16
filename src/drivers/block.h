#ifndef BLOCK_H
#define BLOCK_H

#include "../kernel/types.h"

typedef struct block_device block_device_t;

typedef int (*block_read_fn)(block_device_t* dev, u32 lba, u32 count, void* buf);
typedef int (*block_write_fn)(block_device_t* dev, u32 lba, u32 count, const void* buf);

struct block_device {
    const char* name;
    u32 sector_size;
    u32 sector_count;
    int read_only;
    void* ctx;
    block_read_fn read;
    block_write_fn write;
};

int block_read(block_device_t* dev, u32 lba, u32 count, void* buf);
int block_write(block_device_t* dev, u32 lba, u32 count, const void* buf);

#endif /* BLOCK_H */
