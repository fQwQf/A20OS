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
 *   - installs passthrough contexts by default so existing PCI DMA keeps
 *     working;
 *   - verifies queue-on and DDTP completion, then reports the IOMMU
 *     as enabled with observable queue registers.
 *   - provides one fail-closed per-device domain for a user driver, including
 *     IOVA map/unmap, context/IOTLB invalidation and fault-queue cleanup;
 *   - exports cumulative domain/map/fault counters via riscv_iommu_get_stats()
 *     for /proc/a20/iommu.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/pci_hal.h"
#include "drivers/core/riscv_iommu.h"
#include "drivers/core/dma_mapping.h"
#include "core/errno.h"
#include "mm/slab.h"
#include "core/panic.h"
#include "drivers/core/runtime_test.h"
#include "mm/frame.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/lock.h"
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
#define IOMMU_QUEUE_BUSY    (1u << 17)
#define IOMMU_QUEUE_ERROR    ((1u << 8) | (1u << 9) | (1u << 10))

#define IOMMU_CAP_SV39      (1ull << 9)
#define IOMMU_CAP_MSI_FLAT  (1ull << 22)

/* DDTE / DC formats. */
#define DDTE_VALID          (1ull << 0)
#define DC_TC_V             (1ull << 0)
#define DC_FSC_MODE_SV39    8ull
#define DC_FSC_MODE_SHIFT   60ull
/* TA.PSCID tags translation-cache entries.  With a bare G-stage the cache key
 * is (tag, GSCID, PSCID, IOVA); leaving PSCID zero for every device makes two
 * domains that map the same IOVA share each other's cached translation.  Each
 * claimed device therefore gets its own PSCID (1..IOMMU_USER_DOMAINS). */
#define DC_TA_PSCID_SHIFT   12ull

/* SV39 page-table entry bits. */
#define IOMMU_PTE_V         (1ull << 0)
#define IOMMU_PTE_R         (1ull << 1)
#define IOMMU_PTE_W         (1ull << 2)
#define IOMMU_PTE_U         (1ull << 4)
#define IOMMU_PTE_A         (1ull << 6)
#define IOMMU_PTE_D         (1ull << 7)
#define IOMMU_PTE_PPN_SHIFT 10ull

/* IOVA chosen for the translation-domain probe: 0x10000000. */
#define IOMMU_PROBE_IOVA    0x10000000ull
#define IOMMU_PROBE_BAD_IOVA 0x20000000ull

#define IOMMU_CQ_LOGSZ       7u   /* 2^(7+1) * 16 bytes = 4 KiB */
#define IOMMU_FQ_LOGSZ       6u   /* 2^(6+1) * 32 bytes = 4 KiB */
#define IOMMU_CQ_MASK        ((1u << (IOMMU_CQ_LOGSZ + 1)) - 1)
#define IOMMU_FQ_MASK        ((1u << (IOMMU_FQ_LOGSZ + 1)) - 1)

#define IOMMU_CMD_IOTINVAL   1ull
#define IOMMU_CMD_IOFENCE    2ull
#define IOMMU_CMD_IODIR      3ull
#define IOMMU_CMD_IODIR_DV   (1ull << 33)
#define IOMMU_CMD_DID_SHIFT  40u

#define IOMMU_USER_IOVA      0x01000000ull
#define IOMMU_USER_MAX_PAGES 64u

static const device_id_t riscv_iommu_ids[] = {
    { RISCV_IOMMU_VENDOR, RISCV_IOMMU_DEVICE, VENDOR_ANY, DEVICE_ANY, 0 },
    { 0, 0, 0, 0, 0 }
};

static uint64_t g_iommu_base;
static uint8_t *g_iommu_ddt;
static uint64_t *g_iommu_cq;
static uint64_t *g_iommu_fq;
static uint32_t g_iommu_dc_stride;
static uint32_t g_iommu_cq_tail;
static int g_iommu_ready;
static spinlock_t g_iommu_cq_lock = SPINLOCK_INIT;
static spinlock_t g_iommu_domain_lock = SPINLOCK_INIT;

typedef struct iommu_user_domain {
    uint16_t devid;
    int owner_pid;
    int active;
    int blocked;
    int claim_failed;
    pfn_t root_pfn;
    pfn_t l1_pfn;
    pfn_t l0_pfn;
    uint64_t *l0;
    uint64_t mapped_pages;
    pfn_t pinned[IOMMU_USER_MAX_PAGES]; /* independent RAM refs per IOVA */
    uint64_t fault_count;
    uint32_t last_cause;
    uint64_t last_iova;
} iommu_user_domain_t;

/* One translation domain per claimed device.  A domain is owned by the
 * (devid, pid) pair that claimed it; different devices may be owned by
 * different processes at the same time, and each has its own page table, so
 * their IOVA spaces are independent.  The table is small and fixed: a user
 * driver claims a device, not an unbounded number of them. */
