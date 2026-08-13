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
 *     IOVA map/unmap, context/IOTLB invalidation and fault-queue cleanup.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/pci_hal.h"
#include "drivers/core/riscv_iommu.h"
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
#define IOMMU_QUEUE_ERROR    ((1u << 8) | (1u << 9) | (1u << 10))

#define IOMMU_CAP_SV39      (1ull << 9)
#define IOMMU_CAP_MSI_FLAT  (1ull << 22)

/* DDTE / DC formats. */
#define DDTE_VALID          (1ull << 0)
#define DC_TC_V             (1ull << 0)
#define DC_FSC_MODE_SV39    8ull
#define DC_FSC_MODE_SHIFT   60ull

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
    pfn_t root_pfn;
    pfn_t l1_pfn;
    pfn_t l0_pfn;
    uint64_t *l0;
    uint64_t mapped_pages;
    uint64_t fault_count;
    uint32_t last_cause;
    uint64_t last_iova;
} iommu_user_domain_t;

static iommu_user_domain_t g_user_domain;

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
    uint32_t tail = g_iommu_cq_tail & IOMMU_CQ_MASK;
    uint32_t next = (tail + 1) & IOMMU_CQ_MASK;
    if ((iommu_read32(IOMMU_REG_CQH) & IOMMU_CQ_MASK) == next) {
        spin_unlock_irqrestore(&g_iommu_cq_lock, flags);
        return -1;
    }
    g_iommu_cq[tail * 2] = dword0;
    g_iommu_cq[tail * 2 + 1] = dword1;
    wmb();
    iommu_write32(IOMMU_REG_CQT, next);
    int done = 0;
    for (unsigned i = 0; i < 1000000; i++) {
        if ((iommu_read32(IOMMU_REG_CQH) & IOMMU_CQ_MASK) == next) {
            done = 1;
            break;
        }
    }
    uint32_t csr = iommu_read32(IOMMU_REG_CQCSR);
    if (done && !(csr & IOMMU_QUEUE_ERROR))
        g_iommu_cq_tail = next;
    else
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

