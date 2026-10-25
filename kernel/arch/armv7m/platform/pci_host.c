#include "drivers/bus/pci_hal.h"

#include "core/errno.h"
extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);
int arch_pci_mmio_window(uint64_t *base, uint64_t *size) {
    if (!base || !size)
        return -EINVAL;
    *base = 0;
    *size = 0;
    return -EOPNOTSUPP;
}
int arch_pci_bar_reachable(uint64_t base, uint64_t size) {
    if (!size || size > (uint64_t)(~(size_t)0))
        return 0;
    return arch_device_map_mmio(base, (size_t)size, 0) != NULL;
}
