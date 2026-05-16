/*
 * A20OS Native ABI — VMO implementation.
 * Design reference: docs/native-abi/04-memory.md §2
 */
#include "core/types.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/lock.h"
#include "mm/slab.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "abi/native/vmo.h"
#include "abi/native/errno.h"

struct a20_vmo *a20_vmo_create(uint32_t type, uint64_t size, uint32_t options)
{
    struct a20_vmo *vmo = kmalloc(sizeof(*vmo));
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

void a20_vmo_destroy(struct a20_vmo *vmo)
{
    if (!vmo) return;
    if (vmo->pages) {
        for (uint32_t i = 0; i < vmo->page_count; i++) {
            if (vmo->pages[i] != PFN_NONE) {
                pfa_free_page(vmo->pages[i]);
            }
        }
        kfree(vmo->pages);
    }
    kfree(vmo);
}

void a20_vmo_release(struct a20_vmo *vmo)
{
    if (vmo && refcount_dec_and_test(&vmo->refcount))
        a20_vmo_destroy(vmo);
}

pfn_t a20_vmo_get_page(struct a20_vmo *vmo, uint32_t index)
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

int64_t a20_vmo_resize(struct a20_vmo *vmo, uint64_t new_size)
{
    if (!vmo) return -A20_ERR_INVALID_ARGUMENT;

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
        return A20_OK;
    }

    pfn_t *new_pages = kmalloc(new_np * sizeof(pfn_t));
    if (!new_pages) {
        spin_unlock(&vmo->lock);
        return -A20_ERR_NO_MEMORY;
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
    return A20_OK;
}
