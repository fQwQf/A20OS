#include "pci.h"
#include "drv/virtio_transport.h"
#include "drv/virtio_blk.h"
#include "platform.h"
#include "core/stdio.h"
#include "core/string.h"

static uint32_t ecam_read(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg) {
    uintptr_t addr = PCIE_ECAM_BASE
        | ((uint32_t)bus << 20)
        | ((uint32_t)dev << 15)
        | ((uint32_t)func << 12)
        | (reg & 0xFFC);
    return *(volatile uint32_t *)addr;
}

static uint16_t ecam_read16(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg) {
    uint32_t word = ecam_read(bus, dev, func, reg & ~3);
    return (uint16_t)(word >> ((reg & 2) * 8));
}

static uint8_t ecam_read8(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg) {
    uint32_t word = ecam_read(bus, dev, func, reg & ~3);
    return (uint8_t)(word >> ((reg & 3) * 8));
}

static void ecam_write(uint8_t bus, uint8_t dev, uint8_t func, uint32_t reg, uint32_t val) {
    uintptr_t addr = PCIE_ECAM_BASE
        | ((uint32_t)bus << 20)
        | ((uint32_t)dev << 15)
        | ((uint32_t)func << 12)
        | (reg & 0xFFC);
    *(volatile uint32_t *)addr = val;
}

static int pci_inited = 0;
static pci_virtio_dev_t pci_devs[8];
static int pci_ndevs = 0;

static uint64_t pci_mmio_alloc = PCIE_MMIO_BASE;

static uint32_t read_bar(int dev, int bar) {
    return ecam_read(0, dev, 0, PCI_BAR0 + bar * 4);
}

static uintptr_t alloc_bar_mem(uint8_t dev, int bar) {
    uint32_t old = read_bar(dev, bar);
    ecam_write(0, dev, 0, PCI_BAR0 + bar * 4, 0xFFFFFFFF);
    uint32_t size_word = ecam_read(0, dev, 0, PCI_BAR0 + bar * 4);
    ecam_write(0, dev, 0, PCI_BAR0 + bar * 4, old);

    if (size_word == 0 || size_word == 0xFFFFFFFF)
        return 0;

    int is_io = size_word & 1;
    uint32_t mask = is_io ? ~0x3UL : ~0xFUL;
    uint32_t size = ~(size_word & mask) + 1;

    uintptr_t addr = (pci_mmio_alloc + size - 1) & ~(size - 1);
    pci_mmio_alloc = addr + size;

    ecam_write(0, dev, 0, PCI_BAR0 + bar * 4, (uint32_t)addr);
    return addr;
}

static void alloc_device_bars(int dev) {
        uint8_t ht = ecam_read8(0, dev, 0, PCI_HEADER_TYPE);
        int hdr_type = ht & 0x7F;
        int max_bar = (hdr_type == 1) ? 2 : 6; // Type 1 是 Bridge (2个BAR)，Type 0 是普通设备 (6个BAR)

    for (int i = 0; i < max_bar; i++) {
        uint32_t bar_val = read_bar(dev, i);
        if (bar_val == 0)
            continue;

        int is_io = bar_val & 1;
        int is_64 = (!is_io) && (((bar_val >> 1) & 0x3) == 2);

        if (!is_io) {
            alloc_bar_mem(dev, i);
            if (is_64 && i + 1 < max_bar) {
                ecam_write(0, dev, 0, PCI_BAR0 + (i + 1) * 4, 0);
                i++;
            }
        }
    }
}

static void enable_device(int dev) {
    uint32_t cmd = ecam_read(0, dev, 0, PCI_COMMAND);
    cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    ecam_write(0, dev, 0, PCI_COMMAND, cmd);
}

static int find_virtio_caps(int dev, pci_virtio_dev_t *vd) {
    uint16_t status = ecam_read16(0, dev, 0, PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST))
        return -1;

    uint8_t ptr = ecam_read8(0, dev, 0, PCI_CAPABILITIES_PTR) & 0xFC;
    int found = 0;

    while (ptr && ptr < 0xFF) {
        uint32_t cap0 = ecam_read(0, dev, 0, ptr);
        uint8_t cap_id = cap0 & 0xFF;
        uint8_t next = (cap0 >> 8) & 0xFF;

        if (cap_id != PCI_CAP_ID_VNDR) {
            ptr = next & 0xFC;
            continue;
        }

        uint8_t cfg_type = (cap0 >> 24) & 0xFF;
        uint32_t bar_word = ecam_read(0, dev, 0, ptr + 4);
        uint8_t bar_idx = bar_word & 0xFF;

        uint32_t cap_offset = ecam_read(0, dev, 0, ptr + 8);
        // length 字段在 ptr + 12 处

        uint32_t bar_val = read_bar(dev, bar_idx);
        uintptr_t bar_base = bar_val & ~0xFUL;

        switch (cfg_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            vd->common_base = bar_base + cap_offset;
            found |= 1;
            break;
        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            vd->notify_base = bar_base + cap_offset;
            vd->notify_off_multiplier = ecam_read(0, dev, 0, ptr + 16);
            found |= 2;
            break;
        case VIRTIO_PCI_CAP_ISR_CFG: 
            vd->isr_base = bar_base + cap_offset;
            found |= 8;
            break;
        case VIRTIO_PCI_CAP_DEVICE_CFG:
            vd->config_base = bar_base + cap_offset;
            found |= 4;
            break;
        }

        ptr = next & 0xFC;
    }

    return (found & 7) == 7 ? 0 : -1;
}

