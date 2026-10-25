/*
 * PCI Bus — device enumeration via ECAM or Type-0/1 config cycles
 *
 * Used by LoongArch QEMU virt and future PC platforms.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/pci_hal.h"
#include "drivers/bus/pci_msix.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/block/virtio_blk.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/string.h"
#include "mm/slab.h"

#include "core/errno.h"

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
static int g_pci_initialized;

/* Config-space access goes through the arch HAL, which decides between an ECAM
 * memory window and the legacy 0xCF8/0xCFC port pair once per access by probing
 * for a host bridge at 00:00.0.
 *
 * This used to compute the ECAM address here and read it directly, which made
 * the legacy path in pci_host.c unreachable: on a machine with no ECAM window at
 * all (i440fx) every read returned zero, enumeration published one phantom
 * device per slot, and nothing was ever found.  The arch layer exists precisely
 * so that question is answered in one place. */
static uint32_t pci_ecam_read(int bus, int dev, int func, uint32_t reg) {
    return arch_pci_config_read32(bus, dev, func, reg);
}

static void pci_ecam_write(int bus, int dev, int func, uint32_t reg, uint32_t val) {
    arch_pci_config_write32(bus, dev, func, reg, val);
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
    uint8_t bar_io[6];
    int      bar_resource[6];    /* PCI BAR -> published MMIO/port resource */
} pci_dev_info_t;

#define PCI_MAX_PUBLISHED_DEVICES 128
static pci_dev_info_t g_pci_infos[PCI_MAX_PUBLISHED_DEVICES];
static resource_t g_pci_resources[PCI_MAX_PUBLISHED_DEVICES][7];
static char g_pci_names[PCI_MAX_PUBLISHED_DEVICES][32];
static device_t g_pci_devs[PCI_MAX_PUBLISHED_DEVICES];
static uint8_t g_pci_in_use[PCI_MAX_PUBLISHED_DEVICES];

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

int pci_device_location(const device_t *dev, unsigned *bus,
                        unsigned *slot, unsigned *function) {
    if (!dev || dev->bus != &pci_bus || !dev->plat_data)
        return -ENODEV;
    const pci_dev_info_t *info = (const pci_dev_info_t *)dev->plat_data;
    if (bus) *bus = info->bus;
    if (slot) *slot = info->dev;
    if (function) *function = info->func;
    return 0;
}

/* Resolve the INTx interrupt line for an enumerated PCI function through the
 * platform routing hook (arch_pci_intx_irq).  Returns -1 when the function
 * has no interrupt pin or the platform provides no routing for it; callers
 * must keep their completion-polling fallback in that case. */
int pci_intx_irq(const device_t *dev) {
    const pci_dev_info_t *info = dev ? (const pci_dev_info_t *)dev->plat_data : NULL;
    if (!info)
        return -1;
    uint8_t pin = (uint8_t)(pci_ecam_read(info->bus, info->dev, info->func, 0x3C) >> 8) & 0x7U;
    if (!pin)
        return -1;
    return arch_pci_intx_irq(info->bus, info->dev, info->func, (int)pin);
}

static uint64_t g_pci_mmio_alloc;

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
#define PCOMMON_QUEUE_MSIX_VECTOR    0x1AU
#define PCOMMON_QUEUE_DESC_LO        0x20U
#define PCOMMON_QUEUE_DESC_HI        0x24U
#define PCOMMON_QUEUE_DRV_LO         0x28U
#define PCOMMON_QUEUE_DRV_HI         0x2CU
#define PCOMMON_QUEUE_DEV_LO         0x30U
#define PCOMMON_QUEUE_DEV_HI         0x34U
#define PCOMMON_QUEUE_NOTIFY_OFF     0x1EU

/* QueueNotifyOff is fixed for the lifetime of the device, but reading it back
 * costs a 16-bit MMIO round trip on every kick -- and the kick is the one
 * register write a virtio driver performs per buffer, so this is a per-packet
 * round trip on virtio-net and a per-request one on virtio-blk.  Cache it per
 * queue index: the first kick for a queue still reads it, and every later one
 * is a bare store to the doorbell.  A queue index outside the cache falls back
 * to the read, so an unusually wide device is slower rather than wrong. */
#define PCI_VIRTIO_NOTIFY_CACHE 8

typedef struct pci_virtio_transport {
    uintptr_t common;
    uintptr_t notify;
    uintptr_t isr;
    uintptr_t config;
    uint32_t notify_multiplier;
    uint16_t type;
    /* Bit i set: notify_off_cache[i] holds queue i's QueueNotifyOff. */
    uint32_t notify_off_cached;
    uint16_t notify_off_cache[PCI_VIRTIO_NOTIFY_CACHE];
    device_t   *device;       /* the function this transport describes */
    unsigned queues_selected;  /* queues given a vector by msix_prepare() */
} pci_virtio_transport_t;


