/*
 * RISC-V IOMMU PCI driver.
 *
 * QEMU 10 exposes riscv-iommu-pci (vendor 1b36/device 0014).  This
 * driver assigns the BAR, then performs a full software-init of the
 * device per the RISC-V IOMMU spec 1.0 / QEMU implementation:
 *
 *   - allocates DDT page, per-device DC page, command queue and fault
 *     queue;
 *   - programs CQB/FQB/DDTP;
 *   - enables CQ and FQ (CQCSR/FQCSR);
 *   - installs a passthrough Device Context (TC.V=1, BARE stages) for
 *     the PCI leaf devices so existing DMA keeps working;
 *   - verifies queue-on and DDTP completion, then reports the IOMMU
 *     as enabled with observable queue registers.
 *
 * Real per-domain page tables and IOVA mapping remain future work;
 * this is the hardware-initialized, device-owned baseline.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/pci_hal.h"
#include "mm/frame.h"
#include "core/klog.h"
#include "core/string.h"

#define RISCV_IOMMU_VENDOR 0x1b36u
#define RISCV_IOMMU_DEVICE 0x0014u

/* Register map (QEMU hw/riscv/riscv-iommu-bits.h). */
#define IOMMU_REG_CAP       0x0000u  /* 64-bit */
#define IOMMU_REG_FCTL      0x0008u
#define IOMMU_REG_DDTP      0x0010u  /* 64-bit */
#define IOMMU_REG_CQB       0x0018u  /* 64-bit */
#define IOMMU_REG_CQH       0x0020u
#define IOMMU_REG_CQT       0x0024u
#define IOMMU_REG_FQB       0x0028u  /* 64-bit */
#define IOMMU_REG_FQH       0x0030u
#define IOMMU_REG_FQT       0x0034u
#define IOMMU_REG_CQCSR     0x0048u
#define IOMMU_REG_FQCSR     0x004cu

#define IOMMU_DDTP_MODE_1LVL 2u
#define IOMMU_DDTP_BUSY     (1u << 4)
#define IOMMU_PPN_SHIFT     10u
#define IOMMU_PPN_MASK      0x3ffffffffffc00ull
#define IOMMU_LOG2SZ_MASK   0x1fu

#define IOMMU_QUEUE_ENABLE  (1u << 0)
#define IOMMU_QUEUE_ACTIVE  (1u << 16)

/* DDTE / DC formats. */
#define DDTE_VALID          (1ull << 0)
#define DC_TC_V             (1ull << 0)

#define IOMMU_CQ_LOGSZ       8u   /* 256 entries of 16 bytes in 4 KiB */
#define IOMMU_FQ_LOGSZ       7u   /* 128 entries of 32 bytes in 4 KiB */

static const device_id_t riscv_iommu_ids[] = {
    { RISCV_IOMMU_VENDOR, RISCV_IOMMU_DEVICE, VENDOR_ANY, DEVICE_ANY, 0 },
    { 0, 0, 0, 0, 0 }
};

static uint64_t g_iommu_base;

static uint32_t iommu_read32(uint32_t off)
{
    if (!g_iommu_base)
        return 0;
    return readl((const volatile void *)(uintptr_t)(g_iommu_base + off));
}

static void iommu_write32(uint32_t off, uint32_t val)
{
    if (!g_iommu_base)
        return;
    writel(val, (volatile void *)(uintptr_t)(g_iommu_base + off));
}

static uint64_t iommu_read64(uint32_t off)
{
    return (uint64_t)iommu_read32(off) |
           ((uint64_t)iommu_read32(off + 4) << 32);
}

static void iommu_write64(uint32_t off, uint64_t val)
{
    iommu_write32(off, (uint32_t)val);
    iommu_write32(off + 4, (uint32_t)(val >> 32));
}

static uint64_t iommu_phys(uint64_t ppn_field)
{
    return (ppn_field >> IOMMU_PPN_SHIFT) << 12;
}

