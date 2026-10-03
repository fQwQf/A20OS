/*
 * MSI-X capability handling.
 *
 * The kernel's interrupt model delivers every device interrupt on a numbered
 * line that an irqchip routes; a request_irq() handler sits behind that line.
 * MSI-X replaces the wiring with a memory write the device performs itself, so
 * this file's whole job is to make the device's table describe vectors that the
 * existing dispatch path already understands: address from the platform, data
 * = the IRQ line id, and an entry that stays masked until a handler exists.
 *
 * Everything here is capability-driven.  A function with no MSI-X capability,
 * a platform with no arch_msix_message_address(), or a driver that never asks
 * for vectors leaves the device exactly as it was found, and the caller keeps
 * its INTx or completion-polling path.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/pci_msix.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/string.h"

/* One record per MSI-X function the kernel has programmed.  The table address
 * is resolved once, at enable time: the BARs are already assigned by then and
 * relocating a device with a live table behind it is not something a hot-unplug
 * path does.  Records are keyed by the published device_t, whose address is
 * stable for as long as the function stays enumerated, so no part of the
 * enumeration record's private layout has to be visible here. */
#define PCI_MSIX_MAX_FUNCTIONS 32

typedef struct pci_msix_state {
    const device_t *dev;
    uint8_t  table_size;
    uint8_t  table_bir;
    uint32_t table_offset;
    uintptr_t table;          /* kernel address of entry 0 */
    unsigned programed;       /* entries written by pci_msix_program_vector() */
} pci_msix_state_t;

static pci_msix_state_t g_msix[PCI_MSIX_MAX_FUNCTIONS];

static pci_msix_state_t *msix_state(const device_t *dev, int create);

int __attribute__((weak)) arch_msix_message_address(uint32_t vector,
                                                   uint32_t *addr_lo,
                                                   uint32_t *addr_hi)
{
    (void)vector;
    (void)addr_lo;
    (void)addr_hi;
    return -EOPNOTSUPP;
}

int __attribute__((weak)) arch_msix_vector_setup(uint32_t vector, int masked)
{
    (void)vector;
    (void)masked;
    return -EOPNOTSUPP;
}

int pci_msix_capability(const device_t *dev, pci_msix_info_t *out)
{
    if (!dev || !out)
        return -EINVAL;

    uint8_t offset = pci_find_capability(dev, PCI_CAP_ID_MSIX);
    if (!offset)
        return -ENODEV;

    uint16_t control = pci_cfg_read16(dev, (uint32_t)offset +
                                      PCI_MSIX_CAP_MESSAGE_CONTROL);
    uint32_t message_addr_lo = pci_cfg_read32(dev, (uint32_t)offset +
                                              PCI_MSIX_CAP_MESSAGE_ADDR_LO);
    uint32_t vector_control = pci_cfg_read32(dev, (uint32_t)offset +
                                             PCI_MSIX_CAP_VECTOR_CONTROL);
    uint32_t pba_control = pci_cfg_read32(dev, (uint32_t)offset +
                                          PCI_MSIX_CAP_PBA_CONTROL);

    out->cap_offset = offset;
    out->table_size = (uint8_t)((control & PCI_MSIX_TABLE_SIZE_MASK) + 1U);
    out->enabled    = (control & PCI_MSIX_ENABLE) ? 1 : 0;

    /*
     * The table's home is named in Vector Control: bits 3:1 the BAR, bits 31:12
     * the offset within it.  That register is firmware-writable, and on a
     * firmwareless boot it reads zero for every device -- which does not mean
     * "the table is at BAR0+0", it means nobody wrote it down.
     *
     * The Message Address Lower field is the other place a device states the
     * same thing, in the same BIR/offset encoding, and it is a device ROM field
     * that no firmware touches.  A device whose firmware did program Vector
     * Control is believed first, since that is the architecturally defined
     * source; when it is silent we fall back to the field the device itself
     * published.  Rejecting the all-zero case is what keeps a device that
     * published neither from having its table programmed over BAR0, which for
     * an I/O window would mean writing over live device registers.
     */
    if (vector_control != 0U) {
        out->table_bir = (uint8_t)((vector_control >> PCI_MSIX_BIR_SHIFT) &
                                   PCI_MSIX_BIR_MASK);
        out->table_offset = (vector_control & PCI_MSIX_VC_OFFSET_MASK) <<
                            PCI_MSIX_OFFSET_SHIFT;
        out->from_vector_ctrl = 1;
    } else {
        out->table_bir = (uint8_t)((message_addr_lo >>
                                    PCI_MSIX_LEGACY_BIR_SHIFT) &
                                   PCI_MSIX_LEGACY_BIR_MASK);
        out->table_offset = message_addr_lo & PCI_MSIX_LEGACY_OFFSET_MASK;
        out->from_vector_ctrl = 0;
    }
    out->pba_bir    = (uint8_t)((pba_control >> PCI_MSIX_BIR_SHIFT) &
                                PCI_MSIX_BIR_MASK);

    /* A table of zero entries cannot exist -- the field is N-1 -- and a table
     * larger than 2048 would run past the PBA the size field implies. */
    if (out->table_size == 0)
        return -EINVAL;
    return 0;
}

