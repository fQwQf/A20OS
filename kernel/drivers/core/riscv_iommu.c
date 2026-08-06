/*
 * RISC-V IOMMU PCI discovery skeleton.
 *
 * QEMU 10 exposes riscv-iommu-pci as vendor 1b36/device 0014. This
 * driver deliberately performs discovery only: it records the BAR and
 * identity without enabling translation. Enabling an IOMMU requires
 * DDT/IOPMP ownership, command/fault queues, and page-table
 * invalidation; those are the next enforcement slice documented in
 * docs/hybrid-kernel/04-dual-placement.md.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_register.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/pci_hal.h"
#include "core/klog.h"

#define RISCV_IOMMU_VENDOR 0x1b36u
#define RISCV_IOMMU_DEVICE 0x0014u

static const device_id_t riscv_iommu_ids[] = {
    { RISCV_IOMMU_VENDOR, RISCV_IOMMU_DEVICE, VENDOR_ANY, DEVICE_ANY, 0 },
    { 0, 0, 0, 0, 0 }
};

static int riscv_iommu_probe(device_t *dev)
{
    if (!dev)
        return -1;
    resource_t *bar = pci_get_bar_resource(dev, 0);
    if (!bar || bar->type != RES_MMIO || bar->start >= bar->end)
        return -1;
    kinfo("[IOMMU] riscv-iommu discovered: BAR0=0x%lx..0x%lx; "
          "translation disabled (discovery-only phase)\n",
          (unsigned long)bar->start, (unsigned long)bar->end);
    return 0;
}

static int riscv_iommu_remove(device_t *dev)
{
    (void)dev;
    return 0;
}

/* The generic PCI lifecycle currently does not bind class-only devices
 * before its bus probe pass. Keep a harmless early discovery hook so the
 * capability is observable while that lifecycle is completed. */
void riscv_iommu_early_probe(void)
{
    uint32_t id = arch_pci_config_read32(0, 1, 0, 0x00);
    if ((id & 0xffffu) != RISCV_IOMMU_VENDOR ||
        (id >> 16) != RISCV_IOMMU_DEVICE)
        return;
    uint32_t bar = arch_pci_config_read32(0, 1, 0, 0x10);
    kinfo("[IOMMU] riscv-iommu PCI present at 00:01.0 BAR0=0x%lx; "
          "translation disabled (discovery-only phase)\n",
          (unsigned long)bar);
}

static driver_t riscv_iommu_driver = {
    .name = "riscv-iommu",
    .id_table = riscv_iommu_ids,
    .bus = &pci_bus,
    .probe = riscv_iommu_probe,
    .remove = riscv_iommu_remove,
    .class_type = DEV_CLASS_NONE,
};

DRIVER_REGISTER(riscv_iommu_driver);
