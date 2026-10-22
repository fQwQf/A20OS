#ifndef _DRIVERS_BUS_PCI_MSI_H
#define _DRIVERS_BUS_PCI_MSI_H

#include "drivers/core/driver_core.h"

/* PCI MSI capability (id 0x05), as distinct from MSI-X (0x11).
 *
 * MSI is one or more messages written into the same per-CPU LAPIC window
 * MSI-X uses; the vector travels in the message data.  The kernel therefore
 * reuses arch_msix_message_address()/arch_msix_vector_setup() for the
 * programming step: a platform that cannot accept MSI-X messages cannot accept
 * MSI ones either, and the weak defaults return -EOPNOTSUPP so callers fall
 * back to INTx.
 *
 * The public entry points program exactly one vector.  Multi-message MSI
 * encodes the message index in low data bits, which no driver in this tree
 * needs yet and which would make the vector-to-cause mapping less obvious than
 * MSI-X's table. */

#define PCI_MSI_CAP_MESSAGE_CONTROL 0x02U
#define PCI_MSI_CAP_MESSAGE_ADDR_LO 0x04U
#define PCI_MSI_CAP_MESSAGE_ADDR_HI 0x08U
#define PCI_MSI_CAP_MESSAGE_DATA    0x0CU

#define PCI_MSI_ENABLE              (1U << 0)
#define PCI_MSI_MMC_MASK            0x07U   /* bits 3:1, 2^n messages */
#define PCI_MSI_MMC_SHIFT           1U
#define PCI_MSI_64BIT               (1U << 4)
#define PCI_MSI_PER_VECTOR_MASK     (1U << 7)

typedef struct pci_msi_info {
    uint8_t  cap_offset;
    uint8_t  enabled;
    uint8_t  address_64;
    uint8_t  per_vector_mask;
    uint8_t  vectors;          /* 1 << Multiple Message Capable */
} pci_msi_info_t;

/* Locate the MSI capability.  Returns 0 and fills *out, -ENODEV when the
 * function has none, -EOVERFLOW for a malformed capability list. */
int pci_msi_capability(const device_t *dev, pci_msi_info_t *out);

/* Enable one message carrying @vector: program the platform message address,
 * unmask that vector at the local controller, then set the capability's
 * Enable bit.  The caller must have reserved @vector with
 * irq_alloc_vectors(1).  Returns 0 on success and leaves the capability
 * disabled on every failure. */
int pci_msi_enable(device_t *dev, uint32_t vector);

/* Clear Enable.  Interrupts already in flight are the caller's problem: it
 * should mask its device-side causes first, as with INTx. */
void pci_msi_disable(device_t *dev);

#endif /* _DRIVERS_BUS_PCI_MSI_H */
