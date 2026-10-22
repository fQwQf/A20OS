/*
 * PCI MSI (capability 0x05) programming.
 *
 * See pci_msi.h for the contract.  The register layout is the pre-MSI-X one
 * from PCI 3.7.6: a capability header, Message Control, an address that is
 * either 32 or 64 bits, and a 16-bit data word.  There is no table and no
 * per-entry mask: the function has one message (this implementation programs
 * one), and Enable is the only switch.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/pci_msi.h"
#include "drivers/bus/pci_msix.h"
#include "core/errno.h"
#include "core/klog.h"

int pci_msi_capability(const device_t *dev, pci_msi_info_t *out)
{
    if (!dev || !out)
        return -EINVAL;
    uint8_t offset = pci_find_capability(dev, PCI_CAP_ID_MSI);
    if (!offset)
        return -ENODEV;
    uint16_t control = pci_cfg_read16(dev, (uint32_t)offset +
                                      PCI_MSI_CAP_MESSAGE_CONTROL);
    out->cap_offset = offset;
    out->enabled = (control & PCI_MSI_ENABLE) ? 1 : 0;
    out->address_64 = (control & PCI_MSI_64BIT) ? 1 : 0;
    out->per_vector_mask = (control & PCI_MSI_PER_VECTOR_MASK) ? 1 : 0;
    out->vectors = (uint8_t)(1U << ((control & PCI_MSI_MMC_MASK) >>
                                    PCI_MSI_MMC_SHIFT));
    return 0;
}

/* Write the address/data pair and enable delivery.  The message address comes
 * from the same arch hook MSI-X uses; refusing here (-EOPNOTSUPP on a platform
 * with no LAPIC message support) is what sends the driver to INTx. */
int pci_msi_enable(device_t *dev, uint32_t vector)
{
    pci_msi_info_t info;
    if (pci_msi_capability(dev, &info) < 0)
        return -ENODEV;
    if (vector == 0)
        return -EINVAL;

    uint32_t addr_lo = 0, addr_hi = 0;
    int r = arch_msix_message_address(vector, PCI_MSIX_CPU_BOOT,
                                      &addr_lo, &addr_hi);
    if (r)
        return r;
    r = arch_msix_vector_setup(vector, PCI_MSIX_CPU_BOOT, 0);
    if (r)
        return r;

    /* Disable while rewriting, so a stale address/data from firmware cannot
     * raise a vector before the new pair is complete. */
    uint16_t control = pci_cfg_read16(dev, (uint32_t)info.cap_offset +
                                      PCI_MSI_CAP_MESSAGE_CONTROL);
    pci_cfg_write16(dev, (uint32_t)info.cap_offset +
                    PCI_MSI_CAP_MESSAGE_CONTROL,
                    (uint16_t)(control & ~PCI_MSI_ENABLE));
    pci_cfg_write32(dev, (uint32_t)info.cap_offset + PCI_MSI_CAP_MESSAGE_ADDR_LO,
                    addr_lo);
    if (info.address_64)
        pci_cfg_write32(dev, (uint32_t)info.cap_offset +
                        PCI_MSI_CAP_MESSAGE_ADDR_HI, addr_hi);
    pci_cfg_write16(dev, (uint32_t)info.cap_offset + PCI_MSI_CAP_MESSAGE_DATA,
                    (uint16_t)vector);

    uint32_t readback = pci_cfg_read32(dev, (uint32_t)info.cap_offset +
                                       PCI_MSI_CAP_MESSAGE_ADDR_LO);
    if (readback != addr_lo) {
        kerr("[MSI] %s: address readback 0x%x != 0x%x\n",
             dev->name, readback, addr_lo);
        (void)arch_msix_vector_setup(vector, PCI_MSIX_CPU_BOOT, 1);
        return -EIO;
    }

    control = pci_cfg_read16(dev, (uint32_t)info.cap_offset +
                             PCI_MSI_CAP_MESSAGE_CONTROL);
    /* Single-message encoding: Multiple Message Enable = 0. */
    control &= (uint16_t)~(PCI_MSI_MMC_MASK << PCI_MSI_MMC_SHIFT);
    pci_cfg_write16(dev, (uint32_t)info.cap_offset +
                    PCI_MSI_CAP_MESSAGE_CONTROL,
                    (uint16_t)(control | PCI_MSI_ENABLE));
    kinfo("[MSI] %s: enabled vector %u (cap 0x%x%s)\n",
          dev->name, vector, info.cap_offset,
          info.address_64 ? ", 64-bit address" : "");
    return 0;
}

void pci_msi_disable(device_t *dev)
{
    pci_msi_info_t info;
    if (pci_msi_capability(dev, &info) < 0)
        return;
    uint16_t control = pci_cfg_read16(dev, (uint32_t)info.cap_offset +
                                      PCI_MSI_CAP_MESSAGE_CONTROL);
    pci_cfg_write16(dev, (uint32_t)info.cap_offset +
                    PCI_MSI_CAP_MESSAGE_CONTROL,
                    (uint16_t)(control & ~PCI_MSI_ENABLE));
}
