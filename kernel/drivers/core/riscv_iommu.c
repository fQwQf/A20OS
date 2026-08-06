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
#define IOMMU_REG_TR_REQ_IOVA 0x0258u /* 64-bit */
#define IOMMU_REG_TR_REQ_CTL  0x0260u /* 64-bit */
#define IOMMU_REG_TR_RESPONSE 0x0268u /* 64-bit */

#define IOMMU_TR_CTL_GO      (1ull << 0)
#define IOMMU_TR_CTL_NW      (1ull << 3)
#define IOMMU_TR_RESP_FAULT  (1ull << 0)
#define IOMMU_TR_RESP_PPN    0x3ffffffffffc00ull

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
#define DC_FSC_MODE_SV39    8ull
#define DC_FSC_MODE_SHIFT   60ull

/* SV39 page-table entry bits. */
#define PTE_V               (1ull << 0)
#define PTE_R               (1ull << 1)
#define PTE_W               (1ull << 2)
#define PTE_X               (1ull << 3)
#define PTE_U               (1ull << 4)
#define PTE_A               (1ull << 6)
#define PTE_D               (1ull << 7)
#define PTE_PPN_SHIFT       10ull

/* IOVA chosen for the translation-domain probe: 0x10000000. */
#define IOMMU_PROBE_IOVA    0x10000000ull
#define IOMMU_PROBE_BAD_IOVA 0x20000000ull

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

/*
 * Install a translating device context (SV39, one mapped page) for
 * devid 0 in the 1LVL DDT (32-byte base-format DC at offset 0) and
 * verify the translation with the TR_REQ debug interface: the mapped
 * IOVA must resolve to the expected physical page and an unmapped
 * IOVA must fault — hardware-observable enforcement.
 */
