/*
 * PCI Bus — device enumeration via ECAM or Type-0/1 config cycles
 *
 * Used by LoongArch QEMU virt and future PC platforms.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/pci_hal.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/block/virtio_blk.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/string.h"

#ifdef CONFIG_X86_64
#include "platform.h"
#endif

#define PCI_ANY_ID          0xFFFFFFFFUL
#define PCI_VENDOR_ID_REDHAT 0x1AF4

#define PCI_MAX_BUS   256
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
    uint64_t bar[6];             /* raw, indexed by PCI BAR number */
    uint64_t bar_size[6];
    int      bar_resource[6];    /* PCI BAR -> device resource, -1 if I/O */
} pci_dev_info_t;

/* Keep the generic PCI layer useful to class drivers without making its
 * private enumeration record part of every driver ABI. */
uint32_t pci_class_code(const device_t *dev) {
    const pci_dev_info_t *info = dev ? (const pci_dev_info_t *)dev->plat_data : NULL;
    if (!info)
        return 0;
    return pci_ecam_read(info->bus, info->dev, info->func, 0x08) >> 8;
}

uint32_t pci_device_id(const device_t *dev) {
    const pci_dev_info_t *info = dev ? (const pci_dev_info_t *)dev->plat_data : NULL;
    if (!info)
        return 0;
    return ((uint32_t)info->vendor << 16) | info->device;
}

#ifdef CONFIG_X86_64
static uintptr_t g_pci_mmio_alloc;
#endif

/* VirtIO modern PCI capability layout (VirtIO 1.0, section 4.1.4.3). */
#define PCI_STATUS_CAP_LIST          0x10U
#define PCI_CAPABILITIES_PTR         0x34U
#define PCI_CAP_ID_VNDR              0x09U
#define VIRTIO_PCI_CAP_COMMON_CFG    1U
#define VIRTIO_PCI_CAP_NOTIFY_CFG    2U
#define VIRTIO_PCI_CAP_ISR_CFG       3U
#define VIRTIO_PCI_CAP_DEVICE_CFG    4U

#define PCOMMON_DEV_FEAT_SEL         0x00U
#define PCOMMON_DEV_FEAT             0x04U
#define PCOMMON_DRV_FEAT_SEL         0x08U
#define PCOMMON_DRV_FEAT             0x0CU
#define PCOMMON_STATUS               0x14U
#define PCOMMON_QUEUE_SEL            0x16U
#define PCOMMON_QUEUE_SIZE           0x18U
#define PCOMMON_QUEUE_ENABLE         0x1CU
#define PCOMMON_QUEUE_DESC_LO        0x20U
#define PCOMMON_QUEUE_DESC_HI        0x24U
#define PCOMMON_QUEUE_DRV_LO         0x28U
#define PCOMMON_QUEUE_DRV_HI         0x2CU
#define PCOMMON_QUEUE_DEV_LO         0x30U
#define PCOMMON_QUEUE_DEV_HI         0x34U
#define PCOMMON_QUEUE_NOTIFY_OFF     0x1EU

typedef struct pci_virtio_transport {
    uintptr_t common;
    uintptr_t notify;
    uintptr_t isr;
    uintptr_t config;
    uint32_t notify_multiplier;
    uint16_t type;
} pci_virtio_transport_t;

static pci_virtio_transport_t g_pci_virtio[32];
static int g_pci_virtio_count;

static uint8_t pci_read8(const pci_dev_info_t *info, uint32_t reg) {
    uint32_t word = pci_ecam_read(info->bus, info->dev, info->func, reg & ~3U);
    return (uint8_t)(word >> ((reg & 3U) * 8U));
}

static uint16_t pci_read16(const pci_dev_info_t *info, uint32_t reg) {
    uint32_t word = pci_ecam_read(info->bus, info->dev, info->func, reg & ~3U);
    return (uint16_t)(word >> ((reg & 2U) * 8U));
}

