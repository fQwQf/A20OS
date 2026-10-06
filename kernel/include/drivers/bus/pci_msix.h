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

/* Logical CPU ids used as the MSI-X destination.  CPU 0 is the boot processor,
 * and it is where every vector stays until a driver moves it, so an untouched
 * device behaves exactly as it did before affinity existed. */
#define PCI_MSIX_CPU_BOOT        0

/* How many entries of a function the kernel can remember a destination for.
 * Every caller in the tree (virtio one vector per queue, e1000e two, NVMe one
 * per queue) stays far below this; the bound exists so the per-function state
 * is a fixed array instead of an allocation on the interrupt path. */
#define PCI_MSIX_AFFINITY_VECTORS 64U

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

/* Affinity: which logical CPU a programmed entry's message is aimed at.
 *
 * The entry has to be masked, its message address rewritten and the local
 * controller entry armed on the new CPU before it is unmasked again, so a
 * message never lands on a CPU that has no handler behind that vector.
 * pci_msix_set_affinity() does that sequence and leaves the entry unmasked
 * only if it was unmasked before.  Asking for the CPU the entry already
 * points at is a successful no-op.
 *
 * Returns -EINVAL for an entry that was never programmed or a negative CPU,
 * -ERANGE for an index past PCI_MSIX_AFFINITY_VECTORS, and whatever errno the
 * arch hooks return otherwise: on a platform whose message path is boot-CPU-
 * only they refuse first, so nothing is half-programmed on the way out.
 *
 * The two failure modes after the mask-before-write step are not equivalent.  A
 * readback mismatch (-EIO) means the write did not take, so the entry still
 * carries the old CPU's address -- and because both the device entry and the
 * old CPU's local entry were masked for the rewrite, the rollback re-arms the
 * old CPU and clears the device mask again, restoring the pre-call state
 * exactly.  A controller entry that could not be armed on the new CPU leaves
 * the entry MASKED on the new CPU's address: the device is stopped rather than
 * allowed to post into a vector with no armed handler, which would drop the
 * message silently.  Either way the entry is left in a state the caller can
 * name, not half-live. */
int pci_msix_set_affinity(device_t *dev, unsigned index, int cpu);

/* Current destination of a programmed entry; -ENOENT if it is not one. */
int pci_msix_get_affinity(device_t *dev, unsigned index, int *cpu);

/* Retarget every programmed entry of every enumerated function.  Returns 0
 * only if all of them moved; a negative errno from the first entry that
 * refused is returned, so a caller cannot mistake a partial move for
 * success.  This is the kernel-wide form behind /proc/a20/irq_affinity. */
int pci_msix_set_all_affinity(int cpu);

/* One line of /proc/a20/irq_affinity: what the kernel programmed and where it
 * points now.  @name borrows the enumeration record's device name and stays
 * valid for as long as the function stays enumerated. */
typedef struct pci_msix_affinity_entry {
    const char *name;
    unsigned index;
    uint32_t vector;
    int cpu;
} pci_msix_affinity_entry_t;

/* Copy up to @max programmed entries into @out and return how many exist in
 * total (which can exceed @max, so a caller can size a buffer). */
unsigned pci_msix_affinity_snapshot(pci_msix_affinity_entry_t *out,
                                    unsigned max);

/*
 * Platform hooks.  A platform that has a message-signalled interrupt path
 * defines these strongly (x86_64 does, in arch/x86_64/trap/irqchip.c); the
 * only definition that is weak is the -EOPNOTSUPP stub in drivers/bus/
 * pci_msix.c, which is what keeps a driver on its INTx or completion-polling
 * fallback on the platforms that have no such path instead of programming a
 * capability that will never be delivered.
 *
 * The prototypes below therefore carry NO weak attribute on purpose: GCC
 * applies an attribute seen on a declaration to the definition that follows
 * it in the same translation unit, so marking these weak here would make the
 * platform's real implementation weak too -- and with two weak definitions
 * of one symbol the link outcome is whichever the linker happens to pick
 * first, which is how the stub ended up winning over the x86_64 hooks.
 */

/* Physical address the device must post its message to for @vector aimed at
 * logical CPU @cpu.  Both the vector and the destination travel in the message
 * itself, so the two hooks below are asked about the same pair every time. */
int arch_msix_message_address(uint32_t vector, int cpu,
                              uint32_t *addr_lo,
                              uint32_t *addr_hi);
/* Program (or mask) the local interrupt controller entry for @vector on
 * logical CPU @cpu.  Called on the current CPU before the entry is unmasked
 * and again whenever the mask changes or the entry moves; the platform is
 * responsible for reaching a remote CPU. */
int arch_msix_vector_setup(uint32_t vector, int cpu,
                           int masked);
/* Number of logical CPUs a message-signalled vector may be aimed at.  One on
 * every platform that has no per-CPU destination, which is what makes an
 * out-of-range request a clean refusal rather than a wrong write. */
int arch_irq_msix_cpu_count(void);

#endif /* _DRIVERS_BUS_PCI_MSIX_H */