#define IOMMU_USER_DOMAINS 8

static iommu_user_domain_t g_user_domains[IOMMU_USER_DOMAINS];
static riscv_iommu_stats_t g_iommu_stats;
static unsigned g_native_owner_serial;

/* Caller holds g_iommu_domain_lock. */
static iommu_user_domain_t *iommu_domain_for(uint16_t devid, int owner_pid)
{
    for (unsigned i = 0; i < IOMMU_USER_DOMAINS; i++) {
        iommu_user_domain_t *d = &g_user_domains[i];
        if (d->active == 1 && d->devid == devid && d->owner_pid == owner_pid)
            return d;
    }
    return NULL;
}

/* Every live domain for a device, regardless of owner.  The fault queue is a
 * single hardware queue shared by all devices, so a drain started by one
 * owner still has to attribute records belonging to another. */
static iommu_user_domain_t *iommu_domain_by_devid(uint16_t devid)
{
    for (unsigned i = 0; i < IOMMU_USER_DOMAINS; i++) {
        iommu_user_domain_t *d = &g_user_domains[i];
        if (d->active == 1 && d->devid == devid)
            return d;
    }
    return NULL;
}

void riscv_iommu_get_stats(riscv_iommu_stats_t *out)
{
    if (!out)
        return;
    uint64_t flags = spin_lock_irqsave(&g_iommu_domain_lock);
    *out = g_iommu_stats;
    out->enabled = g_iommu_ready;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
}

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

static uint64_t *iommu_dc(uint16_t devid)
{
    if (!g_iommu_ddt || !g_iommu_dc_stride ||
        (uint32_t)devid >= 4096u / g_iommu_dc_stride)
        return NULL;
    return (uint64_t *)(g_iommu_ddt + (uint32_t)devid * g_iommu_dc_stride);
}

static int iommu_cmd(uint64_t dword0, uint64_t dword1)
{
    if (!g_iommu_cq)
        return -1;
    uint64_t flags = spin_lock_irqsave(&g_iommu_cq_lock);
    if (iommu_read32(IOMMU_REG_CQCSR) & IOMMU_QUEUE_ERROR) {
        spin_unlock_irqrestore(&g_iommu_cq_lock, flags);
        return -1;
    }
    uint32_t tail = g_iommu_cq_tail & IOMMU_CQ_MASK;
    uint32_t next = (tail + 1) & IOMMU_CQ_MASK;
    if ((iommu_read32(IOMMU_REG_CQH) & IOMMU_CQ_MASK) == next) {
        spin_unlock_irqrestore(&g_iommu_cq_lock, flags);
        return -1;
    }
    g_iommu_cq[tail * 2] = dword0;
    g_iommu_cq[tail * 2 + 1] = dword1;
    wmb();
    /* Ownership transfers on publication, not on observed completion. A
     * timed-out command may still be consumed later; never overwrite its
     * slot on retry. Queue-full admission protects all outstanding slots. */
    g_iommu_cq_tail = next;
    iommu_write32(IOMMU_REG_CQT, next);
    int done = 0;
    for (unsigned i = 0; i < 1000000; i++) {
        if ((iommu_read32(IOMMU_REG_CQH) & IOMMU_CQ_MASK) == next) {
            done = 1;
            break;
        }
    }
    uint32_t csr = iommu_read32(IOMMU_REG_CQCSR);
    if (!done || (csr & IOMMU_QUEUE_ERROR))
        kerr("[IOMMU] command failed op=0x%lx head=%u tail=%u csr=0x%x\n",
             (unsigned long)dword0,
             iommu_read32(IOMMU_REG_CQH) & IOMMU_CQ_MASK, next, csr);
    spin_unlock_irqrestore(&g_iommu_cq_lock, flags);
    return done && !(csr & IOMMU_QUEUE_ERROR) ? 0 : -1;
}

static int iommu_sync_context(uint16_t devid)
{
    if (iommu_cmd(IOMMU_CMD_IODIR | IOMMU_CMD_IODIR_DV |
                  ((uint64_t)devid << IOMMU_CMD_DID_SHIFT), 0) < 0)
        return -1;
    if (iommu_cmd(IOMMU_CMD_IOTINVAL, 0) < 0)
        return -1;
    return iommu_cmd(IOMMU_CMD_IOFENCE, 0);
}

static int iommu_context_block(uint16_t devid)
{
    uint64_t *dc = iommu_dc(devid);
    if (!dc)
        return -1;
    dc[0] = 0;
    wmb();
    return iommu_sync_context(devid);
}