static int pci_match(device_t *dev, const driver_t *drv) {
    if (drv->bus && drv->bus != dev->bus)
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
    if (!is_64)
        return (uint64_t)(~(mask_lo & ~0xFU) + 1U);
    return ~(((uint64_t)mask_hi << 32) | (mask_lo & ~0xFU)) + 1U;
}

static uint64_t pci_bar_address(const pci_dev_info_t *info, int bar,
                                uint32_t bar_lo) {
    uint64_t address = (uint64_t)(bar_lo & ~0xFU);
    if (!(bar_lo & 1U) && (bar_lo & 0x6U) == 0x4U)
        address |= (uint64_t)pci_ecam_read(info->bus, info->dev, info->func,
                                            0x10U + (uint32_t)(bar + 1) * 4U) << 32;
    return address;
}

int pci_enable_and_assign_bars(device_t *dev) {
    pci_dev_info_t *info = dev ? dev->plat_data : NULL;
    if (!info)
        return -1;

#ifdef CONFIG_X86_64
    if (!g_pci_mmio_alloc)
        g_pci_mmio_alloc = PCI_MMIO_BASE - PAGE_OFFSET;
#endif

    /* BAR sizing writes all ones into the BAR.  Disable address decoding
     * while doing that, as required by PCI, then restore it below. */
    uint32_t command = pci_ecam_read(info->bus, info->dev, info->func, 0x04);
    pci_ecam_write(info->bus, info->dev, info->func, 0x04, command & ~0x3U);

    int res_count = 0;
    for (int bar = 0; bar < 6; bar++)
        info->bar_resource[bar] = -1;
    for (int bar = 0; bar < 6; bar++) {
        uint32_t offset = 0x10U + (uint32_t)bar * 4U;
        uint32_t bar_lo = pci_ecam_read(info->bus, info->dev, info->func, offset);
        if (bar_lo == 0 || bar_lo == 0xFFFFFFFFU)
            continue;

        int is_io = (bar_lo & 1U) != 0;
        int is_64 = !is_io && ((bar_lo & 0x6U) == 0x4U);
        uint64_t size = pci_bar_size(info, bar, bar_lo);
        if (!size || (size & (size - 1U)) != 0) {
            kerr("[PCI] %02x:%02x.%x BAR%d invalid size 0x%lx (raw=0x%x)\n",
                 info->bus, info->dev, info->func, bar,
                 (unsigned long)size, bar_lo);
            pci_ecam_write(info->bus, info->dev, info->func, 0x04, command);
            return -1;
        }

        uint64_t addr = is_io ? (bar_lo & ~0x3U) : pci_bar_address(info, bar, bar_lo);

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

        if (!is_io && res_count < 6) {
            dev->res[res_count].type = RES_MMIO;
            dev->res[res_count].start = arch_pci_bar_to_resource(addr);
            dev->res[res_count].end = dev->res[res_count].start + size - 1U;
            dev->res[res_count].flags = is_64 ? IORESOURCE_MMIO_64BIT :
                                                IORESOURCE_MMIO_32BIT;
            info->bar_resource[bar] = res_count;
            res_count++;
        }
        info->bar[bar] = addr;
        info->bar_size[bar] = size;
        if (is_64)
            bar++;
    }

    command |= 0x6U; /* PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER */
    pci_ecam_write(info->bus, info->dev, info->func, 0x04, command);
    /* Keep the interrupt line visible to drivers after rebuilding the BAR
     * resources.  Modern virtio currently polls, but legacy PCI users may
     * still request INTx. */
    if (info->irq != 0 && info->irq != 0xFF && res_count < 6) {
        dev->res[res_count].type = RES_IRQ;
        dev->res[res_count].start = info->irq;
        dev->res[res_count].end = info->irq;
        dev->res[res_count].flags = IORESOURCE_IRQ_LEVEL;
        res_count++;
    }
    dev->res_count = res_count;
    return 0;
}

