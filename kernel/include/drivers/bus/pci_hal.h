#ifndef _DRIVERS_BUS_PCI_HAL_H
#define _DRIVERS_BUS_PCI_HAL_H

#include "core/types.h"

void arch_pci_host_init(uintptr_t ecam_base);
uint32_t arch_pci_config_read32(int bus, int dev, int func, uint32_t reg);
void arch_pci_config_write32(int bus, int dev, int func, uint32_t reg, uint32_t val);
uintptr_t arch_pci_bar_to_resource(uint64_t bar_addr);
/* CPU-physical allocation aperture; unavailable platforms return -EOPNOTSUPP. */
int arch_pci_mmio_window(uint64_t *base, uint64_t *size);
/* Pure mapping/policy check; never touches device registers or assigns a BAR. */
int arch_pci_bar_reachable(uint64_t base, uint64_t size);
int arch_pci_intx_irq(int bus, int dev, int func, int pin);

#endif