static int iommu_context_translate(uint16_t devid, uint64_t root_phys,
                                   uint32_t pscid)
{
    uint64_t *dc = iommu_dc(devid);
    if (!dc || !pscid || iommu_context_block(devid) < 0)
        return -1;
    dc[1] = 0; /* iohgatp: BARE */
    dc[2] = (uint64_t)pscid << DC_TA_PSCID_SHIFT;
    dc[3] = (DC_FSC_MODE_SV39 << DC_FSC_MODE_SHIFT) | (root_phys >> 12);
    if (g_iommu_dc_stride == 64)
        memset(&dc[4], 0, 4 * sizeof(uint64_t));
    wmb();
    dc[0] = DC_TC_V;
    wmb();
    return iommu_sync_context(devid);
}

/*
 * Install a translating device context (SV39, one mapped page) for
 * devid 0 in the 1LVL DDT (32-byte base-format DC at offset 0) and
 * verify the translation with the TR_REQ debug interface: the mapped
 * IOVA must resolve to the expected physical page and an unmapped
 * IOVA must fault — hardware-observable enforcement.
 */
static int riscv_iommu_verify_translation(void)
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
    l0[0] = (data_phys >> 12) << IOMMU_PTE_PPN_SHIFT |
            IOMMU_PTE_V | IOMMU_PTE_R | IOMMU_PTE_W | IOMMU_PTE_U |
            IOMMU_PTE_A | IOMMU_PTE_D;
    l1[0x80] = (l0_phys >> 12) << IOMMU_PTE_PPN_SHIFT | IOMMU_PTE_V;
    root[0] = (l1_phys >> 12) << IOMMU_PTE_PPN_SHIFT | IOMMU_PTE_V;
    kinfo("[IOMMU] pt root@0x%lx l1@0x%lx l0@0x%lx data@0x%lx ptes: "
          "r0=0x%08lx%08lx l1[0x80]=0x%08lx%08lx l0[0]=0x%08lx%08lx\n",
          (unsigned long)root_phys, (unsigned long)l1_phys,
          (unsigned long)l0_phys, (unsigned long)data_phys,
          (unsigned long)(root[0] >> 32), (unsigned long)(uint32_t)root[0],
          (unsigned long)(l1[0x80] >> 32), (unsigned long)(uint32_t)l1[0x80],
          (unsigned long)(l0[0] >> 32), (unsigned long)(uint32_t)l0[0]);

    /* Translating device context for devid 0 (SV39, no process context). */
    if (iommu_context_translate(0, root_phys, IOMMU_USER_DOMAINS + 1) < 0)
        return -1;

    /* TR_REQ: mapped IOVA must translate to data_phys (RO request). */
    iommu_write64(IOMMU_REG_TR_REQ_IOVA, IOMMU_PROBE_IOVA);
    iommu_write64(IOMMU_REG_TR_REQ_CTL,
                  IOMMU_TR_CTL_GO | IOMMU_TR_CTL_NW);
    uint64_t resp = iommu_read64(IOMMU_REG_TR_RESPONSE);
    /* Spec 1.0: TR_RESPONSE.PPN is bits [53:10] holding ppn << 10.
     * QEMU <= 10.0 stores (ppn & field_mask) without the shift (fixed on
     * master by set_field()).  The unshifted encoding is accepted only when
     * CONFIG_IOMMU_TRRESP_LEGACY_PPN is set, so the exception is explicit and
     * can be dropped once the required QEMU baseline no longer needs it. */
    uint64_t expect_field = (data_phys >> 2) & IOMMU_TR_RESP_PPN;
    uint64_t legacy_field = (data_phys >> 12) & IOMMU_TR_RESP_PPN;
    uint64_t got_field = resp & IOMMU_TR_RESP_PPN;
#ifdef CONFIG_IOMMU_TRRESP_LEGACY_PPN
    int legacy_ok = (got_field == legacy_field);
#else
    int legacy_ok = 0;
