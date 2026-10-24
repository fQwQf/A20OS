#ifndef _DRIVERS_BLOCK_BLOCK_DEV_H
#define _DRIVERS_BLOCK_BLOCK_DEV_H

#include "core/types.h"

typedef struct block_write {
    uint64_t lba;
    const void *buf;
    size_t count;
} block_write_t;

typedef struct block_dev {
    int (*read_sector)(struct block_dev *dev, uint64_t lba, void *buf,
                       size_t count);
    int (*write_sector)(struct block_dev *dev, uint64_t lba, const void *buf,
                        size_t count);
    /* Optional: commit the device's volatile write cache to stable media.
     * NULL means "no durability guarantee" -- fsync() then only reaches the
     * device and a power cut can still lose the data.  Callers must not
     * assume fsync() implies a flush; that assumption is what loses
     * committed transactions.  Returns 0 or a negative errno. */
    int (*flush)(struct block_dev *dev);
    uint64_t capacity;
    uint32_t sector_size;
    void *priv;
    /* Optional: called when the last consumer drops this device. Lets a
     * provider refuse being rebound while a consumer is still attached. */
    void (*release)(struct block_dev *dev);
    /* Optional synchronous multi-vector write.  A return guarantees that all
     * submitted DMA has completed or been cancelled; buffers remain borrowed
     * until then.  A negative errno may follow partial writes. */
    int (*write_batch)(struct block_dev *dev, const block_write_t *writes,
                       size_t nr);
} block_dev_t;

#endif
