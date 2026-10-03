#include "drivers/bus/pci_hal.h"
#include "platform.h"

/*
 * ECAM access for RISC-V boards.
 *
 * Not every RISC-V SoC has a PCIe root complex: the SophGo SG2000 and CV1800B
 * do not, and their board headers therefore leave PCIE_ECAM_BASE undefined.
 * The accessors still have to exist -- pci_bus.c and the driver-module export
 * table reference them unconditionally -- so on a board with no ECAM they
 * report "nothing is plugged in" rather than dereferencing a made-up window.
 * 0xffffffff is the architectural "no device" answer for a config read, which
 * is exactly what a real ECAM returns for an unimplemented function, so the
 * bus enumerator's own absent-device path handles this without a special case.
 */
#ifdef PCIE_ECAM_BASE

static uintptr_t pci_ecam_base = PCIE_ECAM_BASE;

void arch_pci_host_init(uintptr_t ecam_base) {
    pci_ecam_base = ecam_base;
}

uint32_t arch_pci_config_read32(int bus, int dev, int func, uint32_t reg) {
    uintptr_t addr = pci_ecam_base
        | ((uint32_t)bus << 20)
        | ((uint32_t)dev << 15)
        | ((uint32_t)func << 12)
        | (reg & 0xFFCU);
    return *(volatile uint32_t *)addr;
}

void arch_pci_config_write32(int bus, int dev, int func, uint32_t reg, uint32_t val) {
    uintptr_t addr = pci_ecam_base
        | ((uint32_t)bus << 20)
        | ((uint32_t)dev << 15)
        | ((uint32_t)func << 12)
        | (reg & 0xFFCU);
    *(volatile uint32_t *)addr = val;
}

uintptr_t arch_pci_bar_to_resource(uint64_t bar_addr) {
    return (uintptr_t)bar_addr + PAGE_OFFSET;
}

#else /* !PCIE_ECAM_BASE */

void arch_pci_host_init(uintptr_t ecam_base) {
    (void)ecam_base;
}

uint32_t arch_pci_config_read32(int bus, int dev, int func, uint32_t reg) {
    (void)bus; (void)dev; (void)func; (void)reg;
    return 0xffffffffU;
}

void arch_pci_config_write32(int bus, int dev, int func, uint32_t reg, uint32_t val) {
    (void)bus; (void)dev; (void)func; (void)reg; (void)val;
}

uintptr_t arch_pci_bar_to_resource(uint64_t bar_addr) {
    (void)bar_addr;
    return 0;
}

#endif /* PCIE_ECAM_BASE */
