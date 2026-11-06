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
#include "core/panic.h"
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

extern pfn_t a20_vmo_get_page_rust(struct vmo *vmo, uint32_t index);
extern int a20_vmo_get_page_charged_rust(struct vmo *vmo, uint32_t index,
                                         struct cg_node *cg, pfn_t *out);
extern int64_t a20_vmo_resize_rust(struct vmo *vmo, uint64_t new_size);
extern void a20_vmo_destroy_rust(struct vmo *vmo);
extern void a20_vmo_ref_rust(struct vmo *vmo);
extern void a20_vmo_release_rust(struct vmo *vmo);
extern pfn_t a20_vmo_peek_page_rust(struct vmo *vmo, uint32_t index);
extern pfn_t a20_vmo_sparse_peek(void *root, uint32_t index);
extern int a20_vmo_sparse_reserve(void **root, uint32_t index);
extern int a20_vmo_sparse_publish(void *root, uint32_t index, pfn_t pfn);
extern void a20_vmo_sparse_destroy(void *root, struct vmo *vmo);

struct vmo_charge {
    struct vmo_charge *next;
    struct cg_node *cg;
    uint64_t pages;
};

void *a20_vmo_sparse_alloc_node(size_t bytes) { return kcalloc_atomic(1, bytes); }
void a20_vmo_sparse_free_node(void *node) { kfree(node); }

struct vmo *vmo_create(uint32_t type, uint64_t size, uint32_t options)
{
    /* page_count is u32; reject rounding and allocation-size overflow before
     * a truncated page array can be indexed by a later demand fault. */
    if (size > (uint64_t)-1 - (PAGE_SIZE - 1))
        return NULL;
    uint64_t count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (count > (uint32_t)-1 || count > SIZE_MAX / sizeof(pfn_t))
        return NULL;
    /* Shared mmap enters with mm->lock held. Sparse creation allocates only
     * this object, using the non-reclaiming allocator under that lock. */
    struct vmo *vmo = type == VMO_SHARED_ANON
                          ? kmalloc_atomic(sizeof(*vmo)) : kmalloc(sizeof(*vmo));
    if (!vmo) return NULL;

