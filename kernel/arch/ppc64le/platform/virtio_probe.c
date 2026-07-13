#include "pci.h"
#include "drivers/bus/pci_hal.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/block/virtio_blk.h"
#include "core/stdio.h"
#include "core/string.h"

#define PPC64_H_LOGICAL_CI_LOAD  0x3CUL
#define PPC64_H_LOGICAL_CI_STORE 0x40UL

static pci_virtio_dev_t pci_devs[8];
static int pci_ndevs;
static int pci_inited;

static uint32_t pci_read32(int dev, uint32_t reg) {
    return arch_pci_config_read32(0, dev, 0, reg);
}

static uint16_t pci_read16(int dev, uint32_t reg) {
    uint32_t word = pci_read32(dev, reg & ~3U);
    return (uint16_t)(word >> ((reg & 2U) * 8U));
}

static uint8_t pci_read8(int dev, uint32_t reg) {
    uint32_t word = pci_read32(dev, reg & ~3U);
    return (uint8_t)(word >> ((reg & 3U) * 8U));
}

static uint64_t pci_bar_address(int dev, int bar) {
    uint32_t lo = pci_read32(dev, PCI_BAR0 + (uint32_t)bar * 4U);
    if (lo == 0 || lo == 0xFFFFFFFFU || (lo & 1U))
        return 0;

    uint64_t addr = lo & ~0xFULL;
    if (((lo >> 1) & 3U) == 2U) {
        uint32_t hi = pci_read32(dev, PCI_BAR0 + (uint32_t)(bar + 1) * 4U);
        addr |= (uint64_t)hi << 32;
    }
    return addr;
}

static uint64_t ci_load(uintptr_t addr, uint64_t size) {
    register uint64_t r3 __asm__("r3") = PPC64_H_LOGICAL_CI_LOAD;
    register uint64_t r4 __asm__("r4") = size;
    register uint64_t r5 __asm__("r5") = addr;
    __asm__ __volatile__(
        "sc 1"
        : "+r"(r3), "+r"(r4), "+r"(r5)
        :
        : "r6", "r7", "r8", "r9", "r10", "r11", "r12",
          "ctr", "lr", "cr0", "memory");
    return r3 == 0 ? r4 : 0;
}

static void ci_store(uintptr_t addr, uint64_t size, uint64_t value) {
    register uint64_t r3 __asm__("r3") = PPC64_H_LOGICAL_CI_STORE;
    register uint64_t r4 __asm__("r4") = size;
    register uint64_t r5 __asm__("r5") = addr;
    register uint64_t r6 __asm__("r6") = value;
    __asm__ __volatile__(
        "sc 1"
        : "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6)
        :
        : "r7", "r8", "r9", "r10", "r11", "r12",
          "ctr", "lr", "cr0", "memory");
}

static uint8_t mmio_read8(uintptr_t addr) {
    return (uint8_t)ci_load(addr, 1);
}

static uint16_t mmio_read16(uintptr_t addr) {
    return __builtin_bswap16((uint16_t)ci_load(addr, 2));
}

static uint32_t mmio_read32(uintptr_t addr) {
    return __builtin_bswap32((uint32_t)ci_load(addr, 4));
}

static void mmio_write8(uintptr_t addr, uint8_t value) {
    ci_store(addr, 1, value);
}

static void mmio_write16(uintptr_t addr, uint16_t value) {
    ci_store(addr, 2, __builtin_bswap16(value));
}

static void mmio_write32(uintptr_t addr, uint32_t value) {
    ci_store(addr, 4, __builtin_bswap32(value));
}

