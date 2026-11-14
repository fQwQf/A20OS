/*
 * A20OS user-space driver (udriver) support — kernel side.
 * See kernel/include/drivers/core/udriver.h for the design.
 */
#include "core/types.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "mm/slab.h"
#include "mm/vmo.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/riscv_iommu.h"
#include "drivers/core/udriver.h"
#include "drivers/core/driver_core.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/driver_descriptor.h"
#include "ipc/ipc.h"
#include "ipc/objstats.h"

#define UDRIVER_EDU_VENDOR 0x1234u
#define UDRIVER_EDU_DEVICE 0x11e8u

/* Every matching PCI function the user-driver path can claim.  Each has its
 * own BAR window and (on a RISC-V IOMMU board) its own translation domain, so
 * one process may own several of them at once. */
#define UDRIVER_EDU_MAX 4

static pci_user_device_info_t g_edu[UDRIVER_EDU_MAX];
static int g_edu_count = -1;    /* -1: not scanned yet */

static int udriver_edu_prepare(void)
{
    if (g_edu_count >= 0)
        return g_edu_count > 0;
    g_edu_count = 0;
    for (unsigned i = 0; i < UDRIVER_EDU_MAX; i++) {
        if (pci_user_device_find_index(UDRIVER_EDU_VENDOR,
                                       UDRIVER_EDU_DEVICE, i,
                                       &g_edu[g_edu_count]) < 0)
            break;
        /* Enumeration assigns the BAR and enables decoding.  Keep DMA
         * disabled until a process owns an attached IOMMU domain. */
        if (pci_user_device_bus_master(g_edu[g_edu_count].devid, 0) < 0)
            break;
        g_edu_count++;
    }
    return g_edu_count > 0;
}

static int udriver_device_index(uint64_t phys)
{
    for (int i = 0; i < g_edu_count; i++)
        if (phys >= g_edu[i].bar0_phys &&
            phys < g_edu[i].bar0_phys + g_edu[i].bar0_size)
            return i;
    return -1;
}

static int udriver_mmio_allowed(uint64_t phys, uint64_t size)
{
    if (size == 0 || phys + size < phys)
        return 0;
    if (current_board && current_board->user_mmio_allowed &&
        current_board->user_mmio_allowed(phys, size))
        return 1;
    for (int i = 0; i < g_edu_count; i++)
        if (phys >= g_edu[i].bar0_phys &&
            phys + size <= g_edu[i].bar0_phys + g_edu[i].bar0_size)
            return 1;
    return 0;
}

int udriver_mmio_user_owned(uint64_t phys)
{
    if (current_board && current_board->user_mmio_owned &&
        current_board->user_mmio_owned(phys))
        return 1;
    return udriver_device_index(phys) >= 0;
}

int udriver_window_present(uint64_t phys)
{
    return current_board && current_board->user_mmio_present &&
        current_board->user_mmio_present(phys);
}

int udriver_match_present(uint32_t bus, uint32_t vendor, uint32_t device)
{
    if (bus == A20_DRIVER_BUS_MMIO)
        return udriver_window_present(vendor);
    if (bus == A20_DRIVER_BUS_PCI && vendor == UDRIVER_EDU_VENDOR &&
        device == UDRIVER_EDU_DEVICE)
        return udriver_edu_prepare();
    return 0;
}

int udriver_map_mmio(mm_struct_t *mm, uint64_t phys, uint64_t size,
                     uint32_t prot, uint64_t *out_va)
{
    if (!mm || !mm->pgdir || !out_va || !size ||
        size > SIZE_MAX - (PAGE_SIZE - 1) || !prot || (prot & ~3U))
        return -1;
    size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if ((phys & (PAGE_SIZE - 1)) || !udriver_mmio_allowed(phys, size))
        return -1;
    vaddr_t mapped = mm_mmap_physical(mm, 0, (size_t)size, prot, MAP_SHARED,
                                      phys, PTE_DEVICE);
    if (mm_addr_is_error(mapped)) return -1;
    *out_va = mapped;
    return 0;
}

