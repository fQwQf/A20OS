/*
 * A20OS core — Virtual Memory Object (VMO).
 *
 * The VMO is the core "page source" used by the Native ABI for anonymous
 * memory, file-backed mappings and shared memory.  It lives in the core MM
 * layer so that both ABIs map through the same VMA/fault machinery and so
 * that the core fault handler does not depend on any ABI-specific header.
 */
#include "core/types.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/lock.h"
#include "mm/slab.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/vmo.h"
#include "cg/cgroup.h"
#include "ipc/objstats.h"
#include "ipc/ipc.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "proc/park.h"

struct vmo *vmo_create(uint32_t type, uint64_t size, uint32_t options)
{
    struct vmo *vmo = kmalloc(sizeof(*vmo));
    if (!vmo) return NULL;

    uint32_t npages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    vmo->pages = NULL;
    if (npages > 0) {
        vmo->pages = kmalloc(npages * sizeof(pfn_t));
        if (!vmo->pages) {
            kfree(vmo);
            return NULL;
        }
        for (uint32_t i = 0; i < npages; i++)
            vmo->pages[i] = PFN_NONE;
    }

    refcount_set(&vmo->refcount, 1);
    vmo->size = size;
    vmo->phys_size = 0;
    vmo->type = type;
    vmo->options = options;
    spin_init(&vmo->lock);
    wait_queue_init(&vmo->faulters);
    vmo->page_count = npages;
    /* Must be zeroed: vmo_destroy uncharges charge_cg whenever it is
     * non-NULL, and tasks without a cgroup never overwrite the field. */
    vmo->charge_cg = NULL;
    vmo->charged_pages = 0;
    vmo->pager = NULL;
    a20_objstat_add(&g_a20_objstats.vmos, 1);
    return vmo;
}

static void vmo_destroy(struct vmo *vmo)
{
    if (!vmo) return;
    if (vmo->pager)
        a20_pager_detach_vmo(vmo);
    a20_objstat_add(&g_a20_objstats.vmos, -1);
    a20_objstat_add(&g_a20_objstats.vmo_pages,
                    -(int64_t)(vmo->phys_size / PAGE_SIZE));
    if (vmo->pages) {
        int any = 0;
        for (uint32_t i = 0; i < vmo->page_count; i++)
            if (vmo->pages[i] != PFN_NONE) { any = 1; break; }
        if (any)
            arch_tlb_flush();  /* BEFORE frames return to the buddy */
        for (uint32_t i = 0; i < vmo->page_count; i++) {
            if (vmo->pages[i] != PFN_NONE)
                pfa_free_page(vmo->pages[i]);
        }
        kfree(vmo->pages);
    }
    /* Release the cgroup charge for materialized pages. */
    if (vmo->charge_cg && vmo->charged_pages)
        cg_mem_uncharge(vmo->charge_cg, vmo->charged_pages);
    kfree(vmo);
}

void vmo_release(struct vmo *vmo)
{
    if (vmo && refcount_dec_and_test(&vmo->refcount))
        vmo_destroy(vmo);
}

void vmo_ref(struct vmo *vmo)
{
    if (vmo) refcount_inc(&vmo->refcount);
}

/*
 * Return the canonical frame for page @index, materializing it on first
 * touch.  The caller must hold the owning task's mm->lock; the frame is
 * owned by the VMO and is never released by unmapping a PTE.
 */
