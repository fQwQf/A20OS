#ifndef _DRIVERS_BLOCK_AHCI_H
#define _DRIVERS_BLOCK_AHCI_H

#include "drivers/block/block_dev.h"

block_dev_t *ahci_get_dev(int idx);

#endif