static uint8_t pci_read8(const pci_dev_info_t *info, uint32_t reg) {
    uint32_t word = pci_ecam_read(info->bus, info->dev, info->func, reg & ~3U);
    return (uint8_t)(word >> ((reg & 3U) * 8U));
}

static uint16_t pci_read16(const pci_dev_info_t *info, uint32_t reg) {
    uint32_t word = pci_ecam_read(info->bus, info->dev, info->func, reg & ~3U);
    return (uint16_t)(word >> ((reg & 2U) * 8U));
}

uint8_t pci_cfg_read8(const device_t *dev, uint32_t reg) {
    const pci_dev_info_t *info = dev ? (const pci_dev_info_t *)dev->plat_data : NULL;
    return info ? pci_read8(info, reg) : 0;
}

uint16_t pci_cfg_read16(const device_t *dev, uint32_t reg) {
    const pci_dev_info_t *info = dev ? (const pci_dev_info_t *)dev->plat_data : NULL;
    return info ? pci_read16(info, reg) : 0;
}

uint32_t pci_cfg_read32(const device_t *dev, uint32_t reg) {
    const pci_dev_info_t *info = dev ? (const pci_dev_info_t *)dev->plat_data : NULL;
    return info ? pci_ecam_read(info->bus, info->dev, info->func, reg) : 0;
}

void pci_cfg_write16(const device_t *dev, uint32_t reg, uint16_t val) {
    const pci_dev_info_t *info = dev ? (const pci_dev_info_t *)dev->plat_data : NULL;
    if (!info)
        return;
    uint32_t offset = reg & ~3U;
    uint32_t word = pci_ecam_read(info->bus, info->dev, info->func, offset);
    /* Command shares a dword with W1C Status: do not replay latched bits. */
    if (offset == 0x04 && !(reg & 2U)) word &= 0xffffU;
    uint32_t shift = (reg & 2U) * 8U;
    word &= ~(0xFFFFU << shift);
    word |= (uint32_t)val << shift;
    pci_ecam_write(info->bus, info->dev, info->func, offset, word);
}

void pci_cfg_write32(const device_t *dev, uint32_t reg, uint32_t val) {
    const pci_dev_info_t *info = dev ? (const pci_dev_info_t *)dev->plat_data : NULL;
    if (!info)
        return;
    pci_ecam_write(info->bus, info->dev, info->func, reg, val);
}

/* CONFIG_CAP_WALK_MODEL:
 * - The capability list is a singly linked list of dword-aligned headers, each
 *   byte 0 the capability ID and byte 1 the next offset in its low six bits.
 * - Two guards bound the walk: a hard iteration limit, and the self-loop test
 *   below.  A cycle through two or more entries is not covered by that test,
 *   which is why the limit exists; without it a firmware bug would spin here
 *   with no other work to do. */
#define PCI_CAP_WALK_LIMIT 48

uint8_t pci_find_capability(const device_t *dev, uint8_t cap_id) {
    const pci_dev_info_t *info = dev ? (const pci_dev_info_t *)dev->plat_data : NULL;
    if (!info || !(pci_read16(info, 0x06) & PCI_STATUS_CAP_LIST))
        return 0;

    uint8_t ptr = pci_read8(info, PCI_CAPABILITIES_PTR) & 0xFCU;
    for (int limit = 0; ptr && limit < PCI_CAP_WALK_LIMIT; limit++) {
        uint32_t cap = pci_ecam_read(info->bus, info->dev, info->func, ptr);
        if ((uint8_t)(cap & 0xFFU) == cap_id)
            return ptr;
        uint8_t next = (uint8_t)(cap >> 8) & 0xFCU;
        if (next == ptr)
            break;
        ptr = next;
    }
    return 0;
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
            (id->subdevice == DEVICE_ANY || id->subdevice == info->subdevice)) {
            dev->matched_id = id;
            return 1;
        }
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

