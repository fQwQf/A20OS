#ifndef _DRIVERS_BLOCK_AHCI_H
#define _DRIVERS_BLOCK_AHCI_H

#include "drivers/block/block_dev.h"

#define AHCI_PLATFORM_VENDOR 0x1C00U
#define AHCI_PLATFORM_DEVICE 4U

#define AHCI_PLATFORM_F_READ_ONLY (1U << 0)
#define AHCI_PLATFORM_F_PRESERVE_FIRMWARE_LINK (1U << 1)

typedef struct ahci_platform_data {
    uint32_t flags;
    uint32_t port_map;
} ahci_platform_data_t;

block_dev_t *ahci_get_dev(int idx);

#endif