static pci_msix_state_t *msix_state(const device_t *dev, int create)
{
    if (!dev)
        return NULL;

    int free_slot = -1;
    for (int i = 0; i < PCI_MSIX_MAX_FUNCTIONS; i++) {
        if (!g_msix[i].dev) {
            if (free_slot < 0)
                free_slot = i;
            continue;
        }
        if (g_msix[i].dev == dev)
            return &g_msix[i];
    }
    if (!create || free_slot < 0)
        return NULL;

    pci_msix_state_t *st = &g_msix[free_slot];
    memset(st, 0, sizeof(*st));
    st->dev = dev;
    return st;
}

static void msix_control_write(const device_t *dev, uint8_t offset,
                               uint16_t set, uint16_t clear)
{
    uint16_t control = pci_cfg_read16(dev, (uint32_t)offset +
                                      PCI_MSIX_CAP_MESSAGE_CONTROL);
    control = (uint16_t)((control & ~clear) | set);
    pci_cfg_write16(dev, (uint32_t)offset + PCI_MSIX_CAP_MESSAGE_CONTROL,
                    control);
}

int pci_msix_enable(device_t *dev, unsigned vectors)
{
    pci_msix_info_t info;
    int r = pci_msix_capability(dev, &info);
    if (r)
        return r;

    uint32_t addr_lo, addr_hi;
    if (arch_msix_message_address(0, &addr_lo, &addr_hi) < 0) {
        kinfo("[MSI-X] %s: platform has no message-signalled interrupt path\n",
              dev->name);
        return -EOPNOTSUPP;
    }
    if (vectors == 0 || vectors > info.table_size) {
        kerr("[MSI-X] %s: %u vectors requested, table has %u\n",
             dev->name, vectors, info.table_size);
        return -EINVAL;
    }

    pci_msix_state_t *st = msix_state(dev, 1);
    if (!st) {
        kerr("[MSI-X] %s: state table full (%d functions)\n",
             dev->name, PCI_MSIX_MAX_FUNCTIONS);
        return -ENOSPC;
    }

    /* Neither field names a table: the function, and there is nothing
     * to program.  Saying so is the honest answer -- guessing BIR 0 here would
     * write entries over whatever BAR0 happens to hold. */
    if (info.table_bir == 0 && info.table_offset == 0) {
        kinfo("[MSI-X] %s: capability names no table (Vector Control and "
              "Message Address Low both read zero); keeping the legacy "
              "interrupt path\n", dev->name);
        return -EOPNOTSUPP;
    }

    resource_t *res = pci_get_bar_resource(dev, info.table_bir);
    if (!res) {
        kerr("[MSI-X] %s: table BIR %u is not an assigned memory BAR\n",
             dev->name, info.table_bir);
        return -EINVAL;
    }

    /* Quiesce before touching anything: clearing Enable while a change is
     * half-written is exactly how a device ends up posting a message with a
     * stale data value. */
    msix_control_write(dev, info.cap_offset,
                       PCI_MSIX_FUNCTION_MASK, PCI_MSIX_ENABLE);

    st->table_size = info.table_size;
    st->table = (uintptr_t)res->start + info.table_offset;
    st->programed = 0;

    kinfo("[MSI-X] %s: capability at 0x%02x, table %u entries in BAR%u+0x%x "
          "(%s, pba BAR%u), %u requested\n",
          dev->name, info.cap_offset, info.table_size, info.table_bir,
          info.table_offset,
          info.from_vector_ctrl ? "Vector Control"
                                : "Message Address Low",
          info.pba_bir, vectors);
    return 0;
}