static uint64_t pci_bar_size(const pci_dev_info_t *info, int bar,
                             uint32_t bar_lo, uint32_t *bar_flags) {
    uint32_t offset = 0x10U + (uint32_t)bar * 4U;
    uint32_t original_hi = 0;
    pci_ecam_write(info->bus, info->dev, info->func, offset, 0xFFFFFFFFU);
    uint32_t mask_lo = pci_ecam_read(info->bus, info->dev, info->func, offset);
    uint32_t flags = bar_lo ? bar_lo : mask_lo;
    int is_io = (flags & 1U) != 0;
    int is_64 = !is_io && ((flags & 0x6U) == 0x4U);

    if (bar_flags)
        *bar_flags = flags;
    if (is_64) {
        unsigned header = (pci_ecam_read(info->bus, info->dev, info->func, 0x0c) >> 16) & 0x7f;
        int count = header == 0 ? 6 : header == 1 ? 2 : 0;
        if (bar + 1 >= count) {
            pci_ecam_write(info->bus, info->dev, info->func, offset, bar_lo);
            return 0;
        }
        original_hi = pci_ecam_read(info->bus, info->dev, info->func, offset + 4U);
        pci_ecam_write(info->bus, info->dev, info->func, offset + 4U,
                       0xFFFFFFFFU);
    }
    uint32_t mask_hi = is_64 ?
        pci_ecam_read(info->bus, info->dev, info->func, offset + 4U) : 0;

    pci_ecam_write(info->bus, info->dev, info->func, offset, bar_lo);
    if (is_64)
        pci_ecam_write(info->bus, info->dev, info->func, offset + 4U, original_hi);

    if (is_io)
        return (uint64_t)(~(mask_lo & ~0x3U) + 1U);
    if (!is_64)
        return (uint64_t)(~(mask_lo & ~0xFU) + 1U);
    return ~(((uint64_t)mask_hi << 32) | (mask_lo & ~0xFU)) + 1U;
}

static uint64_t pci_bar_address(const pci_dev_info_t *info, int bar,
                                uint32_t bar_lo, int is_64) {
    uint64_t address = (uint64_t)(bar_lo & ~0xFU);
    if (is_64)
        address |= (uint64_t)pci_ecam_read(info->bus, info->dev, info->func,
                                             0x10U + (uint32_t)(bar + 1) * 4U) << 32;
    return address;
}

/* Allocation policy is host-owned. Avoid every published firmware BAR, even
 * when no driver has bound that function yet. The cursor is monotonically
 * consumed; exhausting the declared host window is an explicit error. */
static int pci_allocate_mmio(uint64_t size, int is_64, uint64_t *address) {
    uint64_t base, length;
    int rc = arch_pci_mmio_window(&base, &length);
    if (rc) return rc;
    if (!size || !length || length > (~(uint64_t)0) - base) return -ERANGE;
    uint64_t cursor = g_pci_mmio_alloc > base ? g_pci_mmio_alloc : base;
    for (unsigned attempt = 0; attempt <= PCI_MAX_PUBLISHED_DEVICES * 6U; attempt++) {
        if (cursor > (~(uint64_t)0) - (size - 1U)) return -ERANGE;
        uint64_t aligned = (cursor + size - 1U) & ~(size - 1U);
        if (aligned < base || aligned > base + length || size > base + length - aligned ||
            (!is_64 && (aligned > (~(uint32_t)0) || size - 1U > (~(uint32_t)0) - aligned)))
            return -ERANGE;
        uint64_t next = aligned;
        for (unsigned slot = 0; slot < PCI_MAX_PUBLISHED_DEVICES; slot++) {
            if (!g_pci_in_use[slot]) continue;
            const pci_dev_info_t *info = &g_pci_infos[slot];
            for (int i = 0; i < 6; i++) {
                uint64_t address = info->bar[i], length = info->bar_size[i];
                if (info->bar_io[i] || !address || !length) continue;
                if (length - 1U > (~(uint64_t)0) - address) return -ERANGE;
                uint64_t end = address + length - 1U;
                if (aligned <= end && address <= aligned + size - 1U) {
                    if (end == (~(uint64_t)0)) return -ERANGE;
                    if (end + 1U > next) next = end + 1U;
                }
            }
        }
        if (next != aligned) { cursor = next; continue; }
        if (!arch_pci_bar_reachable(aligned, size)) return -ERANGE;
        g_pci_mmio_alloc = aligned + size;
        *address = aligned;
        return 0;
    }
    return -ENOSPC;
}