#endif
    kinfo("[IOMMU] TR_REQ mapped iova=0x%lx -> field=0x%08lx%08lx "
          "(expected 0x%08lx%08lx) fault=%u raw=0x%08lx%08lx\n",
          (unsigned long)IOMMU_PROBE_IOVA,
          (unsigned long)(got_field >> 32), (unsigned long)(uint32_t)got_field,
          (unsigned long)(expect_field >> 32), (unsigned long)(uint32_t)expect_field,
          !!(resp & IOMMU_TR_RESP_FAULT),
          (unsigned long)(resp >> 32), (unsigned long)(uint32_t)resp);
    if ((resp & IOMMU_TR_RESP_FAULT) ||
        (got_field != expect_field && !legacy_ok)) {
        kerr("[IOMMU] TR_REQ mapped translation mismatch\n");
        return -1;
    }
    if (legacy_ok && got_field != expect_field)
        kinfo("[IOMMU] TR_REQ uses legacy (QEMU <= 10.0) PPN encoding\n");

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
    g_iommu_base = (uintptr_t)pci_map_bar(dev, 0);
    if (!g_iommu_base) return -EIO;
    kinfo("[IOMMU] riscv-iommu: BAR0=0x%lx..0x%lx\n",
          (unsigned long)bar->start, (unsigned long)bar->end);

    uint64_t cap = iommu_read64(IOMMU_REG_CAP);
    uint32_t fctl = iommu_read32(IOMMU_REG_FCTL);
    kinfo("[IOMMU] capabilities=0x%08lx%08lx features=0x%08x (version=%u)\n",
          (unsigned long)(cap >> 32), (unsigned long)(uint32_t)cap,
          fctl, (unsigned)(cap & 0xff));
    if (!(cap & IOMMU_CAP_SV39)) {
        kerr("[IOMMU] SV39 translation is unavailable\n");
        return -1;
    }

    /* A previous failed probe may have left translation and queues running
     * with non-zero head/tail indices; the queue head/tail registers only
     * reset on an off->on transition.  Disable translation and both queues
     * first and wait for the hardware to go quiescent. */
    iommu_write64(IOMMU_REG_DDTP, 0);
    iommu_write32(IOMMU_REG_CQCSR, 0);
    iommu_write32(IOMMU_REG_FQCSR, 0);
    int quiesced = 0;
    for (unsigned i = 0; i < 1000000; i++) {
        if (!(iommu_read32(IOMMU_REG_CQCSR) &
              (IOMMU_QUEUE_ACTIVE | IOMMU_QUEUE_BUSY)) &&
            !(iommu_read32(IOMMU_REG_FQCSR) &
              (IOMMU_QUEUE_ACTIVE | IOMMU_QUEUE_BUSY))) {
            quiesced = 1;
            break;
        }
    }
    if (!quiesced) {
        kerr("[IOMMU] queues did not quiesce; aborting probe\n");
        return -1;
    }

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

    /* MSI_FLAT selects the 64-byte extended DC; otherwise DCs are 32 bytes.
     * Start every representable requester in BARE mode to preserve existing
     * kernel PCI drivers.  A user-owned requester is changed to translating
     * mode before bus mastering is enabled for its process. */
    g_iommu_dc_stride = (cap & IOMMU_CAP_MSI_FLAT) ? 64u : 32u;
    for (uint32_t off = 0; off < 4096; off += g_iommu_dc_stride)
        *(uint64_t *)((uint8_t *)ddt_va + off) = DC_TC_V;

    g_iommu_ddt = ddt_va;
    g_iommu_cq = cq_va;
    g_iommu_fq = fq_va;
    g_iommu_cq_tail = 0;

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

    /* Hardware-observable baseline; the runtime API below uses the same
     * context and page-table format for the real PCI requester ID. */
    if (riscv_iommu_verify_translation() != 0) {
        kerr("[IOMMU] translation verification failed\n");
        return -1;
    }
    g_iommu_ready = 1;
    return 0;
}

