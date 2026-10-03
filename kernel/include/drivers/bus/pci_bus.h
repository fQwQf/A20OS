#ifndef _DRIVERS_BUS_PCI_BUS_H
#define _DRIVERS_BUS_PCI_BUS_H

#include "drivers/core/driver_core.h"

extern bus_type_t pci_bus;

/* Narrow PCI view used by the user-driver isolation layer.  The private PCI
 * enumeration record stays private; only an explicitly matched function and
 * its BAR0/requester ID cross the boundary. */
typedef struct pci_user_device_info {
    uint16_t vendor;
    uint16_t device;
    uint16_t devid;       /* requester ID: bus << 8 | device << 3 | function */
    uint16_t irq;
    uint64_t bar0_phys;
    uint64_t bar0_size;
} pci_user_device_info_t;

bus_type_t *get_pci_bus(void);
int pci_enable_and_assign_bars(device_t *dev);
/*
 * Config-space access for an already published function.  All reads and writes
 * are dword accesses at ECAM: a byte or half-word crossing a dword boundary is
 * not split, so a register that straddles 0x3C must be read through
 * pci_cfg_read32() instead.
 */
uint8_t  pci_cfg_read8(const device_t *dev, uint32_t reg);
uint16_t pci_cfg_read16(const device_t *dev, uint32_t reg);
uint32_t pci_cfg_read32(const device_t *dev, uint32_t reg);
void pci_cfg_write16(const device_t *dev, uint32_t reg, uint16_t val);
void pci_cfg_write32(const device_t *dev, uint32_t reg, uint32_t val);
/* Walk the capability list and return the config-space offset of @cap_id, or
 * 0 when the function has no such capability.  Also returns 0 when the list
 * advertises capabilities but the first pointer is unmasked garbage, so a
 * caller can treat "no capability" and "broken list" alike. */
uint8_t pci_find_capability(const device_t *dev, uint8_t cap_id);
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
int pci_user_device_find(uint16_t vendor, uint16_t device,
                         pci_user_device_info_t *out);
int pci_user_device_bus_master(uint16_t devid, int enable);
void pci_enumerate(uintptr_t ecam_base, int bus_start, int bus_end);
/* Reconcile ECAM with the published PCI device set.  Call only from process
 * context; additions probe immediately and vanished functions are removed. */
void pci_rescan(void);

/* Create a VirtIO 1.0 (modern PCI) transport for an enumerated PCI function.
 * type is the VirtIO device ID (1=net, 2=blk, 8=scsi, 16=gpu, 18=input,
 * 25=sound). */
struct virtio_transport;
int pci_virtio_transport_init(device_t *dev, int type,
                              struct virtio_transport *transport);

#endif