static int pci_assign_bars(device_t *dev, int bus_master) {
    pci_dev_info_t *info = dev ? dev->plat_data : NULL;
    if (!info)
        return -1;

    unsigned header = (pci_ecam_read(info->bus, info->dev, info->func, 0x0c) >> 16) & 0x7f;
    int bar_count = header == 0 ? 6 : header == 1 ? 2 : 0;
    if (!bar_count) return -EOPNOTSUPP;

    /* BAR sizing writes all ones into the BAR.  Disable address decoding
     * while doing that, as required by PCI, then restore it below. */
    uint32_t command = pci_ecam_read(info->bus, info->dev, info->func, 0x04) & 0xffffU;
    pci_ecam_write(info->bus, info->dev, info->func, 0x04, command & ~0x3U);

    int res_count = 0;
    for (int bar = 0; bar < 6; bar++)
        info->bar_resource[bar] = -1;
    for (int bar = 0; bar < bar_count; bar++) {
        uint32_t offset = 0x10U + (uint32_t)bar * 4U;
        uint32_t bar_lo = pci_ecam_read(info->bus, info->dev, info->func, offset);
        if (bar_lo == 0xFFFFFFFFU)
            continue;

        uint32_t bar_flags = 0;
        uint64_t size = pci_bar_size(info, bar, bar_lo, &bar_flags);
        if (!size && bar_lo == 0)
            continue;
        if (!size || (size & (size - 1U)) != 0) {
            kerr("[PCI] %02x:%02x.%x BAR%d invalid size 0x%lx (raw=0x%x)\n",
                 info->bus, info->dev, info->func, bar,
                 (unsigned long)size, bar_lo);
            pci_ecam_write(info->bus, info->dev, info->func, 0x04, command);
            return -1;
        }

        int is_io = (bar_flags & 1U) != 0;
        int is_64 = !is_io && ((bar_flags & 0x6U) == 0x4U);
        uint64_t addr = is_io ? (bar_lo & ~0x3U) :
                                pci_bar_address(info, bar, bar_lo, is_64);

        if (!is_io && (!addr || !arch_pci_bar_reachable(addr, size))) {
            uint64_t aligned;
            int rc = pci_allocate_mmio(size, is_64, &aligned);
            if (rc) {
                kerr("[PCI] %02x:%02x.%x BAR%d has no usable host MMIO allocation: %d\n",
                     info->bus, info->dev, info->func, bar, rc);
                pci_ecam_write(info->bus, info->dev, info->func, 0x04, command);
                return rc;
            }
            pci_ecam_write(info->bus, info->dev, info->func, offset,
                           (uint32_t)(aligned | (bar_flags & 0xFU)));
            if (is_64)
                pci_ecam_write(info->bus, info->dev, info->func, offset + 4U,
                               (uint32_t)(aligned >> 32));
            addr = aligned;
        }
        if (!is_io && size - 1U > (~(uint64_t)0) - addr) {
            pci_ecam_write(info->bus, info->dev, info->func, 0x04, command);
            return -ERANGE;
        }

        if (addr && res_count < 6) {
            dev->res[res_count].type = is_io ? RES_IOPORT : RES_MMIO;
            dev->res[res_count].start = addr;
            dev->res[res_count].end = dev->res[res_count].start + size - 1U;
            dev->res[res_count].flags = is_io ? 0 : (is_64 ? IORESOURCE_MMIO_64BIT :
                                                IORESOURCE_MMIO_32BIT) |
                                                ((bar_flags & 8U) ? IORESOURCE_PREFETCH : 0);
            info->bar_resource[bar] = res_count;
            res_count++;
            if (is_io) command |= 1U;
        }
        info->bar[bar] = addr;
        info->bar_size[bar] = size;
        info->bar_io[bar] = is_io;
        if (is_64)
            bar++;
    }

    command |= 0x2U; /* PCI_COMMAND_MEMORY */
    if (bus_master)
        command |= 0x4U;
    else
        command &= ~0x4U;
    pci_ecam_write(info->bus, info->dev, info->func, 0x04, command);
    /* Only a host-routed driver IRQ is a usable resource. The PCI Interrupt
     * Line byte is firmware metadata, never an architecture IRQ identifier. */
    int irq = pci_intx_irq(dev);
    if (irq >= 0 && res_count < 7) {
        dev->res[res_count].type = RES_IRQ;
        dev->res[res_count].start = (uint32_t)irq;
        dev->res[res_count].end = (uint32_t)irq;
        dev->res[res_count].flags = IORESOURCE_IRQ_LEVEL;
        res_count++;
    }
    dev->res_count = res_count;
    return 0;
}