static int domain_claim(uint16_t devid, int owner_pid)
{
    if (!g_iommu_ready || !owner_pid || !iommu_dc(devid))
        return -1;

retry:
    ; /* a label precedes a statement in C99 */
    uint64_t flags = spin_lock_irqsave(&g_iommu_domain_lock);
    iommu_user_domain_t *existing = iommu_domain_for(devid, owner_pid);
    if (existing) {
        int failed = existing->claim_failed;
        int blocked = existing->blocked;
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        if (failed) {
            if (riscv_iommu_domain_release(devid, owner_pid) < 0)
                return -1;
            goto retry;
        }
        return blocked ? -1 : 0; /* only healthy claims are idempotent */
    }
    for (unsigned i = 0; i < IOMMU_USER_DOMAINS; i++) {
        if (g_user_domains[i].active != 0 && g_user_domains[i].devid == devid) {
            spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
            return -1;                  /* already owned by someone else */
        }
    }
    iommu_user_domain_t *d = NULL;
    for (unsigned i = 0; i < IOMMU_USER_DOMAINS; i++) {
        if (g_user_domains[i].active == 0) {
            d = &g_user_domains[i];
            break;
        }
    }
    if (!d) {
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }
    d->active = -1;                     /* reserved; not usable until DC is live */
    d->devid = devid;
    d->owner_pid = owner_pid;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);

    pfn_t root_pfn = pfa_alloc_page();
    pfn_t l1_pfn = pfa_alloc_page();
    pfn_t l0_pfn = pfa_alloc_page();
    if (root_pfn == PFN_NONE || l1_pfn == PFN_NONE || l0_pfn == PFN_NONE) {
        if (root_pfn != PFN_NONE) pfa_free_page(root_pfn);
        if (l1_pfn != PFN_NONE) pfa_free_page(l1_pfn);
        if (l0_pfn != PFN_NONE) pfa_free_page(l0_pfn);
        flags = spin_lock_irqsave(&g_iommu_domain_lock);
        memset(d, 0, sizeof(*d));
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }

    uint64_t *root = pfn_to_virt(root_pfn);
    uint64_t *l1 = pfn_to_virt(l1_pfn);
    uint64_t *l0 = pfn_to_virt(l0_pfn);
    memset(root, 0, PAGE_SIZE);
    memset(l1, 0, PAGE_SIZE);
    memset(l0, 0, PAGE_SIZE);
    uint32_t vpn2 = (uint32_t)((IOMMU_USER_IOVA >> 30) & 0x1ff);
    uint32_t vpn1 = (uint32_t)((IOMMU_USER_IOVA >> 21) & 0x1ff);
    root[vpn2] = ((uint64_t)pfn_to_phys(l1_pfn) >> 12)
                 << IOMMU_PTE_PPN_SHIFT | IOMMU_PTE_V;
    l1[vpn1] = ((uint64_t)pfn_to_phys(l0_pfn) >> 12)
               << IOMMU_PTE_PPN_SHIFT | IOMMU_PTE_V;
    wmb();

    /* Record ownership before publishing a context that can reference these
     * page tables. Failed context invalidation is not permission to free them. */
    flags = spin_lock_irqsave(&g_iommu_domain_lock);
    d->root_pfn = root_pfn;
    d->l1_pfn = l1_pfn;
    d->l0_pfn = l0_pfn;
    d->l0 = l0;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
    uint32_t pscid = (uint32_t)(d - g_user_domains) + 1U;
    if (iommu_context_translate(devid, pfn_to_phys(root_pfn), pscid) < 0) {
        int safe = iommu_context_block(devid) == 0;
        flags = spin_lock_irqsave(&g_iommu_domain_lock);
        if (safe)
            memset(d, 0, sizeof(*d));
        else {
            d->active = 1;
            d->blocked = d->claim_failed = 1;
        }
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        if (safe) {
            pfa_free_page(root_pfn);
            pfa_free_page(l1_pfn);
            pfa_free_page(l0_pfn);
        }
        return -1;
    }

    flags = spin_lock_irqsave(&g_iommu_domain_lock);
    d->active = 1;
    g_iommu_stats.domains_claimed++;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
    kinfo("[IOMMU] user domain attached did=%u owner=%d\n",
          devid, owner_pid);
    return 0;
}

int riscv_iommu_domain_claim(uint16_t devid, int owner_pid)
{
    return owner_pid > 0 ? domain_claim(devid, owner_pid) : -1;
}

static int domain_map(uint16_t devid, int owner_pid, uint64_t phys,
                        uint32_t npages, uint64_t *out_iova, bool *needs_unmap,
                        enum driver_dma_direction direction)
{
    if (!out_iova)
        return -1;
    *out_iova = 0;
    *needs_unmap = false;
    if (!npages || npages > IOMMU_USER_MAX_PAGES ||
        (phys & (PAGE_SIZE - 1)) ||
        phys > ~(uint64_t)0 - ((uint64_t)npages * PAGE_SIZE - 1))
        return -1;
    pfn_t pages[IOMMU_USER_MAX_PAGES];
    for (uint32_t i = 0; i < npages; i++) {
        uint64_t pa = phys + (uint64_t)i * PAGE_SIZE;
        pages[i] = phys_to_pfn(pa);
        if (!pfn_valid(pages[i]) || pfn_to_phys(pages[i]) != pa)
            return -1;
    }
    uint64_t flags = spin_lock_irqsave(&g_iommu_domain_lock);
    iommu_user_domain_t *d = iommu_domain_for(devid, owner_pid);
    if (!d || d->blocked) {
        g_iommu_stats.map_failures++;
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }
    uint64_t run = npages == 64 ? ~0ull : (1ull << npages) - 1;
    uint32_t first = 0;
    while (first + npages <= IOMMU_USER_MAX_PAGES &&
           (d->mapped_pages & (run << first)))
        first++;
    if (first + npages > IOMMU_USER_MAX_PAGES) {
        g_iommu_stats.map_failures++;
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }
    for (uint32_t i = 0; i < npages; i++) {
        frame_get(pages[i]);
        d->pinned[first + i] = pages[i];
        d->l0[first + i] = ((phys + (uint64_t)i * PAGE_SIZE) >> 12)
                           << IOMMU_PTE_PPN_SHIFT |
                           IOMMU_PTE_V | IOMMU_PTE_R | IOMMU_PTE_U | IOMMU_PTE_A |
                           (direction == DRIVER_DMA_TO_DEVICE ? 0 : IOMMU_PTE_W | IOMMU_PTE_D);
    }
    d->mapped_pages |= run << first;
    g_iommu_stats.mapped_pages += npages;
    *out_iova = IOMMU_USER_IOVA + (uint64_t)first * PAGE_SIZE;
    *needs_unmap = true;
    wmb();
    if (iommu_cmd(IOMMU_CMD_IOTINVAL, 0) < 0 ||
        iommu_cmd(IOMMU_CMD_IOFENCE, 0) < 0) {
        for (uint32_t i = 0; i < npages; i++)
            d->l0[first + i] = 0;
        wmb();
        /* Hardware may have cached these PTEs before the command timed out.
         * Keep the slots and pins until successful domain block/release. */
        d->blocked = 1;
        g_iommu_stats.map_failures++;
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }
    g_iommu_stats.maps++;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
    return 0;
}

