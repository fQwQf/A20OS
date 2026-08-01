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
    return vmo;
}

static void vmo_destroy(struct vmo *vmo)
{
    if (!vmo) return;
    if (vmo->pages) {
        for (uint32_t i = 0; i < vmo->page_count; i++) {
            if (vmo->pages[i] != PFN_NONE)
                pfa_free_page(vmo->pages[i]);
        }
        kfree(vmo->pages);
    }
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
    memset(va, 0, PAGE_SIZE);
    vmo->pages[index] = pfn;
    vmo->phys_size += PAGE_SIZE;
    spin_unlock(&vmo->lock);
    return pfn;
}

int64_t vmo_resize(struct vmo *vmo, uint64_t new_size)
{
    if (!vmo) return -1;

    uint32_t new_np = (uint32_t)((new_size + PAGE_SIZE - 1) / PAGE_SIZE);

    spin_lock(&vmo->lock);

    if (new_np <= vmo->page_count) {
        for (uint32_t i = new_np; i < vmo->page_count; i++) {
            if (vmo->pages[i] != PFN_NONE) {
                pfa_free_page(vmo->pages[i]);
                vmo->pages[i] = PFN_NONE;
            }
        }
        vmo->size = new_size;
        vmo->page_count = new_np;
        spin_unlock(&vmo->lock);
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
