#include "drivers/bus/pci_hal.h"
#include "platform.h"

static uintptr_t pci_ecam_base = (PCIE_ECAM_BASE & 0x0fffffffffffffffULL) | 0x8000000000000000ULL;
extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);

void arch_pci_host_init(uintptr_t ecam_base) {
    pci_ecam_base = (ecam_base & 0x0fffffffffffffffULL) | 0x8000000000000000ULL;
}

uint32_t arch_pci_config_read32(int bus, int dev, int func, uint32_t reg) {
    uintptr_t addr = pci_ecam_base
        | ((uint32_t)bus << 20)
        | ((uint32_t)dev << 15)
        | ((uint32_t)func << 12)
        | (reg & 0xFFC);
    return *(volatile uint32_t *)addr;
}

void arch_pci_config_write32(int bus, int dev, int func, uint32_t reg, uint32_t val) {
    uintptr_t addr = pci_ecam_base
        | ((uint32_t)bus << 20)
        | ((uint32_t)dev << 15)
        | ((uint32_t)func << 12)
        | (reg & 0xFFC);
    *(volatile uint32_t *)addr = val;
}

uintptr_t arch_pci_bar_to_resource(uint64_t bar_addr) {
    return (uintptr_t)arch_device_map_mmio(bar_addr, 1, 0);
}

#include "core/errno.h"
extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);
int arch_pci_mmio_window(uint64_t *base, uint64_t *size) {
    if (!base || !size) return -EINVAL;
#ifdef CONFIG_BOARD_QEMU_VIRT_LOONGARCH64
    *base = 0x40000000ULL; *size = 0x40000000ULL;
    return 0;
#else
    *base = 0; *size = 0;
    return -EOPNOTSUPP;
#endif
}
int arch_pci_bar_reachable(uint64_t base, uint64_t size) {
    if (!size || size > (uint64_t)(~(size_t)0)) return 0;
    return arch_device_map_mmio(base, (size_t)size, 0) != NULL;
}