resource_t *pci_get_bar_resource(device_t *dev, unsigned int bar) {
    pci_dev_info_t *info = dev ? (pci_dev_info_t *)dev->plat_data : NULL;
    if (!info || bar >= ARRAY_SIZE(info->bar_resource))
        return NULL;
    int resource = info->bar_resource[bar];
    if (resource < 0 || resource >= dev->res_count)
        return NULL;
    return &dev->res[resource];
}

static uint32_t pci_virtio_read32(virtio_transport_t *transport, uint32_t off) {
    pci_virtio_transport_t *vt = (pci_virtio_transport_t *)transport->priv;
    if (!vt)
        return 0;

    switch (off) {
    case VIRTIO_MMIO_MAGIC: return 0x74726976U;
    case VIRTIO_MMIO_VERSION: return 2U;
    case VIRTIO_MMIO_DEVICE_ID: return vt->type;
    case VIRTIO_MMIO_DEVICE_FEATURES: return readl((const volatile void *)(vt->common + PCOMMON_DEV_FEAT));
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL: return readl((const volatile void *)(vt->common + PCOMMON_DEV_FEAT_SEL));
    case VIRTIO_MMIO_DRIVER_FEATURES: return readl((const volatile void *)(vt->common + PCOMMON_DRV_FEAT));
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL: return readl((const volatile void *)(vt->common + PCOMMON_DRV_FEAT_SEL));
    case VIRTIO_MMIO_QUEUE_NUM_MAX: return readw((const volatile void *)(vt->common + PCOMMON_QUEUE_SIZE));
    case VIRTIO_MMIO_QUEUE_READY: return readw((const volatile void *)(vt->common + PCOMMON_QUEUE_ENABLE));
    case VIRTIO_MMIO_STATUS: return readb((const volatile void *)(vt->common + PCOMMON_STATUS));
    case VIRTIO_MMIO_QUEUE_DESC_LOW: return readl((const volatile void *)(vt->common + PCOMMON_QUEUE_DESC_LO));
    case VIRTIO_MMIO_QUEUE_DESC_HIGH: return readl((const volatile void *)(vt->common + PCOMMON_QUEUE_DESC_HI));
    case VIRTIO_MMIO_QUEUE_DRIVER_LOW: return readl((const volatile void *)(vt->common + PCOMMON_QUEUE_DRV_LO));
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH: return readl((const volatile void *)(vt->common + PCOMMON_QUEUE_DRV_HI));
    case VIRTIO_MMIO_QUEUE_DEVICE_LOW: return readl((const volatile void *)(vt->common + PCOMMON_QUEUE_DEV_LO));
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH: return readl((const volatile void *)(vt->common + PCOMMON_QUEUE_DEV_HI));
    case VIRTIO_MMIO_INTERRUPT_STATUS: return vt->isr ? readb((const volatile void *)vt->isr) : 0;
    default:
        if (off >= VIRTIO_MMIO_CONFIG && vt->config)
            return readl((const volatile void *)(vt->config + off - VIRTIO_MMIO_CONFIG));
        return 0;
    }
}