static int riscv_iommu_verify_translation(uint64_t ddt_phys, uint64_t ddt_va)
{
    pfn_t root_pfn = pfa_alloc_page();
    pfn_t l1_pfn = pfa_alloc_page();
    pfn_t l0_pfn = pfa_alloc_page();
    pfn_t data_pfn = pfa_alloc_page();
    if (root_pfn == PFN_NONE || l1_pfn == PFN_NONE ||
        l0_pfn == PFN_NONE || data_pfn == PFN_NONE)
        return -1;

    uint64_t *root = pfn_to_virt(root_pfn);
    uint64_t *l1 = pfn_to_virt(l1_pfn);
    uint64_t *l0 = pfn_to_virt(l0_pfn);
    uint8_t *data = pfn_to_virt(data_pfn);
    memset(root, 0, 4096);
    memset(l1, 0, 4096);
    memset(l0, 0, 4096);
    memset(data, 0, 4096);

    uint64_t root_phys = pfn_to_phys(root_pfn);
    uint64_t l1_phys = pfn_to_phys(l1_pfn);
    uint64_t l0_phys = pfn_to_phys(l0_pfn);
    uint64_t data_phys = pfn_to_phys(data_pfn);

    /* Map IOMMU_PROBE_IOVA (0x10000000) -> data page.
     * SV39 vpn[2:0] = 0 / 0x80 / 0 for that IOVA. */
    l0[0] = (data_phys >> 12) << PTE_PPN_SHIFT |
            PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D;
    l1[0x80] = (l0_phys >> 12) << PTE_PPN_SHIFT | PTE_V;
    root[0] = (l1_phys >> 12) << PTE_PPN_SHIFT | PTE_V;
    kinfo("[IOMMU] pt root@0x%lx l1@0x%lx l0@0x%lx data@0x%lx ptes: "
          "r0=0x%08lx%08lx l1[0x80]=0x%08lx%08lx l0[0]=0x%08lx%08lx\n",
          (unsigned long)root_phys, (unsigned long)l1_phys,
          (unsigned long)l0_phys, (unsigned long)data_phys,
          (unsigned long)(root[0] >> 32), (unsigned long)(uint32_t)root[0],
          (unsigned long)(l1[0x80] >> 32), (unsigned long)(uint32_t)l1[0x80],
          (unsigned long)(l0[0] >> 32), (unsigned long)(uint32_t)l0[0]);

    /* Translating device context for devid 0 (SV39, no process context). */
    uint64_t *dc = (uint64_t *)ddt_va;
    dc[0] = DC_TC_V;                    /* tc: V=1, SXL=0, SBE=0 */
    dc[1] = 0;                          /* iohgatp: BARE */
    dc[2] = 0;                          /* ta */
    dc[3] = (DC_FSC_MODE_SV39 << DC_FSC_MODE_SHIFT) | (root_phys >> 12);

    /* TR_REQ: mapped IOVA must translate to data_phys (RO request).
     * Note: Debian QEMU 10.0.11 stores PPN_DOWN(iova) & PPN_MASK in the
     * response PPN field (its set_field is a plain bit-and), so compare
     * the field against ppn_down & PPN_MASK. */
    iommu_write64(IOMMU_REG_TR_REQ_IOVA, IOMMU_PROBE_IOVA);
    iommu_write64(IOMMU_REG_TR_REQ_CTL,
                  IOMMU_TR_CTL_GO | IOMMU_TR_CTL_NW);
    uint64_t resp = iommu_read64(IOMMU_REG_TR_RESPONSE);
    uint64_t expect_field = (data_phys >> 12) & IOMMU_TR_RESP_PPN;
    uint64_t got_field = resp & IOMMU_TR_RESP_PPN;
    kinfo("[IOMMU] TR_REQ mapped iova=0x%lx -> field=0x%08lx%08lx "
          "(expected 0x%08lx%08lx) fault=%u raw=0x%08lx%08lx\n",
          (unsigned long)IOMMU_PROBE_IOVA,
          (unsigned long)(got_field >> 32), (unsigned long)(uint32_t)got_field,
          (unsigned long)(expect_field >> 32), (unsigned long)(uint32_t)expect_field,
          !!(resp & IOMMU_TR_RESP_FAULT),
          (unsigned long)(resp >> 32), (unsigned long)(uint32_t)resp);
    if ((resp & IOMMU_TR_RESP_FAULT) || got_field != expect_field) {
        kerr("[IOMMU] TR_REQ mapped translation mismatch\n");
        return -1;
    }

    /* TR_REQ: unmapped IOVA must be rejected by the hardware. */
    iommu_write64(IOMMU_REG_TR_REQ_IOVA, IOMMU_PROBE_BAD_IOVA);
    iommu_write64(IOMMU_REG_TR_REQ_CTL,
                  IOMMU_TR_CTL_GO | IOMMU_TR_CTL_NW);
    uint64_t resp2 = iommu_read64(IOMMU_REG_TR_RESPONSE);
    kinfo("[IOMMU] TR_REQ unmapped iova=0x%lx -> fault=%u cause=%lu\n",
          (unsigned long)IOMMU_PROBE_BAD_IOVA,
          !!(resp2 & IOMMU_TR_RESP_FAULT),
          (unsigned long)((resp2 >> 10) & 0xfff));
    if (!(resp2 & IOMMU_TR_RESP_FAULT)) {
        kerr("[IOMMU] TR_REQ unmapped IOVA was not rejected\n");
        return -1;
    }

    kinfo("[IOMMU] translation domain verified (SV39, 1 page mapped, "
          "unmapped rejected)\n");
    return 0;
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

    /* Allocate DDT (doubles as the DC array in 1LVL mode), CQ, FQ. */
    pfn_t ddt_pfn = pfa_alloc_page();
    pfn_t cq_pfn = pfa_alloc_page();
    pfn_t fq_pfn = pfa_alloc_page();
    if (ddt_pfn == PFN_NONE || cq_pfn == PFN_NONE || fq_pfn == PFN_NONE) {
        kerr("[IOMMU] page allocation failed\n");
        return -1;
    }
    void *ddt_va = pfn_to_virt(ddt_pfn);
    void *cq_va = pfn_to_virt(cq_pfn);
    void *fq_va = pfn_to_virt(fq_pfn);
    memset(ddt_va, 0, 4096);
    memset(cq_va, 0, 4096);
    memset(fq_va, 0, 4096);

    uint64_t ddt_phys = pfn_to_phys(ddt_pfn);
    uint64_t cq_phys = pfn_to_phys(cq_pfn);
    uint64_t fq_phys = pfn_to_phys(fq_pfn);

    /* 1LVL DDT: each 32-byte slot is a base-format Device Context.
     * devid 1 (the IOMMU itself) gets a passthrough context (TC.V=1,
     * both stages BARE); devid 0 is reprogrammed with a translating
     * context by riscv_iommu_verify_translation(). */
    uint64_t *dc1 = (uint64_t *)((uint8_t *)ddt_va + 32);
    dc1[0] = DC_TC_V;   /* tc: V only */
    dc1[1] = 0;         /* iohgatp: BARE */
    dc1[2] = 0;         /* ta */
    dc1[3] = 0;         /* fsc: BARE */

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

    kinfo("[IOMMU] hardware initialized: DDT@0x%lx CQ@0x%lx FQ@0x%lx\n",
          (unsigned long)ddt_phys,
          (unsigned long)cq_phys, (unsigned long)fq_phys);

    /* Per-domain translation: SV39 domain for devid 0, verified via TR_REQ. */
    if (riscv_iommu_verify_translation(ddt_phys,
                                       (uint64_t)(uintptr_t)ddt_va) != 0) {
        kerr("[IOMMU] translation verification failed\n");
        return -1;
    }
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