int riscv_iommu_domain_map(uint16_t devid, int owner_pid, uint64_t phys,
                           uint32_t npages, uint64_t *out_iova)
{
    bool pending;
    int ret = domain_map(devid, owner_pid, phys, npages, out_iova, &pending,
                           DRIVER_DMA_BIDIRECTIONAL);
    if (ret && out_iova)
        *out_iova = 0; /* Legacy API owns quarantine internally on failure. */
    return ret;
}

int riscv_iommu_domain_unmap(uint16_t devid, int owner_pid, uint64_t iova,
                             uint32_t npages)
{
    if (!npages || npages > IOMMU_USER_MAX_PAGES || iova < IOMMU_USER_IOVA ||
        ((iova - IOMMU_USER_IOVA) & (PAGE_SIZE - 1)) ||
        (iova - IOMMU_USER_IOVA) / PAGE_SIZE > IOMMU_USER_MAX_PAGES - npages)
        return -1;
    uint32_t first = (uint32_t)((iova - IOMMU_USER_IOVA) / PAGE_SIZE);
    uint64_t flags = spin_lock_irqsave(&g_iommu_domain_lock);
    iommu_user_domain_t *d = iommu_domain_for(devid, owner_pid);
    uint64_t run = npages == 64 ? ~0ull : (1ull << npages) - 1;
    uint64_t mask = first < IOMMU_USER_MAX_PAGES ? run << first : 0;
    if (!d || first + npages > IOMMU_USER_MAX_PAGES ||
        (d->mapped_pages & mask) != mask) {
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }
    for (uint32_t i = 0; i < npages; i++)
        d->l0[first + i] = 0;
    wmb();
    int r = iommu_cmd(IOMMU_CMD_IOTINVAL, 0) < 0 ||
            iommu_cmd(IOMMU_CMD_IOFENCE, 0) < 0 ? -1 : 0;
    if (r == 0) {
        d->mapped_pages &= ~mask;
        g_iommu_stats.unmaps++;
        g_iommu_stats.mapped_pages -= npages;
        for (uint32_t i = 0; i < npages; i++)
            frame_put(d->pinned[first + i]);
    }
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
    return r;
}

/* Drain the shared fault queue and attribute each record to the domain that
 * owns its device.  A drain is triggered by one owner's get_info(), but the
 * records are not filtered by that owner: the queue is per IOMMU, so a record
 * for device B can be at the head when owner A polls.  Attributing it to A (or
 * discarding it) would leave B running with a stale translation and no report,
 * which is the opposite of fail-closed. */
int riscv_iommu_domain_fault(uint16_t devid, int owner_pid,
                            uint64_t *count, uint32_t *cause,
                            uint64_t *iova, int *blocked)
{
    struct {
        uint16_t devid;
        uint32_t cause;
        uint64_t iova;
    } events[IOMMU_USER_DOMAINS];
    unsigned n_events = 0;

    uint64_t flags = spin_lock_irqsave(&g_iommu_domain_lock);
    iommu_user_domain_t *d = iommu_domain_for(devid, owner_pid);
    if (!d) {
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }

    uint32_t head = iommu_read32(IOMMU_REG_FQH) & IOMMU_FQ_MASK;
    uint32_t tail = iommu_read32(IOMMU_REG_FQT) & IOMMU_FQ_MASK;
    while (head != tail) {
        rmb();
        uint64_t *record = &g_iommu_fq[head * 4];
        uint64_t hdr = record[0];
        uint16_t record_devid = (uint16_t)(hdr >> 40);
        g_iommu_stats.fault_records++;
        iommu_user_domain_t *owner = iommu_domain_by_devid(record_devid);
        if (owner) {
            owner->fault_count++;
            owner->last_cause = (uint32_t)(hdr & 0xfff);
            owner->last_iova = record[2];
            g_iommu_stats.faults++;
            g_iommu_stats.last_fault_devid = record_devid;
            g_iommu_stats.last_fault_cause = owner->last_cause;
            g_iommu_stats.last_fault_iova = owner->last_iova;
            g_iommu_stats.last_fault_owner = owner->owner_pid;
            if (!owner->blocked) {
                owner->blocked = 1;
                g_iommu_stats.blocked_events++;
                if (n_events < IOMMU_USER_DOMAINS) {
                    events[n_events].devid = record_devid;
                    events[n_events].cause = owner->last_cause;
                    events[n_events].iova = owner->last_iova;
                    n_events++;
                }
            }
        }
        head = (head + 1) & IOMMU_FQ_MASK;
    }
    iommu_write32(IOMMU_REG_FQH, head);
    if (count) *count = d->fault_count;
    if (cause) *cause = d->last_cause;
    if (iova) *iova = d->last_iova;
    if (blocked) *blocked = d->blocked;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);

    /* Block the device at fault time, not on the caller's next poll:
     * invalidating the device context alone still leaves bus mastering
     * enabled, so the device could keep issuing DMA. */
    for (unsigned i = 0; i < n_events; i++) {
        extern int pci_user_device_bus_master(uint16_t devid, int enable);
        pci_user_device_bus_master(events[i].devid, 0);
        (void)iommu_context_block(events[i].devid);
        kinfo("[IOMMU] DMA fault blocked did=%u cause=%u iova=0x%lx\n",
              events[i].devid, events[i].cause,
              (unsigned long)events[i].iova);
    }
    return 0;
}