static int iommu_context_translate(uint16_t devid, uint64_t root_phys)
{
    uint64_t *dc = iommu_dc(devid);
    if (!dc || iommu_context_block(devid) < 0)
        return -1;
    dc[1] = 0; /* iohgatp: BARE */
    dc[2] = 0; /* ta */
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
    if (iommu_context_translate(0, root_phys) < 0)
        return -1;

    /* TR_REQ: mapped IOVA must translate to data_phys (RO request). */
    iommu_write64(IOMMU_REG_TR_REQ_IOVA, IOMMU_PROBE_IOVA);
    iommu_write64(IOMMU_REG_TR_REQ_CTL,
                  IOMMU_TR_CTL_GO | IOMMU_TR_CTL_NW);
    uint64_t resp = iommu_read64(IOMMU_REG_TR_RESPONSE);
    uint64_t expect_field = (data_phys >> 2) & IOMMU_TR_RESP_PPN;
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
    if (!(cap & IOMMU_CAP_SV39)) {
        kerr("[IOMMU] SV39 translation is unavailable\n");
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

int riscv_iommu_domain_claim(uint16_t devid, int owner_pid)
{
    if (!g_iommu_ready || owner_pid <= 0 || !iommu_dc(devid))
        return -1;

    pfn_t root_pfn = pfa_alloc_page();
    pfn_t l1_pfn = pfa_alloc_page();
    pfn_t l0_pfn = pfa_alloc_page();
    if (root_pfn == PFN_NONE || l1_pfn == PFN_NONE || l0_pfn == PFN_NONE) {
        if (root_pfn != PFN_NONE) pfa_free_page(root_pfn);
        if (l1_pfn != PFN_NONE) pfa_free_page(l1_pfn);
        if (l0_pfn != PFN_NONE) pfa_free_page(l0_pfn);
        return -1;
    }

    uint64_t flags = spin_lock_irqsave(&g_iommu_domain_lock);
    if (g_user_domain.active) {
        int same = g_user_domain.active == 1 && !g_user_domain.blocked &&
                   g_user_domain.devid == devid &&
                   g_user_domain.owner_pid == owner_pid;
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        pfa_free_page(root_pfn);
        pfa_free_page(l1_pfn);
        pfa_free_page(l0_pfn);
        return same ? 0 : -1;
    }
    g_user_domain.active = -1; /* reserved; not usable until DC is live */
    g_user_domain.devid = devid;
    g_user_domain.owner_pid = owner_pid;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);

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

    if (iommu_context_translate(devid, pfn_to_phys(root_pfn)) < 0) {
        flags = spin_lock_irqsave(&g_iommu_domain_lock);
        memset(&g_user_domain, 0, sizeof(g_user_domain));
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        pfa_free_page(root_pfn);
        pfa_free_page(l1_pfn);
        pfa_free_page(l0_pfn);
        return -1;
    }

    flags = spin_lock_irqsave(&g_iommu_domain_lock);
    g_user_domain.root_pfn = root_pfn;
    g_user_domain.l1_pfn = l1_pfn;
    g_user_domain.l0_pfn = l0_pfn;
    g_user_domain.l0 = l0;
    g_user_domain.mapped_pages = 0;
    g_user_domain.fault_count = 0;
    g_user_domain.last_cause = 0;
    g_user_domain.last_iova = 0;
    g_user_domain.blocked = 0;
    g_user_domain.active = 1;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
    kinfo("[IOMMU] user domain attached did=%u owner=%d\n",
          devid, owner_pid);
    return 0;
}

int riscv_iommu_domain_map(uint16_t devid, int owner_pid, uint64_t phys,
                           uint32_t npages, uint64_t *out_iova)
{
    if (!out_iova || !npages || npages > IOMMU_USER_MAX_PAGES ||
        (phys & (PAGE_SIZE - 1)))
        return -1;
    uint64_t flags = spin_lock_irqsave(&g_iommu_domain_lock);
    iommu_user_domain_t *d = &g_user_domain;
    if (d->active != 1 || d->blocked || d->devid != devid ||
        d->owner_pid != owner_pid) {
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }
    uint64_t run = npages == 64 ? ~0ull : (1ull << npages) - 1;
    uint32_t first = 0;
    while (first + npages <= IOMMU_USER_MAX_PAGES &&
           (d->mapped_pages & (run << first)))
        first++;
    if (first + npages > IOMMU_USER_MAX_PAGES) {
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }
    for (uint32_t i = 0; i < npages; i++)
        d->l0[first + i] = ((phys + (uint64_t)i * PAGE_SIZE) >> 12)
                           << IOMMU_PTE_PPN_SHIFT |
                           IOMMU_PTE_V | IOMMU_PTE_R | IOMMU_PTE_W |
                           IOMMU_PTE_U | IOMMU_PTE_A | IOMMU_PTE_D;
    wmb();
    if (iommu_cmd(IOMMU_CMD_IOTINVAL, 0) < 0 ||
        iommu_cmd(IOMMU_CMD_IOFENCE, 0) < 0) {
        for (uint32_t i = 0; i < npages; i++)
            d->l0[first + i] = 0;
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }
    d->mapped_pages |= run << first;
    *out_iova = IOMMU_USER_IOVA + (uint64_t)first * PAGE_SIZE;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
    return 0;
}

int riscv_iommu_domain_unmap(uint16_t devid, int owner_pid, uint64_t iova,
                             uint32_t npages)
{
    if (!npages || iova < IOMMU_USER_IOVA ||
        ((iova - IOMMU_USER_IOVA) & (PAGE_SIZE - 1)))
        return -1;
    uint32_t first = (uint32_t)((iova - IOMMU_USER_IOVA) / PAGE_SIZE);
    uint64_t flags = spin_lock_irqsave(&g_iommu_domain_lock);
    iommu_user_domain_t *d = &g_user_domain;
    uint64_t run = npages == 64 ? ~0ull : (1ull << npages) - 1;
    uint64_t mask = first < IOMMU_USER_MAX_PAGES ? run << first : 0;
    if (d->active != 1 || d->devid != devid || d->owner_pid != owner_pid ||
        first + npages > IOMMU_USER_MAX_PAGES ||
        (d->mapped_pages & mask) != mask) {
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }
    for (uint32_t i = 0; i < npages; i++)
        d->l0[first + i] = 0;
    wmb();
    int r = iommu_cmd(IOMMU_CMD_IOTINVAL, 0) < 0 ||
            iommu_cmd(IOMMU_CMD_IOFENCE, 0) < 0 ? -1 : 0;
    if (r == 0)
        d->mapped_pages &= ~mask;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
    return r;
}

int riscv_iommu_domain_fault(uint16_t devid, int owner_pid,
                            uint64_t *count, uint32_t *cause,
                            uint64_t *iova, int *blocked)
{
    uint64_t flags = spin_lock_irqsave(&g_iommu_domain_lock);
    iommu_user_domain_t *d = &g_user_domain;
    if (d->active != 1 || d->devid != devid || d->owner_pid != owner_pid) {
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }

    uint32_t head = iommu_read32(IOMMU_REG_FQH) & IOMMU_FQ_MASK;
    uint32_t tail = iommu_read32(IOMMU_REG_FQT) & IOMMU_FQ_MASK;
    int new_fault = 0;
    while (head != tail) {
        rmb();
        uint64_t *record = &g_iommu_fq[head * 4];
        uint64_t hdr = record[0];
        uint16_t record_devid = (uint16_t)(hdr >> 40);
        if (record_devid == devid) {
            d->fault_count++;
            d->last_cause = (uint32_t)(hdr & 0xfff);
            d->last_iova = record[2];
            new_fault = 1;
        }
        head = (head + 1) & IOMMU_FQ_MASK;
    }
    iommu_write32(IOMMU_REG_FQH, head);
    if (new_fault)
        d->blocked = 1;
    uint32_t report_cause = d->last_cause;
    uint64_t report_iova = d->last_iova;
    if (count) *count = d->fault_count;
    if (cause) *cause = d->last_cause;
    if (iova) *iova = d->last_iova;
    if (blocked) *blocked = d->blocked;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);

    if (new_fault) {
        if (iommu_context_block(devid) < 0)
            return -1;
        kinfo("[IOMMU] DMA fault blocked did=%u cause=%u iova=0x%lx\n",
              devid, report_cause, (unsigned long)report_iova);
    }
    return 0;
}

int riscv_iommu_domain_release(uint16_t devid, int owner_pid)
{
    uint64_t flags = spin_lock_irqsave(&g_iommu_domain_lock);
    iommu_user_domain_t *d = &g_user_domain;
    if (d->active != 1 || d->devid != devid || d->owner_pid != owner_pid) {
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }
    d->active = -1;
    pfn_t root_pfn = d->root_pfn;
    pfn_t l1_pfn = d->l1_pfn;
    pfn_t l0_pfn = d->l0_pfn;
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);

    if (iommu_context_block(devid) < 0) {
        flags = spin_lock_irqsave(&g_iommu_domain_lock);
        d->active = 1;
        d->blocked = 1;
        spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
        return -1;
    }
    pfa_free_page(root_pfn);
    pfa_free_page(l1_pfn);
    pfa_free_page(l0_pfn);
    flags = spin_lock_irqsave(&g_iommu_domain_lock);
    memset(d, 0, sizeof(*d));
    spin_unlock_irqrestore(&g_iommu_domain_lock, flags);
    kinfo("[IOMMU] user domain released did=%u owner=%d\n",
          devid, owner_pid);
    return 0;
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
