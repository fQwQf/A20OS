#ifndef _DRIVERS_BUS_VIRTIO_MMIO_HAL_H
#define _DRIVERS_BUS_VIRTIO_MMIO_HAL_H

#include "core/types.h"

uintptr_t arch_virtio_mmio_slot_base(uintptr_t base, int slot);
uintptr_t arch_virtio_mmio_slot_size(void);

#endif
