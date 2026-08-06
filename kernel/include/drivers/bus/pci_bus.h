#ifndef _DRIVERS_BUS_PCI_BUS_H
#define _DRIVERS_BUS_PCI_BUS_H

#include "drivers/core/driver_core.h"

extern bus_type_t pci_bus;

bus_type_t *get_pci_bus(void);
int pci_enable_and_assign_bars(device_t *dev);
/* Packed class/subclass/prog-if (class << 16 | subclass << 8 | prog-if). */
uint32_t pci_class_code(const device_t *dev);
/* PCI vendor/device as vendor << 16 | device. */
uint32_t pci_device_id(const device_t *dev);
/* Return the resource corresponding to a physical PCI BAR number.  MMIO
 * resources are compacted in device->res, so display drivers must not assume
 * that BAR2 is the second resource when BAR0 is a 64-bit BAR. */
resource_t *pci_get_bar_resource(device_t *dev, unsigned int bar);
/* Resolve the INTx interrupt line for an enumerated PCI function through
 * arch_pci_intx_irq(), or -1 when the platform has no routing for it.
 * Drivers must keep their polling fallback for the -1 case and must NOT
 * treat the legacy IRQ Line register as a usable interrupt identifier. */
int pci_intx_irq(const device_t *dev);
void pci_enumerate(uintptr_t ecam_base, int bus_start, int bus_end);

/* Create a VirtIO 1.0 (modern PCI) transport for an enumerated PCI function.
 * type is the VirtIO device ID (1=net, 2=blk, 8=scsi, 16=gpu, 18=input,
 * 25=sound). */
struct virtio_transport;
int pci_virtio_transport_init(device_t *dev, int type,
                              struct virtio_transport *transport);

#endif
