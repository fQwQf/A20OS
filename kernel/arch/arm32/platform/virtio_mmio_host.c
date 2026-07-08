#include "drivers/bus/virtio_mmio_hal.h"

uintptr_t arch_virtio_mmio_slot_base(uintptr_t base, int slot) {
    return base + (uintptr_t)slot * 0x200UL;
}

uintptr_t arch_virtio_mmio_slot_size(void) {
    return 0x200UL;
}

int arm32_virtio_mmio_host_link_anchor(void) {
    return 0;
}