int pci_enable_and_assign_bars(device_t *dev) {
    return pci_assign_bars(dev, 1);
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

void *pci_map_bar(device_t *dev, unsigned int bar) {
    return device_map_resource(dev, pci_get_bar_resource(dev, bar));
}

int pci_user_device_find_index(uint16_t vendor, uint16_t device,
                               unsigned index, pci_user_device_info_t *out)
{
    if (!out)
        return -1;
    unsigned seen = 0;
    for (int i = 0; i < PCI_MAX_PUBLISHED_DEVICES; i++) {
        pci_dev_info_t *info = &g_pci_infos[i];
        if (!g_pci_in_use[i] || info->vendor != vendor ||
            info->device != device)
            continue;
        if (seen++ != index)
            continue;
        if (pci_assign_bars(&g_pci_devs[i], 0) != 0 ||
            !info->bar[0] || !info->bar_size[0])
            return -1;
        out->vendor = vendor;
        out->device = device;
        out->devid = ((uint16_t)info->bus << 8) |
                     ((uint16_t)info->dev << 3) | info->func;
        int irq = pci_intx_irq(&g_pci_devs[i]);
        out->irq = irq < 0 ? 0xffffu : (uint16_t)irq;
        out->bar0_phys = info->bar[0];
        out->bar0_size = info->bar_size[0];
        return 0;
    }
    return -1;
}

int pci_user_device_find(uint16_t vendor, uint16_t device,
                         pci_user_device_info_t *out)
{
    return pci_user_device_find_index(vendor, device, 0, out);
}

int pci_user_device_bus_master(uint16_t devid, int enable)
{
    uint8_t bus = (uint8_t)(devid >> 8);
    uint8_t dev = (uint8_t)((devid >> 3) & 0x1f);
    uint8_t func = (uint8_t)(devid & 0x7);
    for (int i = 0; i < PCI_MAX_PUBLISHED_DEVICES; i++) {
        pci_dev_info_t *info = &g_pci_infos[i];
        if (!g_pci_in_use[i] || info->bus != bus || info->dev != dev ||
            info->func != func)
            continue;
        uint32_t command = pci_ecam_read(bus, dev, func, 0x04) & 0xffffu;
        if (enable)
            command |= 0x4u;
        else
            command &= ~0x4u;
        pci_ecam_write(bus, dev, func, 0x04, command);
        return 0;
    }
    return -1;
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
        uint16_t qidx = (uint16_t)value;
        uint16_t notify_off;
        if (qidx < PCI_VIRTIO_NOTIFY_CACHE &&
            (vt->notify_off_cached & (1U << qidx))) {
            notify_off = vt->notify_off_cache[qidx];
        } else {
            /* Select only to discover the immutable per-queue offset. A
             * cached kick needs just the doorbell, which KVM can handle via
             * ioeventfd without emulating the common-config register. */
            writew(qidx, (volatile void *)(vt->common + PCOMMON_QUEUE_SEL));
            notify_off = readw((const volatile void *)(vt->common +
                           PCOMMON_QUEUE_NOTIFY_OFF));
            if (qidx < PCI_VIRTIO_NOTIFY_CACHE) {
                vt->notify_off_cache[qidx] = notify_off;
                vt->notify_off_cached |= 1U << qidx;
            }
        }
        writew((uint16_t)value, (volatile void *)(vt->notify +
               (uintptr_t)notify_off * vt->notify_multiplier));
        break;
    }
    default:
        if (off >= VIRTIO_MMIO_CONFIG && vt->config)
            writel(value, (volatile void *)(vt->config + off - VIRTIO_MMIO_CONFIG));
        break;
    }
}

/* Weak default: platforms without wired PCI INTx routing return -1 and the
 * virtio transports keep completion polling.  Overridden by loongarch64. */
int __attribute__((weak)) arch_pci_intx_irq(int bus, int dev, int func, int pin)
{
    (void)bus;
    (void)dev;
    (void)func;
    (void)pin;
    return -1;
}

/* Message-signalled interrupts for one virtio function.  Queue i is bound to
 * vector msix_base + i, which is the whole point of MSI-X over a shared INTx
 * line: one queue no longer contends with another for the same line. */
static int pci_virtio_msix_prepare(virtio_transport_t *t, unsigned vectors)
{
    pci_virtio_transport_t *vt = (pci_virtio_transport_t *)t->priv;
    device_t *dev = vt->device;
    if (!dev)
        return -EINVAL;

    pci_msix_info_t info;
    if (pci_msix_capability(dev, &info) < 0)
        return -ENODEV;
    /* Never take more vectors than there are queues: the driver asks for one
     * per queue, and a function with a shorter table would otherwise have the
     * extra table entries armed with no queue behind them. */
    if (vectors > info.table_size)
        vectors = info.table_size;

    /* The function's own capability states where its table lives, in either
     * of the two fields a device may publish it in; pci_msix_enable() takes
     * that as authoritative and refuses the case where neither does.  Nothing
     * here needs to know the layout, which is the point: the virtio-pci BAR
     * that holds the table is an implementation choice, not part of the
     * virtio specification, and a driver that hard-coded it would program the
     * wrong window on any implementation that chose differently. */
    int r = pci_msix_enable(dev, vectors);
    if (r) {
        kinfo("[VIRTIO-PCI] %s: MSI-X unavailable (%d); using legacy "
              "interrupts\n", dev->name, r);
        return r;
    }

    int base = irq_alloc_vectors(vectors);
    if (base < 0) {
        pci_msix_disable(dev);
        return base;
    }

    for (unsigned i = 0; i < vectors; i++) {
        r = pci_msix_program_vector(dev, i, (uint32_t)(base + (int)i));
        if (r) {
            irq_free_vectors((uint32_t)base, vectors);
            pci_msix_disable(dev);
            return r;
        }
        /* queue_msix_vector lives in common config at 0x1A and names which
         * table entry the queue posts through -- it is an index into the
         * table, not the interrupt line's number, so it can only be written
         * with the queue selected.  The line the entry delivers on is the one
         * already programmed into that entry above; putting the line number
         * here instead would point the queue past the end of the table. */
        writew((uint16_t)i, (volatile void *)(vt->common + PCOMMON_QUEUE_SEL));
        writew((uint16_t)i,
               (volatile void *)(vt->common + PCOMMON_QUEUE_MSIX_VECTOR));
    }
    /* Leave the device interrupt-free until the driver has handlers on every
     * reserved line. */
    vt->queues_selected = vectors;

    t->msix_base = base;
    t->msix_vectors = (int)vectors;
    kinfo("[VIRTIO-PCI] %s: MSI-X reserved, vectors %d..%d for %u queue(s)\n",
          dev->name, base, base + (int)vectors - 1, vectors);
    return 0;
}