static int find_virtio_caps(int dev, pci_virtio_dev_t *vd) {
    if (!(pci_read16(dev, PCI_STATUS) & PCI_STATUS_CAP_LIST))
        return -1;

    uint8_t ptr = pci_read8(dev, PCI_CAPABILITIES_PTR) & 0xFCU;
    int found = 0;

    for (int guard = 0; ptr && ptr < 0xFCU && guard < 48; guard++) {
        uint32_t cap0 = pci_read32(dev, ptr);
        uint8_t cap_id = (uint8_t)cap0;
        uint8_t next = (uint8_t)(cap0 >> 8);

        if (cap_id == PCI_CAP_ID_VNDR) {
            uint8_t cfg_type = (uint8_t)(cap0 >> 24);
            uint8_t bar = (uint8_t)pci_read32(dev, ptr + 4);
            uint32_t offset = pci_read32(dev, ptr + 8);
            uintptr_t base =
                arch_pci_bar_to_resource(pci_bar_address(dev, bar));

            if (base) {
                switch (cfg_type) {
                case VIRTIO_PCI_CAP_COMMON_CFG:
                    vd->common_base = base + offset;
                    found |= 1;
                    break;
                case VIRTIO_PCI_CAP_NOTIFY_CFG:
                    vd->notify_base = base + offset;
                    vd->notify_off_multiplier = pci_read32(dev, ptr + 16);
                    found |= 2;
                    break;
                case VIRTIO_PCI_CAP_ISR_CFG:
                    vd->isr_base = base + offset;
                    break;
                case VIRTIO_PCI_CAP_DEVICE_CFG:
                    vd->config_base = base + offset;
                    found |= 4;
                    break;
                }
            }
        }
        ptr = next & 0xFCU;
    }
    return (found & 7) == 7 ? 0 : -1;
}

static void pci_init(void) {
    if (pci_inited)
        return;
    pci_inited = 1;
    arch_pci_host_init(0);

    printf("[PCI] Scanning pseries PHB...\n");
    for (int dev = 0; dev < PCI_MAX_DEV && pci_ndevs < 8; dev++) {
        uint32_t id = pci_read32(dev, 0);
        uint16_t vendor = (uint16_t)id;
        uint16_t device = (uint16_t)(id >> 16);
        if (vendor != PCI_VENDOR_ID_REDHAT)
            continue;

        int type = device >= 0x1040 ? device - 0x1040 :
            (int)(pci_read32(dev, 0x2C) >> 16);
        if (type != 1 && type != 2)
            continue;

        uint32_t command = pci_read32(dev, PCI_COMMAND);
        command |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
        arch_pci_config_write32(0, dev, 0, PCI_COMMAND, command);

        pci_virtio_dev_t *vd = &pci_devs[pci_ndevs];
        memset(vd, 0, sizeof(*vd));
        vd->dev_num = dev;
        vd->device_type = type;
        if (find_virtio_caps(dev, vd) != 0)
            continue;

        vd->valid = 1;
        pci_ndevs++;
        printf("[PCI] Found virtio-%s at 00:%02x.0\n",
               type == 1 ? "net" : "blk", dev);
    }
    printf("[PCI] Found %d virtio device(s)\n", pci_ndevs);
}

#define PCOMMON_DEV_FEAT_SEL      0x00
#define PCOMMON_DEV_FEAT          0x04
#define PCOMMON_DRV_FEAT_SEL      0x08
#define PCOMMON_DRV_FEAT          0x0C
#define PCOMMON_STATUS            0x14
#define PCOMMON_QUEUE_SEL         0x16
#define PCOMMON_QUEUE_SIZE        0x18
#define PCOMMON_QUEUE_ENABLE      0x1C
#define PCOMMON_QUEUE_NOTIFY_OFF  0x1E
#define PCOMMON_QUEUE_DESC_LO     0x20
#define PCOMMON_QUEUE_DESC_HI     0x24
#define PCOMMON_QUEUE_DRV_LO      0x28
#define PCOMMON_QUEUE_DRV_HI      0x2C
#define PCOMMON_QUEUE_DEV_LO      0x30
#define PCOMMON_QUEUE_DEV_HI      0x34

typedef struct {
    pci_virtio_dev_t *pdev;
} pci_transport_priv_t;