void udriver_revoke_mmio(mm_struct_t *mm, uint64_t phys)
{
#ifndef CONFIG_NOMMU
    if (!mm || !mm->pgdir)
        return;
    for (;;) {
        vaddr_t start = 0;
        size_t length = 0;
        uint64_t flags = spin_lock_irqsave(&mm->lock);
        for (mm_seg_t *vma = mm->mmap; vma; vma = vma->next) {
            if (!(vma->vm_flags & VM_PFNMAP))
                continue;
            int level = 0;
            vaddr_t base = 0;
            size_t leaf_size = 0;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, vma->start, &level,
                                        &base, &leaf_size);
            if (pte && (*pte & PTE_V) && arch_pte_addr(*pte) == phys) {
                start = vma->start;
                length = vma->end - vma->start;
                break;
            }
        }
        spin_unlock_irqrestore(&mm->lock, flags);
        if (!start)
            break;
        if (mm_munmap(mm, start, length) < 0)
            break;
    }
#else
    (void)mm;
    (void)phys;
#endif
}

/* ---- Device ownership claims (docs/hybrid-kernel/04-dual-placement.md) ----
 *
 * A user task may exclusively claim a whitelisted user-owned window.
 * Claims are per-window, idempotent for the owner, and auto-released
 * when the owner dies (udriver_task_cleanup).  Kernel-side read-only
 * probes (e.g. vinput-probe) do not claim; destructive init
 * belongs to the claimant.  The PCI pilot additionally couples ownership to
 * bus mastering, an IOMMU domain and every DMA mapping.
 */
#define UDRIVER_CLAIM_MAX 8
#define UDRIVER_DMA_MAX 8

static spinlock_t g_udr_lock = SPINLOCK_INIT;
/* Board-specific user windows (not PCI devices) keep using this table. */
static int g_window_claim_pid[UDRIVER_CLAIM_MAX]; /* 0 = unclaimed */
/* PCI device claims are indexed by the device's position in g_edu[]. */
static int g_device_claim_pid[UDRIVER_EDU_MAX];   /* 0 = unclaimed */
/* Device a process most recently claimed.  DMA allocations are bound to it, so
 * a process owning two functions can direct each allocation at one of them by
 * claiming that device again (claim is idempotent). */
static int g_active_device = -1;
static int g_active_device_pid;

typedef struct {
    struct vmo *vmo;
    uint64_t iova;
    uint32_t npages;
    uint16_t devid;
    int owner_pid;
    int active;
} udriver_dma_entry_t;

static udriver_dma_entry_t g_udr_dma[UDRIVER_DMA_MAX];

static int udriver_window_index(uint64_t phys)
{
    if (current_board && current_board->user_mmio_index) {
        int index = current_board->user_mmio_index(phys);
        if (index >= 0 && index < UDRIVER_CLAIM_MAX)
            return index;
    }
    return -1;
}

static void udriver_dma_drop_all(int pid)
{
    struct vmo *dead[UDRIVER_DMA_MAX];
    unsigned n = 0;
    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    for (unsigned i = 0; i < UDRIVER_DMA_MAX; i++) {
        if (g_udr_dma[i].active && g_udr_dma[i].owner_pid == pid) {
            dead[n++] = g_udr_dma[i].vmo;
            memset(&g_udr_dma[i], 0, sizeof(g_udr_dma[i]));
        }
    }
    spin_unlock_irqrestore(&g_udr_lock, flags);
    for (unsigned i = 0; i < n; i++)
        vmo_release(dead[i]);
}

/* The device index owned by @pid, or -1.  A user process may own more than one
 * function; its DMA mappings follow the device that was claimed for them. */
static int udriver_claimed_device(int pid)
{
    if (g_active_device_pid == pid && g_active_device >= 0 &&
        g_active_device < g_edu_count &&
        g_device_claim_pid[g_active_device] == pid)
        return g_active_device;
    for (int i = 0; i < g_edu_count; i++)
        if (g_device_claim_pid[i] == pid)
            return i;
    return -1;
}

static int udriver_claim_device(int di, int pid)
{
    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    if (g_device_claim_pid[di] == pid) {
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return 0;
    }
    if (g_device_claim_pid[di] != 0) {
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return -1;
    }
    g_device_claim_pid[di] = -pid; /* reserve while hardware is fail-closed */
    uint16_t devid = g_edu[di].devid;
    spin_unlock_irqrestore(&g_udr_lock, flags);

    driver_discovery_begin();
    int r = -1;
    if (riscv_iommu_domain_claim(devid, pid) == 0) {
        if (pci_user_device_bus_master(devid, 1) == 0)
            r = 0;
        else
            riscv_iommu_domain_release(devid, pid);
    }
    flags = spin_lock_irqsave(&g_udr_lock);
    g_device_claim_pid[di] = r == 0 ? pid : 0;
    if (r == 0) {
        g_active_device = di;          /* later DMA allocations target this one */
        g_active_device_pid = pid;
    }
    spin_unlock_irqrestore(&g_udr_lock, flags);
    driver_discovery_end();
    return r;
}