static void pci_virtio_msix_arm(virtio_transport_t *t)
{
    pci_virtio_transport_t *vt = (pci_virtio_transport_t *)t->priv;
    device_t *dev = vt->device;
    if (!dev || t->msix_vectors <= 0)
        return;

    for (unsigned i = 0; i < vt->queues_selected; i++)
        pci_msix_set_vector_mask(dev, i, 0);
    (void)pci_msix_commit(dev);
}

static void pci_virtio_msix_teardown(virtio_transport_t *t)
{
    pci_virtio_transport_t *vt = (pci_virtio_transport_t *)t->priv;
    device_t *dev = vt->device;
    if (!dev || t->msix_vectors <= 0)
        return;
    pci_msix_disable(dev);
    irq_free_vectors((uint32_t)t->msix_base, (unsigned)t->msix_vectors);
    t->msix_base = -1;
    t->msix_vectors = 0;
}

int pci_virtio_transport_init(device_t *dev, int type,
                              virtio_transport_t *transport) {
    pci_dev_info_t *info = dev ? (pci_dev_info_t *)dev->plat_data : NULL;
    if (!info || !transport)
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

    pci_virtio_transport_t candidate = { .type = (uint16_t)type, .device = dev };
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
            /* Only these types describe MMIO regions used by this transport.
             * PCI_CFG (type 5) is an optional configuration-space access
             * mechanism: its writable BAR/offset/length may initially be zero.
             * Unknown capability types must likewise be skipped. */
            if (cfg_type >= VIRTIO_PCI_CAP_COMMON_CFG &&
                cfg_type <= VIRTIO_PCI_CAP_DEVICE_CFG &&
                bar < 6 && info->bar[bar] && info->bar_size[bar] &&
                (uint64_t)offset < info->bar_size[bar]) {
                uint32_t length = pci_ecam_read(info->bus, info->dev, info->func,
                                                  (uint32_t)ptr + 12U);
                uint8_t cap_length = (uint8_t)(cap >> 16);
                if (cap_length < 16 || !length || length > info->bar_size[bar] - offset ||
                    (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG && length < 56) ||
                    (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG && (cap_length < 20 || length < 2)))
                    return -EINVAL;
                uintptr_t base = (uintptr_t)pci_map_bar(dev, bar);
                if (!base) return -EIO;
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

    /* Binding storage follows the driver-core remove -> devres ordering.
     * Repeated remove/reprobe must not consume a permanent transport slot. */
    pci_virtio_transport_t *owned = kmalloc(sizeof(*owned));
    if (!owned)
        return -ENOMEM;
    *owned = candidate;
    int ret = device_add_resource_action(dev, kfree, owned);
    if (ret) {
        kfree(owned);
        return ret;
    }
    transport->read32 = pci_virtio_read32;
    transport->write32 = pci_virtio_write32;
    transport->priv = owned;
    transport->legacy = 0;
    transport->shared_irq = 1;
    transport->msix_prepare = pci_virtio_msix_prepare;
    transport->msix_arm = pci_virtio_msix_arm;
    transport->msix_teardown = pci_virtio_msix_teardown;
    transport->msix_base = -1;
    transport->msix_vectors = 0;
    transport->irq = arch_pci_intx_irq(info->bus, info->dev, info->func,
                                       pci_read8(info, 0x3D) & 0x7U);
    kinfo("[VIRTIO-PCI] %s: common=0x%lx notify=0x%lx isr=0x%lx config=0x%lx mult=%u\n",
          dev->name, (unsigned long)candidate.common,
          (unsigned long)candidate.notify, (unsigned long)candidate.isr,
          (unsigned long)candidate.config, candidate.notify_multiplier);
    return 0;
}

static int pci_slot_for_bdf(int bus, int dev, int func)
{
    for (int i = 0; i < PCI_MAX_PUBLISHED_DEVICES; i++) {
        if (g_pci_in_use[i] && g_pci_infos[i].bus == (uint8_t)bus &&
            g_pci_infos[i].dev == (uint8_t)dev &&
            g_pci_infos[i].func == (uint8_t)func)
            return i;
    }
    for (int i = 0; i < PCI_MAX_PUBLISHED_DEVICES; i++)
        /* The static creator keeps reference one. Retired records can still
         * be held by progress snapshots or open class devices. */
        if (!g_pci_in_use[i] &&
            __atomic_load_n(&g_pci_devs[i].references, __ATOMIC_ACQUIRE) <= 1)
            return i;
    return -1;
}

/* Publish one BDF.  Returns 1 when the function is a header-type-1
 * bridge, so the caller also walks the buses behind it. */
static int pci_publish_bdf(int bus, int dev, int func,
                           uint8_t (*seen)[PCI_MAX_PUBLISHED_DEVICES],
                           int *found)
{
        uint32_t id = pci_ecam_read(bus, dev, func, 0);
        uint16_t vendor = (uint16_t)(id & 0xFFFF);
        /* Both all-ones and all-zeros mean "nothing is plugged in here", but
         * they mean it for different machines.  0xffff is the answer from a
         * window that decodes the address and finds no device behind it; 0x0000
         * is the answer from a window that is not there at all, which is what
         * reading ECAM off a machine with no ECAM (i440fx, say) gives.  Filtering
         * only 0xffff published one phantom device per slot on such a machine --
         * 128 of them, all id=0000:0000, none matching a driver -- and the
         * failure surfaced much later as "no init program found" with nothing
         * pointing at PCI. */
        if (vendor == 0xFFFF || vendor == 0x0000)
            return 0;

        /* Header type is 0x0E[23:16]; 1 is a PCI-to-PCI bridge.  Decide it
         * before any early return: a bridge that is already registered must
         * still be reported as one, or a rescan stops following it and the
         * devices behind it get reaped as missing. */
        uint32_t hdr = pci_ecam_read(bus, dev, func, 0x0E);
        int is_bridge = ((hdr >> 16) & 0x7F) == 1;

        uint16_t device_id = (uint16_t)((id >> 16) & 0xFFFF);
        int slot = pci_slot_for_bdf(bus, dev, func);
        if (slot < 0) {
            kerr("[BUS] pci: device slots exhausted for %02x:%02x.%x (error=%d)\n",
                 bus, dev, func, -ENOSPC);
            return is_bridge;
        }
        if (g_pci_in_use[slot] &&
            (g_pci_infos[slot].vendor != vendor ||
             g_pci_infos[slot].device != device_id)) {
            device_hotplug(&g_pci_devs[slot], BUS_EVENT_REMOVE);
            g_pci_in_use[slot] = 0;
            /* Unregister drops the registry pin, not external references.
             * Find a safe record again before writing a replacement identity. */
            slot = pci_slot_for_bdf(bus, dev, func);
            if (slot < 0) {
                kerr("[BUS] pci: replacement slots exhausted for %02x:%02x.%x (error=%d)\n",
                     bus, dev, func, -ENOSPC);
                return is_bridge;
            }
        }
        (*seen)[slot] = 1;
        if (g_pci_in_use[slot]) {
            (*found)++;
            return is_bridge;
        }

        pci_dev_info_t *info = &g_pci_infos[slot];
        memset(info, 0, sizeof(*info));
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

        resource_t *res = g_pci_resources[slot];
        memset(res, 0, sizeof(g_pci_resources[slot]));
        int resource_count = 0;
        for (int b = 0; b < 6; b++) info->bar_resource[b] = -1;
        /* Size assigned BARs before binding so unknown devices reserve their
         * apertures too. Type-1 bridge bus/window registers are not BARs. */
        unsigned header_type = (hdr >> 16) & 0x7f;
        int bars = header_type == 0 ? 6 : header_type == 1 ? 2 : 0;
        uint32_t command = pci_ecam_read(bus, dev, func, 0x04) & 0xffffU;
        pci_ecam_write(bus, dev, func, 0x04, command & ~3U);
        for (int b = 0; b < bars; b++) {
            uint32_t raw = pci_ecam_read(bus, dev, func, 0x10 + b * 4), flags;
            if (raw == 0xffffffffU) continue;
            uint64_t size = pci_bar_size(info, b, raw, &flags);
            if (!size || (size & (size - 1U))) continue;
            int io = flags & 1U;
            int wide = !io && ((flags & 6U) == 4U);
            uint64_t address = io ? raw & ~3U : pci_bar_address(info, b, raw, wide);
            info->bar[b] = address;
            info->bar_size[b] = size;
            info->bar_io[b] = io;
            if (address && size && !(size & (size - 1U)) &&
                size - 1U <= (~(uint64_t)0) - address) {
                info->bar_resource[b] = resource_count;
                res[resource_count++] = (resource_t){
                    .type = io ? RES_IOPORT : RES_MMIO,
                    .start = address, .end = address + size - 1U,
                    .flags = io ? 0 : (wide ? IORESOURCE_MMIO_64BIT : IORESOURCE_MMIO_32BIT) |
                                ((flags & 8U) ? IORESOURCE_PREFETCH : 0)};
            }
            if (wide) b++;
        }
        pci_ecam_write(bus, dev, func, 0x04, command);

        snprintf(g_pci_names[slot], sizeof(g_pci_names[slot]),
                 "pci-%04x:%04x-%d", vendor, device_id, slot);

        device_t *pdev     = &g_pci_devs[slot];
        memset(pdev, 0, sizeof(*pdev));
        pdev->name          = g_pci_names[slot];
        pdev->bus           = &pci_bus;
        pdev->plat_data     = info;
        pdev->res           = res;
        pdev->res_count     = resource_count;
        pdev->state         = DEV_STATE_UNINIT;
        int routed_irq = pci_intx_irq(pdev);
        if (routed_irq >= 0) {
            res[pdev->res_count++] = (resource_t){
                .type = RES_IRQ, .start = (uint32_t)routed_irq,
                .end = (uint32_t)routed_irq, .flags = IORESOURCE_IRQ_LEVEL};
        }

        if (device_hotplug(pdev, BUS_EVENT_ADD) < 0)
            return is_bridge;
        g_pci_in_use[slot] = 1;
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
        (*found)++;

    return is_bridge;
}

static void pci_scan_current(int remove_missing)
{
    uint8_t seen[PCI_MAX_PUBLISHED_DEVICES] = {0};
    int found = 0;

    /*
     * Walk buses from a worklist instead of the preconfigured
     * [bus_start, bus_end) range.  A flat range cannot see anything behind a
     * PCIe switch: on the riscv64 board the range is (0, 1), so only bus 0 is
     * ever probed and a device behind a bridge is invisible.  Following header
     * type 1 bridges is what real root complexes require.
     */
    uint8_t visited[PCI_MAX_BUS] = {0};
    int pending[PCI_MAX_BUS];
    int npending = 0;
    for (int b = g_pci_data.bus_start; b < g_pci_data.bus_end; b++)
        if (b >= 0 && b < PCI_MAX_BUS && !visited[b]) {
            visited[b] = 1;
            pending[npending++] = b;
        }

    while (npending > 0 && found < PCI_MAX_PUBLISHED_DEVICES) {
        int bus = pending[--npending];
        for (int dev = 0; dev < PCI_MAX_DEV && found < PCI_MAX_PUBLISHED_DEVICES; dev++) {
            for (int func = 0; func < PCI_MAX_FUNC && found < PCI_MAX_PUBLISHED_DEVICES; func++) {
                if (!pci_publish_bdf(bus, dev, func, &seen, &found))
                    continue;
                /* Bridge: enqueue every bus in [secondary, subordinate]. */
                uint32_t br = pci_ecam_read(bus, dev, func, 0x18);
                int secondary = (br >> 8) & 0xFF;
                int subordinate = (br >> 16) & 0xFF;
                if (secondary == 0 || subordinate < secondary)
                    continue;
                for (int b = secondary; b <= subordinate; b++) {
                    if (b <= 0 || b >= PCI_MAX_BUS || visited[b])
                        continue;
                    visited[b] = 1;
                    pending[npending++] = b;
                }
            }
        }
    }

    if (remove_missing) {
        for (int i = 0; i < PCI_MAX_PUBLISHED_DEVICES; i++) {
            if (g_pci_in_use[i] && !seen[i]) {
                device_hotplug(&g_pci_devs[i], BUS_EVENT_REMOVE);
                g_pci_in_use[i] = 0;
            }
        }
    }

    kinfo("[BUS] pci: found %d devices (ecam=0x%lx, bus %d-%d, bridges walked)\n",
          found, (unsigned long)g_pci_data.ecam_base, g_pci_data.bus_start,
          g_pci_data.bus_end);
}

void pci_enumerate(uintptr_t ecam_base, int bus_start, int bus_end) {
    if (bus_start < 0 || bus_end > 256 || bus_start >= bus_end)
        return;
    g_pci_data.ecam_base = ecam_base;
    g_pci_data.bus_start = bus_start;
    g_pci_data.bus_end   = bus_end;

    arch_pci_host_init(ecam_base);

    int registered = bus_register(&pci_bus);
    if (registered < 0 && registered != -EEXIST)
        return;
    g_pci_initialized = 1;
    driver_discovery_begin();
    pci_scan_current(0);
    driver_discovery_end();
}

void pci_rescan(void)
{
    if (!g_pci_initialized)
        return;
    driver_discovery_begin();
    pci_scan_current(1);
    driver_discovery_end();
    driver_retry_deferred();
}