static uint32_t pci_vt_read32(virtio_transport_t *t, uint32_t off) {
    pci_virtio_dev_t *vd = ((pci_transport_priv_t *)t->priv)->pdev;
    uintptr_t cb = vd->common_base;

    switch (off) {
    case VIRTIO_MMIO_DEVICE_FEATURES:
        return mmio_read32(cb + PCOMMON_DEV_FEAT);
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        return mmio_read32(cb + PCOMMON_DEV_FEAT_SEL);
    case VIRTIO_MMIO_DRIVER_FEATURES:
        return mmio_read32(cb + PCOMMON_DRV_FEAT);
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        return mmio_read32(cb + PCOMMON_DRV_FEAT_SEL);
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        return mmio_read16(cb + PCOMMON_QUEUE_SIZE);
    case VIRTIO_MMIO_STATUS:
        return mmio_read8(cb + PCOMMON_STATUS);
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        return mmio_read32(cb + PCOMMON_QUEUE_DESC_LO);
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        return mmio_read32(cb + PCOMMON_QUEUE_DESC_HI);
    case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
        return mmio_read32(cb + PCOMMON_QUEUE_DRV_LO);
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
        return mmio_read32(cb + PCOMMON_QUEUE_DRV_HI);
    case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
        return mmio_read32(cb + PCOMMON_QUEUE_DEV_LO);
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
        return mmio_read32(cb + PCOMMON_QUEUE_DEV_HI);
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        return vd->isr_base ? mmio_read8(vd->isr_base) : 0;
    case VIRTIO_MMIO_CONFIG:
        return mmio_read32(vd->config_base);
    case VIRTIO_MMIO_CONFIG + 4:
        return mmio_read32(vd->config_base + 4);
    default:
        return 0;
    }
}

static void pci_vt_write32(virtio_transport_t *t, uint32_t off, uint32_t val) {
    pci_virtio_dev_t *vd = ((pci_transport_priv_t *)t->priv)->pdev;
    uintptr_t cb = vd->common_base;

    switch (off) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        mmio_write32(cb + PCOMMON_DEV_FEAT_SEL, val);
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        mmio_write32(cb + PCOMMON_DRV_FEAT, val);
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        mmio_write32(cb + PCOMMON_DRV_FEAT_SEL, val);
        break;
    case VIRTIO_MMIO_STATUS:
        mmio_write8(cb + PCOMMON_STATUS, (uint8_t)val);
        break;
    case VIRTIO_MMIO_QUEUE_SEL:
        mmio_write16(cb + PCOMMON_QUEUE_SEL, (uint16_t)val);
        break;
    case VIRTIO_MMIO_QUEUE_NUM:
        mmio_write16(cb + PCOMMON_QUEUE_SIZE, (uint16_t)val);
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        mmio_write16(cb + PCOMMON_QUEUE_ENABLE, (uint16_t)val);
        break;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        mmio_write32(cb + PCOMMON_QUEUE_DESC_LO, val);
        break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        mmio_write32(cb + PCOMMON_QUEUE_DESC_HI, val);
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
        mmio_write32(cb + PCOMMON_QUEUE_DRV_LO, val);
        break;
    case VIRTIO_MMIO_QUEUE_DRIVER_HIGH:
        mmio_write32(cb + PCOMMON_QUEUE_DRV_HI, val);
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
        mmio_write32(cb + PCOMMON_QUEUE_DEV_LO, val);
        break;
    case VIRTIO_MMIO_QUEUE_DEVICE_HIGH:
        mmio_write32(cb + PCOMMON_QUEUE_DEV_HI, val);
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY: {
        mmio_write16(cb + PCOMMON_QUEUE_SEL, (uint16_t)val);
        uint16_t notify_off = mmio_read16(cb + PCOMMON_QUEUE_NOTIFY_OFF);
        uintptr_t addr =
            vd->notify_base + notify_off * vd->notify_off_multiplier;
        mmio_write16(addr, (uint16_t)val);
        break;
    }
    default:
        break;
    }
}

static pci_transport_priv_t pci_privs[8];
static int pci_npriv;

static int arch_virtio_probe_type(int type, int index, virtio_transport_t *vt) {
    pci_init();

    int seen = 0;
    pci_virtio_dev_t *vd = NULL;
    for (int i = 0; i < pci_ndevs; i++) {
        if (!pci_devs[i].valid || pci_devs[i].device_type != type)
            continue;
        if (seen++ == index) {
            vd = &pci_devs[i];
            break;
        }
    }
    if (!vd || pci_npriv >= 8)
        return -1;

    pci_transport_priv_t *priv = &pci_privs[pci_npriv++];
    priv->pdev = vd;
    vt->read32 = pci_vt_read32;
    vt->write32 = pci_vt_write32;
    vt->priv = priv;
    vt->legacy = 0;
    vt->irq = -1;
    return 0;
}

int arch_virtio_blk_probe(int index, virtio_transport_t *vt) {
    return arch_virtio_probe_type(2, index, vt);
}

int arch_virtio_net_probe(int index, virtio_transport_t *vt) {
    return arch_virtio_probe_type(1, index, vt);
}
