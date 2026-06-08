#ifdef CONFIG_X86_64

#include "drivers/bus/pci_hal.h"
#include "platform.h"

static uintptr_t pci_ecam_base = PCI_ECAM_BASE;

void arch_pci_host_init(uintptr_t ecam_base) {
    pci_ecam_base = ecam_base;
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
    return (uintptr_t)bar_addr + PAGE_OFFSET;
}

#endif
