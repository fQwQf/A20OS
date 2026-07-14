#ifndef _DRIVERS_BLOCK_BLOCK_DEV_H
#define _DRIVERS_BLOCK_BLOCK_DEV_H

#include "core/types.h"

typedef struct block_dev {
    int (*read_sector)(struct block_dev *dev, uint64_t lba, void *buf,
                       size_t count);
    int (*write_sector)(struct block_dev *dev, uint64_t lba, const void *buf,
                        size_t count);
    uint64_t capacity;
    uint32_t sector_size;
    void *priv;
} block_dev_t;

#endif
