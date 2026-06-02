/*
 * DW-SDIO (Synopsys DesignWare SDIO/MMC Host Controller) Driver
 *
 * Supports VisionFive2 and other boards with DW-SDIO.
 * Reference: rocketos os/src/drivers/block/sdio.rs
 */
#ifndef _DW_SDIO_H
#define _DW_SDIO_H

#include "core/types.h"

/* Public API for FS layer */
#define DW_SDIO_SECTOR_SIZE 512

int  dw_sdio_init_dev(uintptr_t base);
int  dw_sdio_read_sector(uintptr_t base, uint64_t lba, void *buf, size_t count);
int  dw_sdio_write_sector(uintptr_t base, uint64_t lba, const void *buf, size_t count);
uint64_t dw_sdio_capacity(uintptr_t base);
int  dw_sdio_card_ready(uintptr_t base);

#endif /* _DW_SDIO_H */