int riscv_iommu_domain_release(uint16_t devid, int owner_pid)
{
    uint64_t flags = spin_lock_irqsave(&g_iommu_domain_lock);
    iommu_user_domain_t *d = iommu_domain_for(devid, owner_pid);
    if (!d) {
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }
    d->active = -1;
    pfn_t root_pfn = d->root_pfn;
    pfn_t l1_pfn = d->l1_pfn;
    pfn_t l0_pfn = d->l0_pfn;
    uint64_t mapped = d->mapped_pages;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);

    if (iommu_context_block(devid) < 0) {
        flags = spin_lock_irqsave(&g_iommu_domain_lock);
        d->active = 1;
        d->blocked = 1;
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }
    for (unsigned i = 0; i < IOMMU_USER_MAX_PAGES; i++)
        if (mapped & (1ull << i))
            frame_put(d->pinned[i]);
    pfa_free_page(root_pfn);
    pfa_free_page(l1_pfn);
    pfa_free_page(l0_pfn);
    flags = spin_lock_irqsave(&g_iommu_domain_lock);
    g_iommu_stats.domains_released++;
    g_iommu_stats.mapped_pages = g_iommu_stats.mapped_pages >= mapped
                                 ? g_iommu_stats.mapped_pages - mapped : 0;
    memset(d, 0, sizeof(*d));
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
    kinfo("[IOMMU] user domain released did=%u owner=%d\n",
          devid, owner_pid);
    return 0;
}

struct native_iommu_context {
    device_t *device; /* retained by the managed domain */
    uint16_t devid;
    int owner;
};

static int native_map(void *opaque, uint64_t physical,
                        enum driver_dma_direction direction, uint64_t *address,
                        bool *needs_unmap)
{
    struct native_iommu_context *context = opaque;
    return domain_map(context->devid, context->owner, physical, 1, address,
                        needs_unmap, direction) ? -EIO : 0;
}

static int native_unmap(void *opaque, uint64_t address,
                          enum driver_dma_direction direction)
{
    (void)direction;
    struct native_iommu_context *context = opaque;
    return riscv_iommu_domain_unmap(context->devid, context->owner, address, 1)
           ? -EIO : 0;
}

static int native_deactivate(void *opaque)
{
    struct native_iommu_context *context = opaque;
    if (pci_cfg_read16(context->device, 0x04) & 4)
        return -EBUSY; /* Caller must stop DMA and disable bus mastering. */
    uint64_t flags = spin_lock_irqsave(&g_iommu_domain_lock);
    bool present = iommu_domain_for(context->devid, context->owner) != NULL;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
    if (!present)
        return 0; /* A failed claim may have left no hardware resources. */
    return riscv_iommu_domain_release(context->devid, context->owner) ? -EIO : 0;
}

static void native_release(void *opaque)
{
    kfree(opaque);
}