pfn_t vmo_get_page(struct vmo *vmo, uint32_t index)
{
    if (!vmo || index >= vmo->page_count) return PFN_NONE;

    spin_lock(&vmo->lock);
    if (vmo->pages[index] != PFN_NONE) {
        pfn_t pfn = vmo->pages[index];
        spin_unlock(&vmo->lock);
        return pfn;
    }
    int paged = (vmo->type == VMO_PAGED && vmo->pager);
    spin_unlock(&vmo->lock);

    if (paged) {
        if (vmo_paged_fault(vmo, index, 1) != 0)
            return PFN_NONE;
        spin_lock(&vmo->lock);
        pfn_t pfn = vmo->pages[index];
        spin_unlock(&vmo->lock);
        return pfn;
    }

    spin_lock(&vmo->lock);
    pfn_t pfn = pfa_alloc_page();
    if (pfn == PFN_NONE) {
        spin_unlock(&vmo->lock);
        return PFN_NONE;
    }

    void *va = pfn_to_virt(pfn);
    uint32_t *probe = (uint32_t *)va;
    if (*probe != 0 || probe[1] != 0) {
        /* Benign since the buddy does not zero on free; counted so the
         * signal survives without interleaving into user serial output. */
        a20_objstat_add(&g_a20_objstats.vmo_dirty_frames, 1);
    }
    memset(va, 0, PAGE_SIZE);
    vmo->pages[index] = pfn;
    vmo->phys_size += PAGE_SIZE;
    a20_objstat_add(&g_a20_objstats.vmo_pages, 1);
    spin_unlock(&vmo->lock);
    return pfn;
}

/*
 * Demand-fault page source with cgroup accounting: materializes page @index * and charges the new frame to @cg.  Returns 0 with *out set, or -ENOMEM if
 * the charge or frame allocation fails.  The charge is recorded per-VMO and
 * released on destroy.
 */
pfn_t vmo_peek_page(struct vmo *vmo, uint32_t index)
{
    if (!vmo || index >= vmo->page_count)
        return PFN_NONE;
    spin_lock(&vmo->lock);
    pfn_t pfn = vmo->pages[index];
    spin_unlock(&vmo->lock);
    return pfn;
}

/*
 * PAGED-VMO fault: request the page from the user-space pager and park until
 * a20_pager_supply_pages() materializes it.  Caller holds no vmo->lock.
 * Returns:
 *   0         page is now materialized (caller re-reads ->pages[index])
 *   -EAGAIN   no pager / pager gone / request could not be queued: caller
 *             should run the normal zero-fill path
 *   -EINTR    killed while parked (fault path delivers a fatal signal)
 *   -EIO      pager request peer closed: page can never be supplied
 */
int vmo_paged_fault(struct vmo *vmo, uint32_t index, int write_access)
{
    task_t *t = proc_current();

    for (;;) {
        spin_lock(&vmo->lock);
        if (vmo->pages[index] != PFN_NONE) {
            spin_unlock(&vmo->lock);
            return 0;
        }
        a20_pager_t *pager = vmo->pager;
        spin_unlock(&vmo->lock);

        if (!pager)
            return -EAGAIN;

        if (a20_channel_peer_closed(pager->requests)) {
            /* Pager disconnected: faulters can never be satisfied. */
            wait_queue_wake_all(&vmo->faulters, index, PROC_WAKE_EVENT);
            return -EIO;
        }

        int rq = a20_pager_request_page(vmo, index, write_access);
        if (rq != A20_OK)
            return -EIO;

        if (!t)
            return -EINTR;

        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_KILLABLE, 0);
        if (!token.task)
            return -EINTR;

        wait_queue_entry_t entry = {0};
        spin_lock(&vmo->lock);
        /* Re-check before linking so a supply that lands between the request
         * and the link cannot be missed. */
        if (vmo->pages[index] != PFN_NONE) {
            spin_unlock(&vmo->lock);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return 0;
        }
        bool linked = wait_queue_link(&vmo->faulters, &entry, token, index);
        spin_unlock(&vmo->lock);

        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&vmo->faulters, &entry);
        proc_park_finish(token);

        if (reason == PROC_WAKE_FATAL_SIGNAL ||
            reason == PROC_WAKE_TASK_EXIT)
            return -EINTR;
        if (proc_wake_reason_is_task_interrupt(reason) &&
            signal_task_has_fatal(t))
            return -EINTR;
        /* Loop: page may now be materialized, or the request needs retry. */
    }
}