int pci_msix_program_vector(device_t *dev, unsigned index, uint32_t vector)
{
    pci_msix_state_t *st = msix_state(dev, 0);
    if (!st || !st->table)
        return -EINVAL;
    if (index >= st->table_size)
        return -EINVAL;

    uint32_t addr_lo, addr_hi;
    int r = arch_msix_message_address(vector, &addr_lo, &addr_hi);
    if (r)
        return r;

    volatile uint32_t *entry = (volatile uint32_t *)
        (st->table + (uintptr_t)index * 16U);

    /* The local controller entry has to be armed before the device can post a
     * message for this vector, and it starts masked for the same reason the
     * table entry does: request_irq() is what unmasks either. */
    r = arch_msix_vector_setup(vector, 1);
    if (r)
        return r;

    writel(addr_lo, (volatile void *)&entry[0]);
    writel(addr_hi, (volatile void *)&entry[1]);
    writel(vector, (volatile void *)&entry[2]);
    writel(PCI_MSIX_ENTRY_MASK, (volatile void *)&entry[3]);

    /* Read the entry back before believing it.  Writing to the wrong window is
     * the failure this whole layer exists to prevent, and it is silent: a
     * device that ignores the write simply never interrupts, while a device
     * whose BAR is a different register file is now corrupt.  The message
     * fields are what decide delivery, so those are the ones checked here. */
    if (entry[0] != addr_lo || entry[1] != addr_hi || entry[2] != vector ||
        (entry[3] & ((uint32_t)PCI_MSIX_BIR_MASK << PCI_MSIX_BIR_SHIFT))) {
        kerr("[MSI-X] %s: entry %u read back %08x %08x %08x %08x after "
             "writing %08x %08x %08x %08x; this window is not an MSI-X "
             "table\n",
             dev->name, index, entry[0], entry[1], entry[2], entry[3],
             addr_lo, addr_hi, vector, PCI_MSIX_ENTRY_MASK);
        return -EIO;
    }

    if (index >= st->programed)
        st->programed = index + 1U;
    return 0;
}

void pci_msix_set_vector_mask(device_t *dev, unsigned index, int masked)
{
    pci_msix_state_t *st = msix_state(dev, 0);
    if (!st || !st->table || index >= st->table_size)
        return;

    volatile uint32_t *ctrl = (volatile uint32_t *)
        (st->table + (uintptr_t)index * 16U + PCI_MSIX_ENTRY_CTRL);
    uint32_t value = *ctrl;
    /* BIR lives in Vector Control when the capability is read through the
     * offset form; a programmed entry uses BIR 0 because the table pointer in
     * the capability already selected the BAR.  Clear it explicitly so a
     * read-modify-write cannot inherit one from a previous owner. */
    value &= ~((uint32_t)PCI_MSIX_BIR_MASK << PCI_MSIX_BIR_SHIFT);
    if (masked)
        value |= PCI_MSIX_ENTRY_MASK;
    else
        value &= ~(uint32_t)PCI_MSIX_ENTRY_MASK;
    *ctrl = value;
}

int pci_msix_commit(device_t *dev)
{
    pci_msix_info_t info;
    int r = pci_msix_capability(dev, &info);
    if (r)
        return r;

    pci_msix_state_t *st = msix_state(dev, 0);
    if (!st || !st->programed)
        return -EINVAL;

    msix_control_write(dev, info.cap_offset,
                       PCI_MSIX_ENABLE, PCI_MSIX_FUNCTION_MASK);
    /* Enable is the last thing set, so every entry is already unmasked by the
     * time this runs.  Reading the control words back proves the table is live
     * rather than a window the device ignores -- and a device that reports a
     * mask bit still set is one that would swallow messages the handlers are
     * ready for, which is a failure worth catching before it costs a hang. */
    for (unsigned i = 0; i < st->programed; i++) {
        volatile uint32_t *ctrl = (volatile uint32_t *)
            (st->table + (uintptr_t)i * PCI_MSIX_ENTRY_SIZE + PCI_MSIX_ENTRY_CTRL);
        if (*ctrl & PCI_MSIX_ENTRY_MASK) {
            kerr("[MSI-X] %s: entry %u is still masked after unmasking; "
                 "not enabling the function\n", dev->name, i);
            pci_msix_disable(dev);
            return -EIO;
        }
    }
    kinfo("[MSI-X] %s: enabled, %u vector(s) armed\n", dev->name,
          st->programed);
    return 0;
}

void pci_msix_disable(device_t *dev)
{
    pci_msix_state_t *st = msix_state(dev, 0);
    if (!st)
        return;

    /* Mask first, then clear Enable: a message posted between the two would
     * otherwise land on a vector with no owner. */
    for (unsigned i = 0; i < st->programed && i < st->table_size; i++)
        pci_msix_set_vector_mask(dev, i, 1);

    pci_msix_info_t info;
    if (pci_msix_capability(dev, &info) == 0) {
        msix_control_write(dev, info.cap_offset,
                           PCI_MSIX_FUNCTION_MASK, PCI_MSIX_ENABLE);
    }
    st->table = 0;
    st->programed = 0;
    st->dev = NULL;
}