int udriver_claim(uint64_t phys, int pid)
{
    int di = udriver_device_index(phys);
    if (di >= 0)
        return udriver_claim_device(di, pid);
    int idx = udriver_window_index(phys);
    if (idx < 0)
        return -2; /* not a user-claimable window */
    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    if (g_window_claim_pid[idx] == pid) {
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return 0;
    }
    if (g_window_claim_pid[idx] != 0) {
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return -1;
    }
    g_window_claim_pid[idx] = pid;
    spin_unlock_irqrestore(&g_udr_lock, flags);
    return 0;
}

int udriver_release(uint64_t phys, int pid)
{
    int di = udriver_device_index(phys);
    if (di >= 0) {
        uint64_t flags = spin_lock_irqsave(&g_udr_lock);
        if (g_device_claim_pid[di] != pid) {
            spin_unlock_irqrestore(&g_udr_lock, flags);
            return -1;
        }
        uint16_t devid = g_edu[di].devid;
        spin_unlock_irqrestore(&g_udr_lock, flags);

        driver_discovery_begin();
        pci_user_device_bus_master(devid, 0);
        if (riscv_iommu_domain_release(devid, pid) < 0) {
            driver_discovery_end();
            return -1;
        }
        driver_discovery_end();
        /* Mappings belonging to this device are dropped with the process's
         * entries; another device owned by the same process keeps its own. */
        udriver_dma_drop_all(pid);
        flags = spin_lock_irqsave(&g_udr_lock);
        g_device_claim_pid[di] = 0;
        if (g_active_device == di && g_active_device_pid == pid) {
            g_active_device = -1;
            g_active_device_pid = 0;
        }
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return 0;
    }
    int idx = udriver_window_index(phys);
    if (idx < 0)
        return -2;
    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    if (g_window_claim_pid[idx] != pid) {
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return -1;
    }
    g_window_claim_pid[idx] = 0;
    spin_unlock_irqrestore(&g_udr_lock, flags);
    return 0;
}

/* Owner pid of the window containing @phys, 0 if unclaimed, -1 if not
 * a user-claimable window.  For kernel probes and diagnostics. */
int udriver_claim_owner(uint64_t phys)
{
    int di = udriver_device_index(phys);
    if (di >= 0) {
        uint64_t flags = spin_lock_irqsave(&g_udr_lock);
        int owner = g_device_claim_pid[di];
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return owner;
    }
    int idx = udriver_window_index(phys);
    if (idx < 0)
        return -1;
    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    int owner = g_window_claim_pid[idx];
    spin_unlock_irqrestore(&g_udr_lock, flags);
    return owner;
}

int udriver_device_get_info(uint32_t bus, uint32_t vendor, uint32_t device,
                            uint32_t index, int pid,
                            udriver_device_info_t *out)
{
    if (!out || bus != A20_DRIVER_BUS_PCI ||
        vendor != UDRIVER_EDU_VENDOR || device != UDRIVER_EDU_DEVICE ||
        !udriver_edu_prepare() || index >= (uint32_t)g_edu_count)
        return -1;
    pci_user_device_info_t *dev = &g_edu[index];
    memset(out, 0, sizeof(*out));
    out->flags = UDRIVER_DEVICE_F_IOMMU;
    out->devid = dev->devid;
    out->irq = dev->irq;
    out->mmio_base = dev->bar0_phys;
    out->mmio_size = dev->bar0_size;

    if (udriver_claim_owner(dev->bar0_phys) == pid) {
        int blocked = 0;
        if (riscv_iommu_domain_fault(dev->devid, pid, &out->fault_count,
                                     &out->fault_cause, &out->fault_iova,
                                     &blocked) < 0)
            return -1;
        if (blocked) {
            out->flags |= UDRIVER_DEVICE_F_BLOCKED;
            pci_user_device_bus_master(dev->devid, 0);
        }
    }
    return 0;
}

/* Return 1 when the caller has no isolated device (legacy physical-DMA
 * semantics), 0 when an IOVA was installed, and -1 on a fail-closed error. */
