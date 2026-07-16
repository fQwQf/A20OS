/*
 * PCI Bus — device enumeration via ECAM or Type-0/1 config cycles
 *
 * Used by LoongArch QEMU virt and future PC platforms.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/pci_hal.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/klog.h"

#ifdef CONFIG_X86_64
#include "platform.h"
#endif

#define PCI_ANY_ID          0xFFFFFFFFUL
#define PCI_VENDOR_ID_REDHAT 0x1AF4

#define PCI_MAX_BUS   128
#define PCI_MAX_DEV   32
#define PCI_MAX_FUNC  8

/* DRIVER_ENUMERATION_FAILURE_MODEL: PCI enumeration publishes bounded static
 * device records; driver_core rolls failed probes back to unbound devices. */

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
    uint16_t subvendor;
    uint16_t subdevice;
    uint64_t bar[6];
    uint32_t bar_sz[6];
    int      bar_count;
} pci_dev_info_t;

static uintptr_t g_pci_mmio_alloc;

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
            (id->device == PCI_ANY_ID || id->device == info->device) &&
            (id->subvendor == VENDOR_ANY || id->subvendor == info->subvendor) &&
            (id->subdevice == DEVICE_ANY || id->subdevice == info->subdevice))
            return 1;
    }
    return 0;
}

bus_type_t pci_bus = {
    .name  = "pci",
    .match = pci_match,
};

bus_type_t *get_pci_bus(void) {
    return &pci_bus;
}

static uint64_t pci_bar_size(const pci_dev_info_t *info, int bar, uint32_t bar_lo) {
    uint32_t offset = 0x10U + (uint32_t)bar * 4U;
    uint32_t original_hi = 0;
    int is_64 = !(bar_lo & 1U) && ((bar_lo & 0x6U) == 0x4U);

    if (is_64)
        original_hi = pci_ecam_read(info->bus, info->dev, info->func, offset + 4U);
    pci_ecam_write(info->bus, info->dev, info->func, offset, 0xFFFFFFFFU);
    if (is_64)
        pci_ecam_write(info->bus, info->dev, info->func, offset + 4U, 0xFFFFFFFFU);

    uint32_t mask_lo = pci_ecam_read(info->bus, info->dev, info->func, offset);
    uint32_t mask_hi = is_64 ?
        pci_ecam_read(info->bus, info->dev, info->func, offset + 4U) : 0;

    pci_ecam_write(info->bus, info->dev, info->func, offset, bar_lo);
    if (is_64)
        pci_ecam_write(info->bus, info->dev, info->func, offset + 4U, original_hi);

    if (bar_lo & 1U)
        return (uint64_t)(~(mask_lo & ~0x3U) + 1U);
    return ~( ((uint64_t)mask_hi << 32) | (mask_lo & ~0xFU) ) + 1U;
}

int pci_enable_and_assign_bars(device_t *dev) {
    pci_dev_info_t *info = dev ? dev->plat_data : NULL;
    if (!info)
        return -1;

#ifdef CONFIG_X86_64
    if (!g_pci_mmio_alloc)
        g_pci_mmio_alloc = PCI_MMIO_BASE - PAGE_OFFSET;
#endif

    int res_count = 0;
    for (int bar = 0; bar < 6; bar++) {
        uint32_t offset = 0x10U + (uint32_t)bar * 4U;
        uint32_t bar_lo = pci_ecam_read(info->bus, info->dev, info->func, offset);
        if (bar_lo == 0 || bar_lo == 0xFFFFFFFFU)
            continue;

        int is_io = (bar_lo & 1U) != 0;
        int is_64 = !is_io && ((bar_lo & 0x6U) == 0x4U);
        uint64_t size = pci_bar_size(info, bar, bar_lo);
        if (!size || (size & (size - 1U)) != 0)
            return -1;

        uint64_t addr = is_io ? (bar_lo & ~0x3U) : (bar_lo & ~0xFU);
        if (is_64)
            addr |= (uint64_t)pci_ecam_read(info->bus, info->dev, info->func,
                                             offset + 4U) << 32;

#ifdef CONFIG_X86_64
        if (!is_io && addr == 0) {
            uintptr_t aligned = (g_pci_mmio_alloc + (uintptr_t)size - 1U) &
                                ~((uintptr_t)size - 1U);
            g_pci_mmio_alloc = aligned + (uintptr_t)size;
            pci_ecam_write(info->bus, info->dev, info->func, offset,
                           (uint32_t)(aligned | (bar_lo & 0xFU)));
            if (is_64)
                pci_ecam_write(info->bus, info->dev, info->func, offset + 4U,
                               (uint32_t)(aligned >> 32));
            addr = aligned;
        }
#endif

        if (!is_io && res_count < dev->res_count) {
            dev->res[res_count].type = RES_MMIO;
            dev->res[res_count].start = arch_pci_bar_to_resource(addr);
            dev->res[res_count].end = dev->res[res_count].start + size - 1U;
            dev->res[res_count].flags = is_64 ? IORESOURCE_MMIO_64BIT :
                                                IORESOURCE_MMIO_32BIT;
            res_count++;
        }
        if (is_64)
            bar++;
    }

    uint32_t command = pci_ecam_read(info->bus, info->dev, info->func, 0x04);
    command |= 0x6U; /* PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER */
    pci_ecam_write(info->bus, info->dev, info->func, 0x04, command);
    return 0;
}

void pci_enumerate(uintptr_t ecam_base, int bus_start, int bus_end) {
    g_pci_data.ecam_base = ecam_base;
    g_pci_data.bus_start = bus_start;
    g_pci_data.bus_end   = bus_end;

#ifdef CONFIG_X86_64
    arch_pci_host_init(ecam_base);
#endif

    bus_register(&pci_bus);

    static pci_dev_info_t pci_infos[64];
    static resource_t pci_resources[64][6];
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

                uint32_t subsystem = pci_ecam_read(bus, dev, func, 0x2C);
                info->subvendor = (uint16_t)subsystem;
                info->subdevice = (uint16_t)(subsystem >> 16);

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

                resource_t *res = pci_resources[dev_idx];
                int res_count = 0;
                for (int b = 0; b < info->bar_count && res_count < 6; b++) {
                    res[res_count].type  = RES_MMIO;
                    res[res_count].start = arch_pci_bar_to_resource(info->bar[b]);
                    res[res_count].end   = res[res_count].start + 0xFFF;
                    res[res_count].flags = IORESOURCE_MMIO_32BIT;
                    res_count++;
                }

                if (info->irq != 0 && info->irq != 0xFF && res_count < 6) {
                    res[res_count].type  = RES_IRQ;
                    res[res_count].start = info->irq;
                    res[res_count].end   = info->irq;
                    res[res_count].flags = IORESOURCE_IRQ_LEVEL;
                    res_count++;
                }

                snprintf(pci_names[dev_idx], sizeof(pci_names[dev_idx]),
                         "pci-%04x:%04x-%d", vendor, device_id, dev_idx);

                static device_t pci_devs[64];
                device_t *pdev     = &pci_devs[dev_idx];
                pdev->name          = pci_names[dev_idx];
                pdev->bus           = &pci_bus;
                pdev->plat_data     = info;
                pdev->res           = res;
                pdev->res_count     = res_count;
                pdev->state         = DEV_STATE_UNINIT;

                device_register(pdev);
                dev_idx++;
            }
        }
    }

    kinfo("[BUS] pci: found %d devices (ecam=0x%lx, bus %d-%d)\n",
          dev_idx, (unsigned long)ecam_base, bus_start, bus_end);
}
