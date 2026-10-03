#ifndef _DRIVERS_BUS_PCI_MSIX_H
#define _DRIVERS_BUS_PCI_MSIX_H

#include "drivers/core/driver_core.h"

/*
 * PCI capability identifiers (PCI 3.7) plus the MSI-X specific layout from
 * PCI 3.7.5.  These are capability-list offsets from the start of the
 * capability's own header, so a cap offset read from the status register and
 * one of these can be added directly.
 */
#define PCI_CAP_ID_MSI             0x05U
#define PCI_CAP_ID_MSIX            0x11U

#define PCI_MSIX_CAP_MESSAGE_CONTROL 0x02U
#define PCI_MSIX_CAP_MESSAGE_ADDR_LO 0x04U
#define PCI_MSIX_CAP_MESSAGE_ADDR_HI 0x08U
#define PCI_MSIX_CAP_MESSAGE_DATA    0x0CU
#define PCI_MSIX_CAP_VECTOR_CONTROL  0x10U
#define PCI_MSIX_CAP_PBA_CONTROL     0x14U

/* Message Control (PCI 3.7.5.1). */
#define PCI_MSIX_TABLE_SIZE_MASK  0x07FFU
#define PCI_MSIX_FUNCTION_MASK    (1U << 14)
#define PCI_MSIX_ENABLE           (1U << 15)

/* Table entry, 16 bytes each (PCI 3.7.5.3). */
#define PCI_MSIX_ENTRY_SIZE       16U
#define PCI_MSIX_ENTRY_ADDR_LO    0x00U
#define PCI_MSIX_ENTRY_ADDR_HI    0x04U
#define PCI_MSIX_ENTRY_DATA       0x08U
#define PCI_MSIX_ENTRY_CTRL       0x0CU
#define PCI_MSIX_ENTRY_MASK       (1U << 0)
/* Two encodings of the same pair of numbers are in play, and which one a field
 * uses is decided by the field, not by the device.
 *
 * Vector Control (PCI 3.7.5.3, the PCI Express form): bits 3:1 name the BAR,
 * bits 31:12 hold the table's offset inside it divided by sixteen.
 */
#define PCI_MSIX_BIR_MASK         0x7U
#define PCI_MSIX_BIR_SHIFT        1U
#define PCI_MSIX_OFFSET_SHIFT     4U
#define PCI_MSIX_VC_OFFSET_MASK   0xFFFFFFF0U

/* Message Address Low (the pre-PCIe form the field inherits, still what a
 * device publishes here when no firmware programmed Vector Control): bits 2:0
 * name the BAR and bits 31:3 hold the byte offset directly, with no scaling. */
#define PCI_MSIX_LEGACY_BIR_MASK  0x7U
#define PCI_MSIX_LEGACY_BIR_SHIFT 0U
#define PCI_MSIX_LEGACY_OFFSET_MASK 0xFFFFFFF8U

/* PBA bitmap, ceil(table_size / 64) 64-bit words (PCI 3.7.5.4). */
#define PCI_MSIX_PBA_WORDS(size)  (((size) + 63U) / 64U)

typedef struct pci_msix_info {
    uint8_t  cap_offset;      /* config-space offset, dword aligned */
    uint8_t  table_size;      /* number of table entries the device implements */
    uint8_t  table_bir;       /* BAR holding the table */
    uint8_t  pba_bir;         /* BAR holding the pending-bit array */
    uint32_t table_offset;    /* table offset inside table_bir */
    int      from_vector_ctrl; /* 1: location came from Vector Control,
                                * 0: from the Message Address Lower field */
    int      enabled;         /* MSI-X Enable bit as last programmed */
} pci_msix_info_t;

/* Locate the MSI-X capability.  Returns 0 and fills *out, or -ENODEV when the
 * function has none, -EOVERFLOW when the capability list is malformed. */
int pci_msix_capability(const device_t *dev, pci_msix_info_t *out);

/* Disable delivery and mask the whole function while the table is rewritten.
 * @vectors must not exceed the table size.  Program vectors with
 * pci_msix_program_vector() next, register handlers, then pci_msix_commit(). */
int pci_msix_enable(device_t *dev, unsigned vectors);

/* Write one masked table entry pointing at the platform's message address
 * and carrying @vector as the message data. */
int pci_msix_program_vector(device_t *dev, unsigned index, uint32_t vector);

/* Unmask/mask a single entry.  Only meaningful after pci_msix_commit(). */
void pci_msix_set_vector_mask(device_t *dev, unsigned index, int masked);

/* Clear the function mask and set Enable, so unmasked entries can fire. */
int pci_msix_commit(device_t *dev);

/* Quiesce the function: mask every programmed entry, then drop Enable. */
void pci_msix_disable(device_t *dev);

/*
 * Platform hooks.  Every one is weak and returns a negative errno on the
 * platforms that have no message-signalled interrupt path at all, which is
 * what keeps a driver on its INTx or completion-polling fallback instead of
 * programming a capability that will never be delivered.
 */

/* Physical address the device must post its message to for @vector. */
int __attribute__((weak)) arch_msix_message_address(uint32_t vector,
                                                   uint32_t *addr_lo,
                                                   uint32_t *addr_hi);
/* Program (or mask) the local interrupt controller entry for @vector.  Called
 * before the entry is unmasked and again whenever the mask changes. */
int __attribute__((weak)) arch_msix_vector_setup(uint32_t vector, int masked);

#endif /* _DRIVERS_BUS_PCI_MSIX_H */