int udriver_dma_map(struct vmo *vmo, int pid, uint64_t *out_iova)
{
    if (!vmo || !out_iova)
        return -1;
    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    int di = udriver_claimed_device(pid);
    if (di < 0) {
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return 1;
    }
    uint16_t devid = g_edu[di].devid;
    int slot = -1;
    for (unsigned i = 0; i < UDRIVER_DMA_MAX; i++) {
        if (g_udr_dma[i].active && g_udr_dma[i].vmo == vmo) {
            *out_iova = g_udr_dma[i].iova;
            spin_unlock_irqrestore(&g_udr_lock, flags);
            return 0;
        }
        if (!g_udr_dma[i].active && slot < 0)
            slot = (int)i;
    }
    if (slot < 0 || !vmo->page_count) {
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return -1;
    }
    pfn_t first = vmo_peek_page(vmo, 0);
    if (first == PFN_NONE) {
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return -1;
    }
    for (uint32_t i = 1; i < vmo->page_count; i++) {
        if (vmo_peek_page(vmo, i) != first + i) {
            spin_unlock_irqrestore(&g_udr_lock, flags);
            return -1;
        }
    }
    g_udr_dma[slot].active = -1;
    g_udr_dma[slot].owner_pid = pid;
    spin_unlock_irqrestore(&g_udr_lock, flags);

    uint64_t iova = 0;
    if (riscv_iommu_domain_map(devid, pid, pfn_to_phys(first),
                               vmo->page_count, &iova) < 0) {
        flags = spin_lock_irqsave(&g_udr_lock);
        memset(&g_udr_dma[slot], 0, sizeof(g_udr_dma[slot]));
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return -1;
    }
    vmo_ref(vmo); /* pin until explicit unmap or owner teardown */
    flags = spin_lock_irqsave(&g_udr_lock);
    g_udr_dma[slot].vmo = vmo;
    g_udr_dma[slot].iova = iova;
    g_udr_dma[slot].npages = vmo->page_count;
    g_udr_dma[slot].devid = devid;
    g_udr_dma[slot].active = 1;
    spin_unlock_irqrestore(&g_udr_lock, flags);
    *out_iova = iova;
    return 0;
}

int udriver_dma_address(struct vmo *vmo, int pid, uint32_t page,
                        uint64_t *out_addr)
{
    if (!vmo || !out_addr)
        return -1;
    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    for (unsigned i = 0; i < UDRIVER_DMA_MAX; i++) {
        if (g_udr_dma[i].active == 1 && g_udr_dma[i].owner_pid == pid &&
            g_udr_dma[i].vmo == vmo && page < g_udr_dma[i].npages) {
            *out_addr = g_udr_dma[i].iova + (uint64_t)page * PAGE_SIZE;
            spin_unlock_irqrestore(&g_udr_lock, flags);
            return 0;
        }
    }
    int isolated = udriver_claimed_device(pid) >= 0;
    spin_unlock_irqrestore(&g_udr_lock, flags);
    return isolated ? -1 : 1;
}

int udriver_dma_unmap(struct vmo *vmo, int pid)
{
    if (!vmo)
        return -1;
    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    int slot = -1;
    for (unsigned i = 0; i < UDRIVER_DMA_MAX; i++)
        if (g_udr_dma[i].active == 1 && g_udr_dma[i].owner_pid == pid &&
            g_udr_dma[i].vmo == vmo) {
            slot = (int)i;
            break;
        }
    if (slot < 0) {
        int isolated = udriver_claimed_device(pid) >= 0;
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return isolated ? -1 : 1;
    }
    udriver_dma_entry_t entry = g_udr_dma[slot];
    g_udr_dma[slot].active = -1;
    spin_unlock_irqrestore(&g_udr_lock, flags);
    if (riscv_iommu_domain_unmap(entry.devid, pid, entry.iova,
                                 entry.npages) < 0) {
        flags = spin_lock_irqsave(&g_udr_lock);
        g_udr_dma[slot].active = 1;
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return -1;
    }
    flags = spin_lock_irqsave(&g_udr_lock);
    memset(&g_udr_dma[slot], 0, sizeof(g_udr_dma[slot]));
    spin_unlock_irqrestore(&g_udr_lock, flags);
    vmo_release(entry.vmo);
    return 0;
}

/* ---- IRQ → event queue delivery ---- */

#define UDRIVER_IRQ_MAX 8

typedef struct {
    uint32_t         irq;
    a20_eventq_t    *queue;      /* referenced while registered */
    uint64_t         user_data;
    int              owner_pid;
    uint8_t          active;
} udriver_irq_entry_t;

static udriver_irq_entry_t g_udr_irq[UDRIVER_IRQ_MAX];