    uint32_t npages = (uint32_t)count;
    vmo->pages = NULL;
    vmo->sparse_root = NULL;
    vmo->shared_charges = NULL;
    if (npages > 0 && type != VMO_SHARED_ANON) {
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


void vmo_release(struct vmo *vmo)
{
    a20_vmo_release_rust(vmo);
}

void vmo_ref(struct vmo *vmo)
{
    a20_vmo_ref_rust(vmo);
}

/*
 * Return the canonical frame for page @index, materializing it on first
 * touch.  The caller must hold the owning task's mm->lock; the frame is
 * owned by the VMO and is never released by unmapping a PTE.
 */
pfn_t vmo_get_page(struct vmo *vmo, uint32_t index)
{
    return a20_vmo_get_page_rust(vmo, index);
}

/*
 * Demand-fault page source with cgroup accounting: materializes page @index * and charges the new frame to @cg.  Returns 0 with *out set, or -ENOMEM if
 * the charge or frame allocation fails.  The charge is recorded per-VMO and
 * released on destroy.
 */
pfn_t vmo_peek_page(struct vmo *vmo, uint32_t index)
{
    return a20_vmo_peek_page_rust(vmo, index);
}

/* Small C atomic/lock/slot primitives. Rust owns public ref/release/peek
 * policy and the final-reference dispatch into the page destruction owner. */
void a20_vmo_lifecycle_ref_inc(struct vmo *vmo)
{
    refcount_inc(&vmo->refcount);
}

int a20_vmo_lifecycle_ref_dec_last(struct vmo *vmo)
{
    return refcount_dec_and_test(&vmo->refcount) ? 0 : 1;
}

int a20_vmo_lifecycle_lock(struct vmo *vmo, uint64_t *flags_out)
{
    if (!vmo || !flags_out)
        return -EINVAL;
    *flags_out = spin_lock_irqsave(&vmo->lock);
    return 0;
}

void a20_vmo_lifecycle_unlock(struct vmo *vmo, uint64_t flags)
{
    spin_unlock_irqrestore(&vmo->lock, flags);
}

uint32_t a20_vmo_lifecycle_page_count_locked(struct vmo *vmo)
{
    return vmo->page_count;
}

pfn_t a20_vmo_lifecycle_slot_locked(struct vmo *vmo, uint32_t index)
{
    return vmo->type == VMO_SHARED_ANON
        ? a20_vmo_sparse_peek(vmo->sparse_root, index) : vmo->pages[index];
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
        int has_pager = vmo->pager != NULL;
        spin_unlock(&vmo->lock);

        if (!has_pager)
            return -EAGAIN;

        int rq = a20_pager_request_page(vmo, index, write_access);
        if (rq == -EAGAIN)
            return -EAGAIN;
#ifdef CONFIG_VMO_PAGER_RUST
        if (rq == -A20_ERR_CANCELED)
            return -EAGAIN; /* the close sweep detached this VMO */
#endif
        if (rq == -A20_ERR_CANCELED)
            return -EAGAIN; /* the close sweep detached this VMO */
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
        /* Peer-close detaches the VMO and wakes existing waiters. If it ran
         * after send but before this waiter linked, do not sleep past that
         * one-shot wake; the caller falls back to the unpaged zero-fill path. */
        if (!vmo->pager) {
            spin_unlock(&vmo->lock);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return -EAGAIN;
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
    return a20_vmo_get_page_charged_rust(vmo, index, cg, out);
}

/* Narrow frame/cgroup/VMO-lock primitives for the Rust materialization
 * transaction. The callback contract requires the VMO lock for `*_locked`
 * and `publish`; pager calls happen after unlock because they may park. */
int a20_vmo_page_lock(struct vmo *vmo, uint32_t index, uint64_t *flags_out)
{
    if (!vmo || !flags_out)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&vmo->lock);
    if (index >= vmo->page_count) {
        spin_unlock_irqrestore(&vmo->lock, flags);
        return -EINVAL;
    }
    *flags_out = flags;
    return 0;
}

void a20_vmo_page_unlock(struct vmo *vmo, uint64_t flags)
{
    spin_unlock_irqrestore(&vmo->lock, flags);
}

pfn_t a20_vmo_page_slot_locked(struct vmo *vmo, uint32_t index)
{
    return a20_vmo_lifecycle_slot_locked(vmo, index);
}

/* Caller holds vmo->lock. Failure leaves at most empty, VMO-owned metadata;
 * there is no charge/frame to roll back. A ledger pin outlives every faulter. */
int a20_vmo_page_prepare_locked(struct vmo *vmo, uint32_t index, struct cg_node *cg)
{
    if (vmo->type != VMO_SHARED_ANON)
        return 0;
    int r = a20_vmo_sparse_reserve(&vmo->sparse_root, index);
    if (r < 0 || !cg)
        return r;
    for (struct vmo_charge *c = vmo->shared_charges; c; c = c->next)
        if (c->cg == cg)
            return 0;
    struct vmo_charge *charge = kmalloc_atomic(sizeof(*charge));
    if (!charge)
        return -ENOMEM;
    cg_group_pin(cg);
    *charge = (struct vmo_charge){ .next = vmo->shared_charges, .cg = cg };
    vmo->shared_charges = charge;
    return 0;
}

int a20_vmo_page_is_paged_locked(struct vmo *vmo)
{
    return vmo->type == VMO_PAGED && vmo->pager != NULL;
}

int a20_vmo_page_paged_fault(struct vmo *vmo, uint32_t index, int write_access)
{
    return vmo_paged_fault(vmo, index, write_access);
}

int a20_vmo_page_charge(struct vmo *vmo, struct cg_node *cg)
{
    return vmo->type == VMO_SHARED_ANON ? cg_mem_charge_shared_anon(cg, 1)
                                       : cg_mem_charge(cg, 1);
}

void a20_vmo_page_uncharge(struct vmo *vmo, struct cg_node *cg)
{
    if (vmo->type == VMO_SHARED_ANON)
        cg_mem_uncharge_shared_anon(cg, 1);
    else
        cg_mem_uncharge(cg, 1);
}

pfn_t a20_vmo_page_alloc(void)
{
    return pfa_alloc_page();
}

void a20_vmo_page_free(pfn_t pfn)
{
    pfa_free_page(pfn);
}

void a20_vmo_page_zero(pfn_t pfn)
{
    void *va = pfn_to_virt(pfn);
    uint32_t *probe = (uint32_t *)va;
    if (*probe != 0 || probe[1] != 0)
        a20_objstat_add(&g_a20_objstats.vmo_dirty_frames, 1);
    memset(va, 0, PAGE_SIZE);
}

void a20_vmo_page_publish_locked(struct vmo *vmo, uint32_t index,
                                 pfn_t pfn, struct cg_node *cg)
{
    if (vmo->type == VMO_SHARED_ANON) {
        if (a20_vmo_sparse_publish(vmo->sparse_root, index, pfn) != 0)
            panic("shared anon VMO: unreserved or duplicate page publication");
    } else {
        vmo->pages[index] = pfn;
    }
    vmo->phys_size += PAGE_SIZE;
    a20_objstat_add(&g_a20_objstats.vmo_pages, 1);
    if (cg) {
        if (vmo->type == VMO_SHARED_ANON) {
            struct vmo_charge *charge = vmo->shared_charges;
            while (charge && charge->cg != cg) charge = charge->next;
            if (!charge) panic("shared anon VMO: unreserved charge owner");
            charge->pages++;
        } else {
            vmo->charge_cg = cg;
        }
        vmo->charged_pages++;
    }
}

int64_t vmo_resize(struct vmo *vmo, uint64_t new_size)
{
    if (vmo && vmo->type == VMO_SHARED_ANON)
        return -EPERM;
    return a20_vmo_resize_rust(vmo, new_size);
}

int a20_vmo_resize_lock(struct vmo *vmo, uint64_t *flags_out)
{
    if (!vmo || !flags_out)
        return -EINVAL;
    *flags_out = spin_lock_irqsave(&vmo->lock);
    return 0;
}

void a20_vmo_resize_unlock(struct vmo *vmo, uint64_t flags)
{
    spin_unlock_irqrestore(&vmo->lock, flags);
}

uint32_t a20_vmo_resize_page_count(struct vmo *vmo)
{
    return vmo->page_count;
}

pfn_t a20_vmo_resize_slot(struct vmo *vmo, uint32_t index)
{
    return vmo->pages[index];
}

void *a20_vmo_resize_alloc_table(uint32_t count)
{
    size_t bytes = (size_t)count * sizeof(pfn_t);
    if (!count || bytes / sizeof(pfn_t) != count)
        return NULL;
    return kmalloc(bytes);
}

void a20_vmo_resize_free_table(void *table)
{
    kfree(table);
}

void a20_vmo_resize_table_set(void *table, uint32_t index, pfn_t pfn)
{
    ((pfn_t *)table)[index] = pfn;
}

void *a20_vmo_resize_publish(struct vmo *vmo, void *table,
                             uint32_t count, uint64_t size)
{
    if (!table) {
        vmo->page_count = count;
        vmo->size = size;
        return NULL;
    }
    pfn_t *old = vmo->pages;
    vmo->pages = table;
    vmo->page_count = count;
    vmo->size = size;
    return old;
}

void a20_vmo_resize_flush_tlb(void)
{
    arch_tlb_flush();
}

void a20_vmo_resize_release_page(struct vmo *vmo, uint32_t index)
{
    pfn_t pfn = vmo->pages[index];
    if (pfn == PFN_NONE)
        return;
    if (!pfn_valid(pfn))
        panic("vmo_resize: vmo %p page %u has out-of-range pfn %llu",
              (void *)vmo, index, (unsigned long long)pfn);
    if (vmo->phys_size < PAGE_SIZE)
        panic("vmo_resize: vmo %p phys_size underflow at page %u",
              (void *)vmo, index);
    pfa_free_page(pfn);
    vmo->pages[index] = PFN_NONE;
    vmo->phys_size -= PAGE_SIZE;
    a20_objstat_add(&g_a20_objstats.vmo_pages, -1);
    if (vmo->charge_cg && vmo->charged_pages) {
        cg_mem_uncharge(vmo->charge_cg, 1);
        vmo->charged_pages--;
        if (vmo->charged_pages == 0)
            vmo->charge_cg = NULL;
    }
}

void a20_vmo_resize_finish_shrink(struct vmo *vmo)
{
    if (vmo->charge_cg && vmo->charged_pages == 0)
        vmo->charge_cg = NULL;
}

void a20_vmo_destroy_notify(struct vmo *vmo)
{
    a20_eventq_on_object_destroy(vmo, A20_OBJ_MEMORY);
}

void a20_vmo_destroy_detach_pager(struct vmo *vmo)
{
    if (vmo->pager)
        a20_pager_detach_vmo(vmo);
}

uint32_t a20_vmo_destroy_page_count(struct vmo *vmo)
{
    return vmo->type == VMO_SHARED_ANON ? 0 : vmo->page_count;
}

pfn_t a20_vmo_destroy_page(struct vmo *vmo, uint32_t index)
{
    return vmo->pages ? vmo->pages[index] : PFN_NONE;
}

uint64_t a20_vmo_destroy_phys_size(struct vmo *vmo)
{
    return vmo->phys_size;
}

uint64_t a20_vmo_destroy_charged_pages(struct vmo *vmo)
{
    return vmo->charged_pages;
}

void a20_vmo_destroy_stats(uint64_t phys_size)
{
    a20_objstat_add(&g_a20_objstats.vmos, -1);
    a20_objstat_add(&g_a20_objstats.vmo_pages,
                    -(int64_t)(phys_size / PAGE_SIZE));
}

void a20_vmo_destroy_flush_tlb(void)
{
    arch_tlb_flush();
}

void a20_vmo_destroy_release_page(struct vmo *vmo, uint32_t index, pfn_t pfn)
{
    if (!pfn_valid(pfn))
        panic("vmo_destroy: vmo %p page %u/%u has out-of-range "
              "pfn %llu (pages array corrupted?)",
              (void *)vmo, index, vmo->page_count,
              (unsigned long long)pfn);
    pfa_free_page(pfn);
}

void a20_vmo_destroy_uncharge(struct vmo *vmo, uint64_t pages)
{
    if (vmo->type == VMO_SHARED_ANON) {
        for (struct vmo_charge *c = vmo->shared_charges; c; c = c->next)
            if (c->pages) cg_mem_uncharge_shared_anon(c->cg, c->pages);
        return;
    }
    if (vmo->charge_cg)
        cg_mem_uncharge(vmo->charge_cg, pages);
}

void a20_vmo_destroy_free_pages(struct vmo *vmo)
{
    if (vmo->type == VMO_SHARED_ANON) {
        a20_vmo_sparse_destroy(vmo->sparse_root, vmo);
        vmo->sparse_root = NULL;
    }
    kfree(vmo->pages);
}

void a20_vmo_destroy_free_object(struct vmo *vmo)
{
    struct vmo_charge *c = vmo->shared_charges;
    while (c) {
        struct vmo_charge *next = c->next;
        cg_group_unpin(c->cg);
        kfree(c);
        c = next;
    }
    kfree(vmo);
}
