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
    vmo->page_count = npages;
    /* Must be zeroed: vmo_destroy uncharges charge_cg whenever it is
     * non-NULL, and tasks without a cgroup never overwrite the field. */
    vmo->charge_cg = NULL;
    vmo->charged_pages = 0;
    a20_objstat_add(&g_a20_objstats.vmos, 1);
    return vmo;
}

static void vmo_destroy(struct vmo *vmo)
{
    if (!vmo) return;
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