int vmo_get_page_charged(struct vmo *vmo, uint32_t index,
                         struct cg_node *cg, pfn_t *out)
{
    if (!vmo || !out || index >= vmo->page_count)
        return -EINVAL;

    spin_lock(&vmo->lock);
    if (vmo->pages[index] != PFN_NONE) {
        *out = vmo->pages[index];
        spin_unlock(&vmo->lock);
        return 0;
    }
    int paged = (vmo->type == VMO_PAGED && vmo->pager);
    spin_unlock(&vmo->lock);

    if (paged) {
        /* PAGED: request the page from the user-space pager.  On -EAGAIN the
         * pager went away and we fall back to the zero-fill path below. */
        int pf = vmo_paged_fault(vmo, index, 1);
        if (pf == 0) {
            spin_lock(&vmo->lock);
            pfn_t pfn = vmo->pages[index];
            spin_unlock(&vmo->lock);
            if (pfn != PFN_NONE) {
                *out = pfn;
                return 0;
            }
        } else if (pf != -EAGAIN) {
            /* pager closed or killed: fail the fault */
            return -EIO;
        }
    }

    spin_lock(&vmo->lock);
    if (cg && cg_mem_charge(cg, 1) != 0) {
        spin_unlock(&vmo->lock);
        return -ENOMEM;
    }

    pfn_t pfn = pfa_alloc_page();
    if (pfn == PFN_NONE) {
        if (cg)
            cg_mem_uncharge(cg, 1);
        spin_unlock(&vmo->lock);
        return -ENOMEM;
    }

    void *va = pfn_to_virt(pfn);
    uint32_t *probe = (uint32_t *)va;
    if (*probe != 0 || probe[1] != 0) {
        /* See vmo_get_page: legal reuse, counted not printed. */
        a20_objstat_add(&g_a20_objstats.vmo_dirty_frames, 1);
    }
    memset(va, 0, PAGE_SIZE);
    vmo->pages[index] = pfn;
    vmo->phys_size += PAGE_SIZE;
    a20_objstat_add(&g_a20_objstats.vmo_pages, 1);
    if (cg) {
        vmo->charge_cg = cg;
        vmo->charged_pages++;
    }
    spin_unlock(&vmo->lock);
    *out = pfn;
    return 0;
}

int64_t vmo_resize(struct vmo *vmo, uint64_t new_size)
{
    if (!vmo) return -1;

    uint32_t new_np = (uint32_t)((new_size + PAGE_SIZE - 1) / PAGE_SIZE);

    spin_lock(&vmo->lock);

    if (new_np <= vmo->page_count) {
        int freed = 0;
        for (uint32_t i = new_np; i < vmo->page_count; i++) {
            if (vmo->pages[i] != PFN_NONE) {
                pfa_free_page(vmo->pages[i]);
                vmo->pages[i] = PFN_NONE;
                a20_objstat_add(&g_a20_objstats.vmo_pages, -1);
                if (vmo->charge_cg && vmo->charged_pages)
                    vmo->charged_pages--;
                freed = 1;
            }
        }
        if (vmo->charge_cg && vmo->charged_pages == 0)
            vmo->charge_cg = NULL;
        vmo->size = new_size;
        vmo->page_count = new_np;
        spin_unlock(&vmo->lock);
        if (freed)
            arch_tlb_flush();
        return 0;
    }

    pfn_t *new_pages = kmalloc(new_np * sizeof(pfn_t));
    if (!new_pages) {
        spin_unlock(&vmo->lock);
        return -1;
    }
    for (uint32_t i = 0; i < vmo->page_count; i++)
        new_pages[i] = vmo->pages[i];
    for (uint32_t i = vmo->page_count; i < new_np; i++)
        new_pages[i] = PFN_NONE;

    pfn_t *old = vmo->pages;
    vmo->pages = new_pages;
    vmo->page_count = new_np;
    vmo->size = new_size;
    spin_unlock(&vmo->lock);

    kfree(old);
    return 0;
}
