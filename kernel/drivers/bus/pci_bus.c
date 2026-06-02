/*
 * PCI Bus — device enumeration via ECAM or Type-0/1 config cycles
 *
 * Used by LoongArch QEMU virt and future PC platforms.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/klog.h"

#define PCI_ANY_ID          0xFFFFFFFFUL
#define PCI_VENDOR_ID_REDHAT 0x1AF4

#define PCI_MAX_BUS   128
#define PCI_MAX_DEV   32
#define PCI_MAX_FUNC  8

typedef struct pci_bus_data {
    uintptr_t ecam_base;
    int       bus_start;
    int       bus_end;
} pci_bus_data_t;

static pci_bus_data_t g_pci_data;

static uint32_t pci_ecam_read(int bus, int dev, int func, uint32_t reg) {
    uintptr_t addr = g_pci_data.ecam_base
        + ((uintptr_t)bus << 20)
        + ((uintptr_t)dev << 15)
        + ((uintptr_t)func << 12)
        + reg;
    return readl((const volatile void *)addr);
}

static void pci_ecam_write(int bus, int dev, int func, uint32_t reg, uint32_t val) {
    uintptr_t addr = g_pci_data.ecam_base
        + ((uintptr_t)bus << 20)
        + ((uintptr_t)dev << 15)
        + ((uintptr_t)func << 12)
        + reg;
    writel(val, (volatile void *)addr);
}

typedef struct pci_dev_info {
    uint16_t vendor;
    uint16_t device;
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint8_t  irq;
    uint64_t bar[6];
    uint32_t bar_sz[6];
    int      bar_count;
} pci_dev_info_t;

static int pci_match(device_t *dev, const driver_t *drv) {
    if (drv->bus != dev->bus)
        return 0;
    if (!drv->id_table)
        return 0;

    pci_dev_info_t *info = (pci_dev_info_t *)dev->plat_data;
    if (!info)
        return 0;

    for (const device_id_t *id = drv->id_table; id->vendor != 0 || id->device != 0; id++) {
        if ((id->vendor == PCI_ANY_ID || id->vendor == info->vendor) &&
            (id->device == PCI_ANY_ID || id->device == info->device))
            return 1;
    }
    return 0;
}

static bus_type_t pci_bus = {
    .name  = "pci",
    .match = pci_match,
};

bus_type_t *get_pci_bus(void) {
    return &pci_bus;
}

void pci_enumerate(uintptr_t ecam_base, int bus_start, int bus_end) {
    g_pci_data.ecam_base = ecam_base;
    g_pci_data.bus_start = bus_start;
    g_pci_data.bus_end   = bus_end;

    bus_register(&pci_bus);

    static pci_dev_info_t pci_infos[64];
    static resource_t pci_resources[64];
    static char pci_names[64][32];
    int dev_idx = 0;

    for (int bus = bus_start; bus < bus_end && dev_idx < 64; bus++) {
        for (int dev = 0; dev < PCI_MAX_DEV && dev_idx < 64; dev++) {
            for (int func = 0; func < PCI_MAX_FUNC && dev_idx < 64; func++) {
                uint32_t id = pci_ecam_read(bus, dev, func, 0);
                uint16_t vendor = (uint16_t)(id & 0xFFFF);
                if (vendor == 0xFFFF)
                    continue;

                uint16_t device_id = (uint16_t)((id >> 16) & 0xFFFF);

                pci_dev_info_t *info = &pci_infos[dev_idx];
                info->vendor = vendor;
                info->device = device_id;
                info->bus    = (uint8_t)bus;
                info->dev   = (uint8_t)dev;
                info->func  = (uint8_t)func;

                uint32_t irq_line = pci_ecam_read(bus, dev, func, 0x3C);
                info->irq = (uint8_t)(irq_line & 0xFF);

                info->bar_count = 0;
                for (int b = 0; b < 6; b++) {
                    uint32_t bar_lo = pci_ecam_read(bus, dev, func, 0x10 + b * 4);
                    if (bar_lo == 0xFFFFFFFF || bar_lo == 0)
                        continue;

                    info->bar[info->bar_count] = bar_lo & ~0xF;
                    info->bar_sz[info->bar_count] = 0x1000;
                    info->bar_count++;
                }

                resource_t *res = &pci_resources[dev_idx];
                res[0].type  = RES_MMIO;
                res[0].start = info->bar[0];
                res[0].end   = info->bar[0] + 0xFFF;
                res[0].flags = IORESOURCE_MMIO_32BIT;

                if (info->irq != 0 && info->irq != 0xFF) {
                    res[1].type  = RES_IRQ;
                    res[1].start = info->irq;
                    res[1].end   = info->irq;
                    res[1].flags = IORESOURCE_IRQ_LEVEL;
                }

                snprintf(pci_names[dev_idx], sizeof(pci_names[dev_idx]),
                         "pci-%04x:%04x-%d", vendor, device_id, dev_idx);

                static device_t pci_devs[64];
                device_t *pdev     = &pci_devs[dev_idx];
                pdev->name          = pci_names[dev_idx];
                pdev->bus           = &pci_bus;
                pdev->plat_data     = info;
                pdev->res           = res;
                pdev->res_count     = (info->irq && info->irq != 0xFF) ? 2 : 1;
                pdev->state         = DEV_STATE_UNINIT;

                device_register(pdev);
                dev_idx++;
            }
        }
    }

    kinfo("[BUS] pci: found %d devices (ecam=0x%lx, bus %d-%d)\n",
          dev_idx, (unsigned long)ecam_base, bus_start, bus_end);
}
