#include "drivers/bus/pci_hal.h"

/* The NaiLoong SoC has no PCI ECAM window.  Keep the HAL wired so common
 * PCI code links; config reads return all-ones (no device). */

void arch_pci_host_init(uintptr_t ecam_base) {
    (void)ecam_base;
}

uint32_t arch_pci_config_read32(int bus, int dev, int func, uint32_t reg) {
    (void)bus;
    (void)dev;
    (void)func;
    (void)reg;
    return 0xFFFFFFFFU;
}

void arch_pci_config_write32(int bus, int dev, int func, uint32_t reg, uint32_t val) {
    (void)bus;
    (void)dev;
    (void)func;
    (void)reg;
    (void)val;
}

uintptr_t arch_pci_bar_to_resource(uint64_t bar_addr) {
    return (uintptr_t)bar_addr;
}

#include "core/errno.h"
extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);
int arch_pci_mmio_window(uint64_t *base, uint64_t *size) {
    if (!base || !size) return -EINVAL;
    *base = 0; *size = 0;
    return -EOPNOTSUPP;
}
int arch_pci_bar_reachable(uint64_t base, uint64_t size) {
    if (!size || size > (uint64_t)(~(size_t)0)) return 0;
    return arch_device_map_mmio(base, (size_t)size, 0) != NULL;
}
