#ifndef _DRV_LOOP_H
#define _DRV_LOOP_H

#include "drivers/block/block_dev.h"

void loop_init(void);

/* Block-device view of a bound loop device, for consumers that speak in
 * 512-byte sectors (swapon/mkswap).  Returns NULL while the device has no
 * backing file; the returned pointer is stable per index. */
block_dev_t *loop_block_device(int idx);

#endif
