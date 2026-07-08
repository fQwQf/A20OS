#include "drivers/bus/pci_hal.h"

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