static void pci_virtio_write32(virtio_transport_t *transport, uint32_t off,
                               uint32_t value) {
    pci_virtio_transport_t *vt = (pci_virtio_transport_t *)transport->priv;
    if (!vt)
        return;

    switch (off) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL: writel(value, (volatile void *)(vt->common + PCOMMON_DEV_FEAT_SEL)); break;
    case VIRTIO_MMIO_DRIVER_FEATURES: writel(value, (volatile void *)(vt->common + PCOMMON_DRV_FEAT)); break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL: writel(value, (volatile void *)(vt->common + PCOMMON_DRV_FEAT_SEL)); break;
    case VIRTIO_MMIO_STATUS: writeb((uint8_t)value, (volatile void *)(vt->common + PCOMMON_STATUS)); break;
    case VIRTIO_MMIO_QUEUE_SEL: writew((uint16_t)value, (volatile void *)(vt->common + PCOMMON_QUEUE_SEL)); break;
    case VIRTIO_MMIO_QUEUE_NUM: writew((uint16_t)value, (volatile void *)(vt->common + PCOMMON_QUEUE_SIZE)); break;
    case VIRTIO_MMIO_QUEUE_READY: writew((uint16_t)value, (volatile void *)(vt->common + PCOMMON_QUEUE_ENABLE)); break;
    case VIRTIO_MMIO_QUEUE_DESC_LOW: writel(value, (volatile void *)(vt->common + PCOMMON_QUEUE_DESC_LO)); break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH: writel(value, (volatile void *)(vt->common + PCOMMON_QUEUE_DESC_HI)); break;
    case VIRTIO_MMIO_QUEUE_DRIVER_LOW: writel(value, (volatile void *)(vt->common + PCOMMON_QUEUE_DRV_LO)); break;
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH: writel(value, (volatile void *)(vt->common + PCOMMON_QUEUE_DRV_HI)); break;
    case VIRTIO_MMIO_QUEUE_DEVICE_LOW: writel(value, (volatile void *)(vt->common + PCOMMON_QUEUE_DEV_LO)); break;
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH: writel(value, (volatile void *)(vt->common + PCOMMON_QUEUE_DEV_HI)); break;
    case VIRTIO_MMIO_QUEUE_NOTIFY: {
        writew((uint16_t)value, (volatile void *)(vt->common + PCOMMON_QUEUE_SEL));
        uint16_t notify_off = readw((const volatile void *)(vt->common + PCOMMON_QUEUE_NOTIFY_OFF));
        writew((uint16_t)value, (volatile void *)(vt->notify +
               (uintptr_t)notify_off * vt->notify_multiplier));
        break;
    }
    default: break;
    }
}

