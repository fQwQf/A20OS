#ifndef _STM32F103_SDCARD_H
#define _STM32F103_SDCARD_H

#include "core/types.h"
#include "drivers/block/block_dev.h"

typedef struct stm32_sdcard_info {
    int present;
    int high_capacity;
    int fat32;
    int bus_width;
    int write_protected;
    uint16_t rca;
    uint64_t sectors;
    uint32_t partition_lba;
    uint32_t fat_sectors;
    uint32_t cluster_count;
    char volume_label[12];
} stm32_sdcard_info_t;

int stm32_sdcard_init(void);
void stm32_sdcard_shutdown(void);
int stm32_sdcard_check(void);
int stm32_sdcard_read(uint64_t lba, void *buf, size_t count);
int stm32_sdcard_write(uint64_t lba, const void *buf, size_t count);
const stm32_sdcard_info_t *stm32_sdcard_info(void);
block_dev_t *stm32_sdcard_block_dev(void);

#endif