int riscv_iommu_dma_domain_create(device_t *device, struct driver_dma_domain **out)
{
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!device)
        return -EINVAL;
    if (!g_iommu_ready)
        return -ENODEV;
    if (device->user_owned)
        return -EBUSY;
    unsigned bus, slot, function;
    int ret = pci_device_location(device, &bus, &slot, &function);
    if (ret)
        return ret;
    if (bus > 255 || slot > 31 || function > 7)
        return -EINVAL;
    uint16_t devid = (bus << 8) | (slot << 3) | function;
    if (!iommu_dc(devid))
        return -EOPNOTSUPP;
    if (pci_cfg_read16(device, 0x04) & 4)
        return -EBUSY;
    struct native_iommu_context *context = kmalloc(sizeof(*context));
    if (!context)
        return -ENOMEM;
    uint64_t flags = spin_lock_irqsave(&g_iommu_domain_lock);
    if (g_native_owner_serial == 0x7fffffffU) {
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        kfree(context);
        return -ENOSPC;
    }
    int owner = -(int)++g_native_owner_serial;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
    *context = (struct native_iommu_context){device, devid, owner};
    const struct driver_dma_managed_ops ops = {
        native_map, native_unmap, native_deactivate, native_release,
    };
    struct driver_dma_domain *domain;
    ret = driver_dma_domain_create_managed(device, &ops, context, &domain);
    if (ret) {
        kfree(context);
        return ret;
    }
    if (domain_claim(devid, owner)) {
        /* Even a failed claim can leave hardware referencing page tables.
         * Preserve a closed managed handle if cleanup cannot be confirmed. */
        if (driver_dma_domain_shutdown(domain))
            *out = domain;
        else
            driver_dma_domain_put(domain);
        return -EIO;
    }
    *out = domain;
    return 0;
}

static uint64_t native_test_translation(uint16_t devid, uint64_t address, bool read)
{
    iommu_write64(IOMMU_REG_TR_REQ_IOVA, address);
    /* DID is [63:40]. Publish the high half before the low GO doorbell. */
    iommu_write32(IOMMU_REG_TR_REQ_CTL + 4, (uint32_t)devid << 8);
    iommu_write32(IOMMU_REG_TR_REQ_CTL, IOMMU_TR_CTL_GO | (read ? IOMMU_TR_CTL_NW : 0));
    for (unsigned i = 0; i < 1000000; i++)
        if (!(iommu_read32(IOMMU_REG_TR_REQ_CTL) & IOMMU_TR_CTL_GO))
            return iommu_read64(IOMMU_REG_TR_RESPONSE);
    panic("DRIVER_IOMMU_NATIVE: translation timeout");
}

void driver_iommu_native_selftest(void)
{
#define IOMMU_CHECK(test) do { if (!(test)) \
    panic("DRIVER_IOMMU_NATIVE: FAIL line=%d: %s", __LINE__, #test); } while (0)
    pci_user_device_info_t info;
    IOMMU_CHECK(!pci_user_device_find(0x1234, 0x11e8, &info));
    device_t *device = device_find_get_by_mmio(info.bar0_phys);
    IOMMU_CHECK(device && !device->drv && !device->user_owned);
    uint16_t command = pci_cfg_read16(device, 4);
    pci_cfg_write16(device, 4, command & ~4U);
    struct driver_dma_domain *domain;
    IOMMU_CHECK(!riscv_iommu_dma_domain_create(device, &domain));
    pfn_t pages[2] = {pfa_alloc_page(), pfa_alloc_page()};
    IOMMU_CHECK(pages[0] != PFN_NONE && pages[1] != PFN_NONE);
    uint64_t physical[2] = {pfn_to_phys(pages[0]), pfn_to_phys(pages[1])};
    struct driver_dma_mapping *mapping;
    IOMMU_CHECK(!driver_dma_map_pages(domain, pages, 2, DRIVER_DMA_TO_DEVICE, &mapping));
    frame_put(pages[0]); frame_put(pages[1]);
    uint64_t address = 0;
    for (unsigned i = 0; i < 2; i++) {
        IOMMU_CHECK(!driver_dma_page_address(mapping, i, &address));
        IOMMU_CHECK(address != physical[i]);
        uint64_t response = native_test_translation(info.devid, address, true);
        uint64_t ppn = response & IOMMU_TR_RESP_PPN;
        bool matches = ppn == ((physical[i] >> 2) & IOMMU_TR_RESP_PPN);
#ifdef CONFIG_IOMMU_TRRESP_LEGACY_PPN
        matches |= ppn == ((physical[i] >> 12) & IOMMU_TR_RESP_PPN);
#endif
        IOMMU_CHECK(!(response & IOMMU_TR_RESP_FAULT) && matches);
        IOMMU_CHECK(native_test_translation(info.devid, address, false) & IOMMU_TR_RESP_FAULT);
    }
    IOMMU_CHECK(!driver_dma_try_unmap_pages(&mapping) && !mapping);
    IOMMU_CHECK(native_test_translation(info.devid, address, true) & IOMMU_TR_RESP_FAULT);
    IOMMU_CHECK(!driver_dma_domain_shutdown(domain));
    IOMMU_CHECK(native_test_translation(info.devid, address, true) & IOMMU_TR_RESP_FAULT);
    driver_dma_domain_put(domain);
    pci_cfg_write16(device, 4, command);
    device_put(device);
    kinfo("DRIVER_IOMMU_NATIVE: PASS map=1 translation=1 readonly=1 unmap=1 detach=1\n");
#undef IOMMU_CHECK
}

static int riscv_iommu_remove(device_t *dev)
{
    (void)dev;
    g_iommu_ready = 0;
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
