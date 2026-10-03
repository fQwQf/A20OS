#ifdef CONFIG_X86_64

#include "cpu.h"
#include "drivers/bus/pci_hal.h"
#include "platform.h"

#define PCI_CFG_ADDRESS_PORT 0x0CF8
#define PCI_CFG_DATA_PORT    0x0CFC

static uintptr_t pci_ecam_base = PCI_ECAM_BASE;
static int pci_legacy_only;

void arch_pci_host_init(uintptr_t ecam_base) {
    pci_ecam_base = ecam_base;
}

/* Machines predating PCIe have no ECAM window at all; CONFIG_ADDRESS reads
 * 0xffffffff and the 0xCF8/0xCFC indirection pair is the only way in.  Once a
 * vendor id reads back as all-ones there is no ECAM, so stop trying. */
static int ecam_absent(void) {
    if (pci_legacy_only)
        return 1;
    if (*(volatile uint32_t *)pci_ecam_base == 0xffffffffU) {
        pci_legacy_only = 1;
        return 1;
    }
    return 0;
}

static uint32_t pci_cfg_read32(int bus, int dev, int func, uint32_t reg) {
    if (!ecam_absent()) {
        uintptr_t addr = pci_ecam_base
            | ((uint32_t)bus << 20)
            | ((uint32_t)dev << 15)
            | ((uint32_t)func << 12)
            | (reg & 0xFFC);
        return *(volatile uint32_t *)addr;
    }
    uint32_t address = 0x80000000U
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)dev << 11)
                     | ((uint32_t)func << 8)
                     | (reg & 0xFC);
    outl(PCI_CFG_ADDRESS_PORT, address);
    return inl(PCI_CFG_DATA_PORT);
}

static void pci_cfg_write32(int bus, int dev, int func, uint32_t reg,
                            uint32_t val) {
    if (!ecam_absent()) {
        uintptr_t addr = pci_ecam_base
            | ((uint32_t)bus << 20)
            | ((uint32_t)dev << 15)
            | ((uint32_t)func << 12)
            | (reg & 0xFFC);
        *(volatile uint32_t *)addr = val;
        return;
    }
    uint32_t address = 0x80000000U
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)dev << 11)
                     | ((uint32_t)func << 8)
                     | (reg & 0xFC);
    outl(PCI_CFG_ADDRESS_PORT, address);
    outl(PCI_CFG_DATA_PORT, val);
}

uint32_t arch_pci_config_read32(int bus, int dev, int func, uint32_t reg) {
    return pci_cfg_read32(bus, dev, func, reg);
}

void arch_pci_config_write32(int bus, int dev, int func, uint32_t reg, uint32_t val) {
    pci_cfg_write32(bus, dev, func, reg, val);
}

uintptr_t arch_pci_bar_to_resource(uint64_t bar_addr) {
    return (uintptr_t)bar_addr + PAGE_OFFSET;
}

#endif