void pci_init(void) {
    if (pci_inited) return;
    pci_inited = 1;

    printf("[PCI] Scanning bus 0...\n");

    for (int dev = 0; dev < PCI_MAX_DEV; dev++) {
        uint32_t id = ecam_read(0, dev, 0, 0);
        uint16_t vendor = id & 0xFFFF;
        uint16_t device = (id >> 16) & 0xFFFF;

        if (vendor == 0xFFFF || vendor == 0)
            continue;

        if (vendor != PCI_VENDOR_ID_REDHAT)
            continue;

        int virtio_type = 0;
        if (device >= 0x1040) {
            virtio_type = device - 0x1040;
        } else {
            uint32_t sub = ecam_read(0, dev, 0, 0x2C);
            uint16_t sub_dev = (sub >> 16) & 0xFFFF;
            virtio_type = sub_dev;
        }

        if (virtio_type != 1 && virtio_type != 2)
            continue;

        alloc_device_bars(dev);
        enable_device(dev);

        pci_virtio_dev_t *vd = &pci_devs[pci_ndevs];
        memset(vd, 0, sizeof(*vd));
        vd->dev_num = dev;
        vd->device_type = virtio_type;

        if (find_virtio_caps(dev, vd) != 0)
            continue;

        vd->valid = 1;
        pci_ndevs++;
        printf("[PCI] Found virtio-%s at 00:%02x.0\n",
               virtio_type == 1 ? "net" : "blk", dev);
    }

    printf("[PCI] Found %d virtio device(s)\n", pci_ndevs);
}

/* ---- PCI modern transport ---- */