static int udriver_irq_thunk(int irq, void *priv)
{
    udriver_irq_entry_t *e = (udriver_irq_entry_t *)priv;
    /* Mask at the irqchip first so a level-triggered line cannot storm
     * while the user handler works; it re-arms via udriver_irq_ack(). */
    irq_disable((uint32_t)irq);
    if (e->active)
        a20_event_notify((void *)(uintptr_t)e->irq, A20_OBJ_DEVICE,
                         A20_EVENT_SIGNALED, e->user_data, 0);
    return 0;
}

int udriver_irq_listen(uint32_t irq, a20_eventq_t *queue,
                       uint64_t user_data, int owner_pid)
{
    if (!queue || irq == 0 || irq >= 1024)
        return -1;

    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    udriver_irq_entry_t *slot = NULL;
    for (unsigned i = 0; i < UDRIVER_IRQ_MAX; i++) {
        if (g_udr_irq[i].active && g_udr_irq[i].irq == irq) {
            spin_unlock_irqrestore(&g_udr_lock, flags);
            return -1; /* already bound */
        }
        if (!g_udr_irq[i].active && !slot)
            slot = &g_udr_irq[i];
    }
    if (!slot) {
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return -1;
    }

    slot->irq = irq;
    slot->queue = queue;
    slot->user_data = user_data;
    slot->owner_pid = owner_pid;
    slot->active = 1;
    a20_objstat_add(&g_a20_objstats.irq_bindings, 1);
    spin_unlock_irqrestore(&g_udr_lock, flags);

    refcount_inc(&queue->refcount);
    if (request_irq(irq, udriver_irq_thunk, 0, slot) < 0) {
        flags = spin_lock_irqsave(&g_udr_lock);
        slot->active = 0;
        spin_unlock_irqrestore(&g_udr_lock, flags);
        a20_eventq_release(queue);
        return -1;
    }
    return 0;
}

static udriver_irq_entry_t *udriver_find_irq(uint32_t irq)
{
    for (unsigned i = 0; i < UDRIVER_IRQ_MAX; i++)
        if (g_udr_irq[i].active && g_udr_irq[i].irq == irq)
            return &g_udr_irq[i];
    return NULL;
}

int udriver_irq_ack(uint32_t irq)
{
    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    udriver_irq_entry_t *e = udriver_find_irq(irq);
    spin_unlock_irqrestore(&g_udr_lock, flags);
    if (!e)
        return -1;
    irq_enable(irq);
    return 0;
}

int udriver_irq_unlisten(uint32_t irq)
{
    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    udriver_irq_entry_t *e = udriver_find_irq(irq);
    a20_eventq_t *queue = NULL;
    if (e) {
        e->active = 0;
        queue = e->queue;
        e->queue = NULL;
        a20_objstat_add(&g_a20_objstats.irq_bindings, -1);
    }
    spin_unlock_irqrestore(&g_udr_lock, flags);
    if (!e)
        return -1;
    free_irq(irq, e);
    if (queue)
        a20_eventq_release(queue);
    return 0;
}

void udriver_task_cleanup(int pid)
{
    a20_eventq_t *dead[UDRIVER_IRQ_MAX];
    uint32_t irqs[UDRIVER_IRQ_MAX];
    void *privs[UDRIVER_IRQ_MAX];
    unsigned n = 0;

    /* Stop DMA and tear down translations before dropping the VMO pins.  A
     * process may own more than one device; each claim is released through its
     * own domain. */
    for (int di = 0; di < g_edu_count; di++) {
        if (udriver_claim_owner(g_edu[di].bar0_phys) == pid)
            (void)udriver_release(g_edu[di].bar0_phys, pid);
    }

    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    for (unsigned i = 0; i < UDRIVER_CLAIM_MAX; i++)
        if (g_window_claim_pid[i] == pid)
            g_window_claim_pid[i] = 0;
    for (unsigned i = 0; i < UDRIVER_IRQ_MAX; i++) {
        if (g_udr_irq[i].active && g_udr_irq[i].owner_pid == pid) {
            g_udr_irq[i].active = 0;
            dead[n] = g_udr_irq[i].queue;
            irqs[n] = g_udr_irq[i].irq;
            privs[n] = &g_udr_irq[i];
            g_udr_irq[i].queue = NULL;
            a20_objstat_add(&g_a20_objstats.irq_bindings, -1);
            n++;
        }
    }
    spin_unlock_irqrestore(&g_udr_lock, flags);

    for (unsigned i = 0; i < n; i++) {
        free_irq(irqs[i], privs[i]);
        if (dead[i])
            a20_eventq_release(dead[i]);
    }
}
