#include "block.h"

int block_read(block_device_t* dev, u32 lba, u32 count, void* buf) {
    if (!dev || !dev->read || !buf || dev->sector_size == 0u || count == 0u) {
        return 0;
    }
    if (dev->sector_count != 0u &&
        (lba >= dev->sector_count || count > dev->sector_count - lba)) {
        return 0;
    }
    return dev->read(dev, lba, count, buf);
}

int block_write(block_device_t* dev, u32 lba, u32 count, const void* buf) {
    if (!dev || !dev->write || !buf || dev->sector_size == 0u ||
        count == 0u || dev->read_only) {
        return 0;
    }
    if (dev->sector_count != 0u &&
        (lba >= dev->sector_count || count > dev->sector_count - lba)) {
        return 0;
    }
    return dev->write(dev, lba, count, buf);
}