static int riscv_iommu_probe(device_t *dev)
{
    if (!dev)
        return -1;
    if (pci_enable_and_assign_bars(dev) != 0) {
        kerr("[IOMMU] %s: BAR setup failed\n", dev->name);
        return -1;
    }
    resource_t *bar = pci_get_bar_resource(dev, 0);
    if (!bar || bar->type != RES_MMIO || bar->start >= bar->end)
        return -1;
    g_iommu_base = bar->start;
    kinfo("[IOMMU] riscv-iommu: BAR0=0x%lx..0x%lx\n",
          (unsigned long)bar->start, (unsigned long)bar->end);

    uint64_t cap = iommu_read64(IOMMU_REG_CAP);
    uint32_t fctl = iommu_read32(IOMMU_REG_FCTL);
    kinfo("[IOMMU] capabilities=0x%08lx%08lx features=0x%08x (version=%u)\n",
          (unsigned long)(cap >> 32), (unsigned long)(uint32_t)cap,
          fctl, (unsigned)(cap & 0xff));

    /* Allocate DDT, DC, CQ and FQ pages. */
    pfn_t ddt_pfn = pfa_alloc_page();
    pfn_t dc_pfn = pfa_alloc_page();
    pfn_t cq_pfn = pfa_alloc_page();
    pfn_t fq_pfn = pfa_alloc_page();
    if (ddt_pfn == PFN_NONE || dc_pfn == PFN_NONE ||
        cq_pfn == PFN_NONE || fq_pfn == PFN_NONE) {
        kerr("[IOMMU] page allocation failed\n");
        return -1;
    }
    void *ddt_va = pfn_to_virt(ddt_pfn);
    void *dc_va = pfn_to_virt(dc_pfn);
    void *cq_va = pfn_to_virt(cq_pfn);
    void *fq_va = pfn_to_virt(fq_pfn);
    memset(ddt_va, 0, 4096);
    memset(dc_va, 0, 4096);
    memset(cq_va, 0, 4096);
    memset(fq_va, 0, 4096);

    uint64_t ddt_phys = (uint64_t)ddt_pfn << 12;
    uint64_t dc_phys = (uint64_t)dc_pfn << 12;
    uint64_t cq_phys = (uint64_t)cq_pfn << 12;
    uint64_t fq_phys = (uint64_t)fq_pfn << 12;

    /* Passthrough DCs for the leaf PCI devices (00:00.0, 00:01.0). */
    for (int devid = 0; devid < 2; devid++) {
        uint64_t *dc = (uint64_t *)((uint8_t *)dc_va + devid * 64);
        dc[0] = DC_TC_V;              /* tc.V=1, stages BARE => passthrough */
        dc[1] = 0;                    /* iohgatp */
        dc[2] = 0;                    /* ta */
        dc[3] = 0;                    /* fsc */
        dc[4] = 0;                    /* msiptp */
        dc[5] = 0;
        dc[6] = 0;
        dc[7] = 0;
        uint64_t *ddte = (uint64_t *)ddt_va + devid;
        *ddte = DDTE_VALID | ((dc_phys >> 12) << IOMMU_PPN_SHIFT);
    }

    /* Program the queues. */
    iommu_write64(IOMMU_REG_CQB,
                  IOMMU_CQ_LOGSZ | ((cq_phys >> 12) << IOMMU_PPN_SHIFT));
    iommu_write32(IOMMU_REG_CQH, 0);
    iommu_write32(IOMMU_REG_CQT, 0);
    iommu_write64(IOMMU_REG_FQB,
                  IOMMU_FQ_LOGSZ | ((fq_phys >> 12) << IOMMU_PPN_SHIFT));
    iommu_write32(IOMMU_REG_FQH, 0);
    iommu_write32(IOMMU_REG_FQT, 0);

    /* DDT pointer: this is the "enable" switch. */
    iommu_write64(IOMMU_REG_DDTP,
                  IOMMU_DDTP_MODE_1LVL | ((ddt_phys >> 12) << IOMMU_PPN_SHIFT));

    /* Enable queues. */
    iommu_write32(IOMMU_REG_CQCSR, IOMMU_QUEUE_ENABLE);
    iommu_write32(IOMMU_REG_FQCSR, IOMMU_QUEUE_ENABLE);

    uint32_t cqcsr = iommu_read32(IOMMU_REG_CQCSR);
    uint32_t fqcsr = iommu_read32(IOMMU_REG_FQCSR);
    uint64_t ddtp = iommu_read64(IOMMU_REG_DDTP);
    kinfo("[IOMMU] enabled: cqcsr=0x%08x fqcsr=0x%08x ddtp=0x%08lx%08lx\n",
          cqcsr, fqcsr,
          (unsigned long)(ddtp >> 32), (unsigned long)(uint32_t)ddtp);

    if (!(cqcsr & IOMMU_QUEUE_ACTIVE) || !(fqcsr & IOMMU_QUEUE_ACTIVE) ||
        (ddtp & IOMMU_DDTP_BUSY)) {
        kerr("[IOMMU] enable failed (cqon=%u fqon=%u busy=%u)\n",
             !!(cqcsr & IOMMU_QUEUE_ACTIVE),
             !!(fqcsr & IOMMU_QUEUE_ACTIVE),
             !!(ddtp & IOMMU_DDTP_BUSY));
        return -1;
    }

    kinfo("[IOMMU] hardware initialized: DDT@0x%lx DC@0x%lx CQ@0x%lx FQ@0x%lx\n",
          (unsigned long)ddt_phys, (unsigned long)dc_phys,
          (unsigned long)cq_phys, (unsigned long)fq_phys);
    return 0;
}

static int riscv_iommu_remove(device_t *dev)
{
    (void)dev;
    g_iommu_base = 0;
    return 0;
}

/* Diagnostic before the driver binding pass. */
void riscv_iommu_early_probe(void)
{
    uint32_t id = arch_pci_config_read32(0, 1, 0, 0x00);
    if ((id & 0xffffu) != RISCV_IOMMU_VENDOR ||
        (id >> 16) != RISCV_IOMMU_DEVICE)
        return;
    kinfo("[IOMMU] riscv-iommu PCI present at 00:01.0 (driver probe follows)\n");
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
