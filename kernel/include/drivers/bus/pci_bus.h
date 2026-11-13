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
/* Process context; caller must quiesce hardware before BAR sizing/relocation.
 * Serializes with discovery and checked aperture claims. Plans and validates
 * every BAR before writing a new address; failed planning preserves command,
 * BARs and published resources (the monotonic allocation cursor may advance).
 * It does not drain in-flight DMA or make config writes fallible. */
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
/* Published function address, without exposing the private enumeration record.
 * Returns -ENODEV for devices on another bus. */
int pci_device_location(const device_t *dev, unsigned *bus,
                        unsigned *slot, unsigned *function);
/* Process-context lookup of a published function. Returns a device reference
 * (device_put by caller), or clears out on failure. Current hosts support only
 * segment zero (-EOPNOTSUPP otherwise); missing functions return -ENODEV.
 * The reference pins metadata, not physical presence or a driver binding.
 * Serialize lookup AND subsequent binding/config use with a discovery
 * transaction when they must describe the same lifecycle observation. */
int pci_device_find_get(uint16_t segment, uint16_t requester, device_t **out);
/* Native software ownership of an explicit requester set. No group number,
 * inferred aliases or isolation guarantee. IDs need not be published; the
 * caller supplies the complete set from its verified hardware/firmware policy.
 * This lease does not block raw config writes or nonparticipating providers. */
#define PCI_REQUESTER_SET_MAX 128
typedef struct pci_requester_reservation pci_requester_reservation_t;
/* Task context, caller holds a registered owner reference. Copies, sorts and
 * deduplicates 1..MAX IDs, then reserves the whole set atomically. Overlap is
 * -EBUSY even for the same owner. Nonzero segment -> -EOPNOTSUPP; too many
 * input IDs -> -E2BIG. Failure clears out. Pins owner, not its binding; the
 * reservation survives unregister, BDF replacement and controller rebind. */
int pci_requester_reserve(device_t *owner, uint16_t segment,
                            const uint16_t *requesters, size_t count,
                            pci_requester_reservation_t **out);
/* Task context, exclusive release. DMA/context invalidation must already be
 * confirmed by the provider; preserve this handle on uncertain hardware stop.
 * No automatic release on device teardown and no hardware access here. */
void pci_requester_release(pci_requester_reservation_t *);
/* Task-context topology observation. Finds the client itself or an ancestor
 * bridge on scope_bus whose devfn lies in [first,last], returning a reference
 * released by device_put(). Validates the whole published bridge path:
 * missing scope/client -> -ENODEV, overlapping routes -> -EBUSY, malformed
 * bridge windows -> -EINVAL, cycles -> -ELOOP. Identity changes fail closed.
 * Clears out on error. No enumeration, BAR/config writes or inferred device
 * dependencies. This does not prove ACS/alias isolation or freeze hardware.
 * Caller holds a client reference; use an outer discovery transaction across
 * this query AND subsequent binding/config observations when needed. */
int pci_device_scope_get(device_t *client, uint8_t scope_bus, uint8_t first,
                           uint8_t last, device_t **out);
/* Return the MMIO or I/O-port resource for a physical PCI BAR number.
 * Resources are compacted in device->res, so display drivers must not assume
 * that BAR2 is the second resource when BAR0 is a 64-bit BAR. */
resource_t *pci_get_bar_resource(device_t *dev, unsigned int bar);
void *pci_map_bar(device_t *dev, unsigned int bar);
/* Resolve the INTx interrupt line for an enumerated PCI function through
 * arch_pci_intx_irq(), or -1 when the platform has no routing for it.
 * Drivers must keep their polling fallback for the -1 case and must NOT
 * treat the legacy IRQ Line register as a usable interrupt identifier. */
int pci_intx_irq(const device_t *dev);
int pci_user_device_find(uint16_t vendor, uint16_t device,
                         pci_user_device_info_t *out);
/* Nth (zero-based) matching function.  pci_user_device_find() is index 0. */
int pci_user_device_find_index(uint16_t vendor, uint16_t device,
                               unsigned index, pci_user_device_info_t *out);
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