static uint32_t pci_common_read32(uintptr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static void pci_common_write32(uintptr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

static uint8_t pci_common_read8(uintptr_t base, uint32_t off) {
    return *(volatile uint8_t *)(base + off);
}

static void pci_common_write8(uintptr_t base, uint32_t off, uint8_t val) {
    *(volatile uint8_t *)(base + off) = val;
}

static uint16_t pci_common_read16(uintptr_t base, uint32_t off) {
    return *(volatile uint16_t *)(base + off);
}

static void pci_common_write16(uintptr_t base, uint32_t off, uint16_t val) {
    *(volatile uint16_t *)(base + off) = val;
}

/* PCI common cfg register offsets */
#define PCOMMON_DEV_FEAT_SEL   0x00
#define PCOMMON_DEV_FEAT       0x04
#define PCOMMON_DRV_FEAT_SEL   0x08
#define PCOMMON_DRV_FEAT       0x0C
#define PCOMMON_STATUS         0x14
#define PCOMMON_QUEUE_SEL      0x16
#define PCOMMON_QUEUE_SIZE     0x18
#define PCOMMON_QUEUE_ENABLE   0x1C
#define PCOMMON_QUEUE_DESC_LO  0x20
#define PCOMMON_QUEUE_DESC_HI  0x24
#define PCOMMON_QUEUE_DRV_LO   0x28
#define PCOMMON_QUEUE_DRV_HI   0x2C
#define PCOMMON_QUEUE_DEV_LO   0x30
#define PCOMMON_QUEUE_DEV_HI   0x34
#define PCOMMON_QUEUE_NOTIFY_OFF 0x1E

typedef struct {
    pci_virtio_dev_t *pdev;
} pci_transport_priv_t;

static uint32_t pci_vt_read32(virtio_transport_t *t, uint32_t mmio_off) {
    pci_transport_priv_t *p = (pci_transport_priv_t *)t->priv;
    pci_virtio_dev_t *vd = p->pdev;
    uintptr_t cb = vd->common_base;

    switch (mmio_off) {
    case VIRTIO_MMIO_DEVICE_FEATURES:
        return pci_common_read32(cb, PCOMMON_DEV_FEAT);
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        return pci_common_read32(cb, PCOMMON_DEV_FEAT_SEL);
    case VIRTIO_MMIO_DRIVER_FEATURES:
        return pci_common_read32(cb, PCOMMON_DRV_FEAT);
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        return pci_common_read32(cb, PCOMMON_DRV_FEAT_SEL);
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        return pci_common_read16(cb, PCOMMON_QUEUE_SIZE);
    case VIRTIO_MMIO_STATUS:
        return pci_common_read8(cb, PCOMMON_STATUS);
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        return pci_common_read32(cb, PCOMMON_QUEUE_DESC_LO);
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        return pci_common_read32(cb, PCOMMON_QUEUE_DESC_HI);
    case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
        return pci_common_read32(cb, PCOMMON_QUEUE_DRV_LO);
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
        return pci_common_read32(cb, PCOMMON_QUEUE_DRV_HI);
    case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
        return pci_common_read32(cb, PCOMMON_QUEUE_DEV_LO);
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
        return pci_common_read32(cb, PCOMMON_QUEUE_DEV_HI);
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        if (vd->isr_base)
            return *(volatile uint8_t *)(vd->isr_base); // ISR 寄存器是 8 位的
        return 0;
    case VIRTIO_MMIO_CONFIG:
        return *(volatile uint32_t *)(vd->config_base + 0);
    case VIRTIO_MMIO_CONFIG + 4:
        return *(volatile uint32_t *)(vd->config_base + 4);
    case VIRTIO_MMIO_GUEST_PAGE_SIZE:
    case VIRTIO_MMIO_QUEUE_PFN:
        return 0;
    default:
        return 0;
    }
}

static void pci_vt_write32(virtio_transport_t *t, uint32_t mmio_off, uint32_t val) {
    pci_transport_priv_t *p = (pci_transport_priv_t *)t->priv;
    pci_virtio_dev_t *vd = p->pdev;
    uintptr_t cb = vd->common_base;

    switch (mmio_off) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        pci_common_write32(cb, PCOMMON_DEV_FEAT_SEL, val);
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        pci_common_write32(cb, PCOMMON_DRV_FEAT, val);
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        pci_common_write32(cb, PCOMMON_DRV_FEAT_SEL, val);
        break;
    case VIRTIO_MMIO_STATUS:
        pci_common_write8(cb, PCOMMON_STATUS, (uint8_t)val);
        break;
    case VIRTIO_MMIO_QUEUE_SEL:
        pci_common_write16(cb, PCOMMON_QUEUE_SEL, (uint16_t)val);
        break;
    case VIRTIO_MMIO_QUEUE_NUM:
        pci_common_write16(cb, PCOMMON_QUEUE_SIZE, (uint16_t)val);
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        pci_common_write16(cb, PCOMMON_QUEUE_ENABLE, (uint16_t)val);
        break;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        pci_common_write32(cb, PCOMMON_QUEUE_DESC_LO, val);
        break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        pci_common_write32(cb, PCOMMON_QUEUE_DESC_HI, val);
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
        pci_common_write32(cb, PCOMMON_QUEUE_DRV_LO, val);
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
        pci_common_write32(cb, PCOMMON_QUEUE_DRV_HI, val);
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
        pci_common_write32(cb, PCOMMON_QUEUE_DEV_LO, val);
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
        pci_common_write32(cb, PCOMMON_QUEUE_DEV_HI, val);
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY: {
        // 1. 先切换到目标队列
        pci_common_write16(cb, PCOMMON_QUEUE_SEL, (uint16_t)val); 
        // 2. 再读取该队列对应的 notify_off
        uint16_t notify_off = pci_common_read16(cb, PCOMMON_QUEUE_NOTIFY_OFF);
        // 3. 计算地址并 Kick
        uintptr_t addr = vd->notify_base + notify_off * vd->notify_off_multiplier;
        *(volatile uint16_t *)addr = (uint16_t)val;
        break;
    }
    case VIRTIO_MMIO_GUEST_PAGE_SIZE:
    case VIRTIO_MMIO_QUEUE_PFN:
        break;
    default:
        break;
    }
}

static pci_transport_priv_t pci_privs[8];
static int pci_npriv;

static int arch_virtio_probe_type(int type, int index, virtio_transport_t *vt) {
    pci_init();

    pci_virtio_dev_t *vd = NULL;
    int seen = 0;
    for (int i = 0; i < pci_ndevs; i++) {
        if (!pci_devs[i].valid || pci_devs[i].device_type != type)
            continue;
        if (seen++ == index) {
            vd = &pci_devs[i];
            break;
        }
    }
    if (!vd)
        return -1;

    if (pci_npriv >= (int)(sizeof(pci_privs) / sizeof(pci_privs[0])))
        return -1;
    pci_transport_priv_t *priv = &pci_privs[pci_npriv++];
    priv->pdev = vd;

    vt->read32 = pci_vt_read32;
    vt->write32 = pci_vt_write32;
    vt->priv = priv;
    vt->legacy = 0;
    return 0;
}

int arch_virtio_blk_probe(int index, virtio_transport_t *vt) {
    return arch_virtio_probe_type(2, index, vt);
}

int arch_virtio_net_probe(int index, virtio_transport_t *vt) {
    return arch_virtio_probe_type(1, index, vt);
}
