#include "drivers/block/virtio_blk.h"

int a20_block_dev_read(block_dev_t *dev, uint64_t lba, void *buf, size_t count)
{
    if (!dev || !dev->read_sector)
        return -1;
    return dev->read_sector(dev, lba, buf, count);
}

int a20_block_dev_write(block_dev_t *dev, uint64_t lba, const void *buf,
                        size_t count)
{
    if (!dev || !dev->write_sector)
        return -1;
    return dev->write_sector(dev, lba, buf, count);
}