int pci_virtio_transport_init(device_t *dev, int type,
                              virtio_transport_t *transport) {
    pci_dev_info_t *info = dev ? (pci_dev_info_t *)dev->plat_data : NULL;
    if (!info || !transport || g_pci_virtio_count >= (int)ARRAY_SIZE(g_pci_virtio))
        return -1;
    if (pci_enable_and_assign_bars(dev) != 0) {
        kerr("[VIRTIO-PCI] %s: BAR setup failed (type=%d)\n", dev->name, type);
        return -1;
    }
    if (!(pci_read16(info, 0x06) & PCI_STATUS_CAP_LIST)) {
        kerr("[VIRTIO-PCI] %s: no PCI capability list (type=%d)\n",
             dev->name, type);
        return -1;
    }

    pci_virtio_transport_t candidate = { .type = (uint16_t)type };
    int found = 0;
    uint8_t ptr = pci_read8(info, PCI_CAPABILITIES_PTR) & 0xFCU;
    for (int limit = 0; ptr && limit < 48; limit++) {
        uint32_t cap = pci_ecam_read(info->bus, info->dev, info->func, ptr);
        uint8_t next = (uint8_t)(cap >> 8) & 0xFCU;
        if ((uint8_t)cap == PCI_CAP_ID_VNDR) {
            uint8_t cfg_type = (uint8_t)(cap >> 24);
            uint8_t bar = pci_read8(info, (uint32_t)ptr + 4U);
            uint32_t offset = pci_ecam_read(info->bus, info->dev, info->func,
                                             (uint32_t)ptr + 8U);
            if (bar < 6 && info->bar[bar] && info->bar_size[bar] &&
                (uint64_t)offset < info->bar_size[bar]) {
                uint32_t bar_lo = pci_ecam_read(info->bus, info->dev, info->func,
                                                 0x10U + (uint32_t)bar * 4U);
                uintptr_t base = arch_pci_bar_to_resource(
                    pci_bar_address(info, bar, bar_lo));
                switch (cfg_type) {
                case VIRTIO_PCI_CAP_COMMON_CFG: candidate.common = base + offset; found |= 1; break;
                case VIRTIO_PCI_CAP_NOTIFY_CFG:
                    candidate.notify = base + offset;
                    candidate.notify_multiplier = pci_ecam_read(info->bus, info->dev,
                                                                 info->func, (uint32_t)ptr + 16U);
                    found |= 2;
                    break;
                case VIRTIO_PCI_CAP_ISR_CFG: candidate.isr = base + offset; break;
                case VIRTIO_PCI_CAP_DEVICE_CFG: candidate.config = base + offset; found |= 4; break;
                default: break;
                }
            }
        }
        if (next == ptr)
            break;
        ptr = next;
    }
    if ((found & 7) != 7 || !candidate.notify_multiplier) {
        kerr("[VIRTIO-PCI] %s: incomplete capabilities type=%d found=0x%x notify-mult=%u cap-ptr=0x%x\n",
             dev->name, type, found, candidate.notify_multiplier,
             pci_read8(info, PCI_CAPABILITIES_PTR));
        return -1;
    }

    g_pci_virtio[g_pci_virtio_count] = candidate;
    transport->read32 = pci_virtio_read32;
    transport->write32 = pci_virtio_write32;
    transport->priv = &g_pci_virtio[g_pci_virtio_count++];
    transport->legacy = 0;
    transport->irq = -1; /* Polling is reliable until VBox MSI/INTx routing is described. */
    kinfo("[VIRTIO-PCI] %s: common=0x%lx notify=0x%lx isr=0x%lx config=0x%lx mult=%u\n",
          dev->name, (unsigned long)candidate.common,
          (unsigned long)candidate.notify, (unsigned long)candidate.isr,
          (unsigned long)candidate.config, candidate.notify_multiplier);
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

    static pci_dev_info_t pci_infos[128];
    /* Six MMIO BARs plus an INTx resource. */
    static resource_t pci_resources[128][7];
    static char pci_names[128][32];
    int dev_idx = 0;

    for (int bus = bus_start; bus < bus_end && dev_idx < 128; bus++) {
        for (int dev = 0; dev < PCI_MAX_DEV && dev_idx < 128; dev++) {
            for (int func = 0; func < PCI_MAX_FUNC && dev_idx < 128; func++) {
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

                uint32_t class_rev = pci_ecam_read(bus, dev, func, 0x08);

                for (int b = 0; b < 6; b++) {
                    uint32_t bar_lo = pci_ecam_read(bus, dev, func, 0x10 + b * 4);
                    info->bar[b] = (bar_lo == 0xFFFFFFFF || bar_lo == 0) ? 0 :
                                   pci_bar_address(info, b, bar_lo);
                    info->bar_size[b] = 0;
                    info->bar_resource[b] = -1;
                }

                resource_t *res = pci_resources[dev_idx];
                memset(res, 0, sizeof(pci_resources[dev_idx]));

                snprintf(pci_names[dev_idx], sizeof(pci_names[dev_idx]),
                         "pci-%04x:%04x-%d", vendor, device_id, dev_idx);

                static device_t pci_devs[128];
                device_t *pdev     = &pci_devs[dev_idx];
                pdev->name          = pci_names[dev_idx];
                pdev->bus           = &pci_bus;
                pdev->plat_data     = info;
                pdev->res           = res;
                pdev->res_count     = 0;
                pdev->state         = DEV_STATE_UNINIT;

                device_register(pdev);
                kinfo("[BUS] pci %02x:%02x.%x id=%04x:%04x sub=%04x:%04x class=%02x:%02x:%02x irq=%u\n",
                      bus, dev, func, vendor, device_id,
                      info->subvendor, info->subdevice,
                      (unsigned int)(class_rev >> 24),
                      (unsigned int)((class_rev >> 16) & 0xffU),
                      (unsigned int)((class_rev >> 8) & 0xffU), info->irq);
                for (int b = 0; b < 6; b++) {
                    if (info->bar[b])
                        kinfo("[BUS]   BAR%d: phys=0x%lx\n", b,
                              (unsigned long)info->bar[b]);
                }
                dev_idx++;
            }
        }
    }

    kinfo("[BUS] pci: found %d devices (ecam=0x%lx, bus %d-%d)\n",
          dev_idx, (unsigned long)ecam_base, bus_start, bus_end);
}
