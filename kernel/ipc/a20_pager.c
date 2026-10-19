/*
 * A20OS core — user-space pager (Native ABI).
 *
 * A PAGED VMO (VMO_PAGED) hands unmaterialized page faults to a user-space
 * pager.  The pager owns a request channel endpoint; when a fault cannot be
 * satisfied locally, the kernel enqueues a page-request message (16-byte
 * payload + the VMO as a message handle) on that channel and parks the
 * faulting thread on the VMO's faulter wait queue until the pager supplies
 * the page (a20_pager_supply_pages) and wakes it.
 *
 * Design reference: docs/native-abi/09-native-abi-deepening.md §2.
 * Ownership:
 * - The pager object holds the request channel endpoint (one reference).
 * - Each attached PAGED VMO holds one reference to the pager
 *   (vmo->pager), so the pager stays alive as long as any VMO references it.
 * - vmo_destroy() calls a20_pager_detach_vmo() to drop that reference.
 */
#include "core/types.h"
#include "core/klog.h"
#include "core/string.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/sync.h"
#include "mm/slab.h"
#include "mm/vmo.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "ipc/ipc.h"
#include "ipc/objstats.h"

extern int64_t a20_pager_supply_pages_rust(a20_pager_t *pager,
                                            struct vmo *vmo,
                                            struct vmo *src,
                                            uint64_t vmo_offset,
                                            uint64_t src_offset,
                                            uint64_t len);

typedef struct a20_page_req {
    uint32_t kind;      /* A20_PAGE_REQ_READ / A20_PAGE_REQ_WRITE */
    uint32_t reserved;
    uint64_t offset;    /* vmo byte offset of the missing page */
} a20_page_req_t;

typedef struct a20_pager_close_watch {
    refcount_t refs;
    spinlock_t lock;
    a20_pager_t *pager; /* weak; protected by lock and inc_not_zero */
} a20_pager_close_watch_t;

static void pager_watch_ref(void *ctx)
{
    a20_pager_close_watch_t *watch = ctx;
    refcount_inc(&watch->refs);
}

static void pager_watch_put(void *ctx)
{
    a20_pager_close_watch_t *watch = ctx;
    if (refcount_dec_and_test(&watch->refs))
        kfree(watch);
}

static void pager_watch_peer_closed(void *ctx)
{
    a20_pager_close_watch_t *watch = ctx;
    a20_pager_t *pager = NULL;
    spin_lock(&watch->lock);
    if (watch->pager && refcount_inc_not_zero(&watch->pager->refcount))
        pager = watch->pager;
    spin_unlock(&watch->lock);
    if (pager) {
        a20_pager_peer_closed_rust(pager);
        a20_pager_put(pager);
    }
}

/* The security label of the faulting task lives in the Native handle table,
 * which is not linked in Linux-only kernels.  Weak defaults keep the request
 * message safe (label 0 = lowest) when the Native ABI is absent. */
__attribute__((weak)) void *task_get_a20_ht(struct task_t *t)
{ (void)t; return NULL; }
__attribute__((weak)) uint8_t a20_ht_get_label(void *ht)
{ (void)ht; return 0; }

/* The request channel endpoint is created by a20_pager_create; the peer is
 * handed to userspace as the pager's request handle. */
a20_pager_t *a20_pager_create(a20_channel_ep_t *requests)
{
    a20_pager_t *p = kmalloc(sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    refcount_set(&p->refcount, 1);
    spin_init(&p->lock);
    p->requests = requests;   /* takes ownership of this endpoint reference */
    a20_pager_close_watch_t *watch = kmalloc(sizeof(*watch));
    if (!watch) {
        kfree(p);
        return NULL;
    }
    refcount_set(&watch->refs, 1); /* pager-owned reference */
    spin_init(&watch->lock);
    watch->pager = p;
    p->close_watch = watch;
    if (a20_channel_ep_set_peer_close_hook(requests, pager_watch_peer_closed,
                                           pager_watch_ref, pager_watch_put,
                                           watch) != A20_OK) {
        p->close_watch = NULL;
        spin_lock(&watch->lock);
        watch->pager = NULL;
        spin_unlock(&watch->lock);
        pager_watch_put(watch);
        kfree(p);
        return NULL;
    }
    return p;
}

int a20_pager_vmo_lock_pager(a20_pager_t *pager, uint64_t *flags)
{
    if (!pager || !flags) return -A20_ERR_INVALID_ARGUMENT;
    *flags = spin_lock_irqsave(&pager->lock);
    return 0;
}
void a20_pager_vmo_unlock_pager(a20_pager_t *pager, uint64_t flags)
{ spin_unlock_irqrestore(&pager->lock, flags); }
int a20_pager_vmo_lock_vmo(struct vmo *vmo, uint64_t *flags)
{
    if (!vmo || !flags) return -A20_ERR_INVALID_ARGUMENT;
    *flags = spin_lock_irqsave(&vmo->lock);
    return 0;
}
void a20_pager_vmo_unlock_vmo(struct vmo *vmo, uint64_t flags)
{ spin_unlock_irqrestore(&vmo->lock, flags); }
uint32_t a20_pager_vmo_type_locked(struct vmo *vmo) { return vmo->type; }
a20_pager_t *a20_pager_vmo_get_locked(struct vmo *vmo) { return vmo->pager; }
void a20_pager_vmo_set_locked(struct vmo *vmo, a20_pager_t *pager)
{ vmo->pager = pager; }
int a20_pager_vmo_dead_locked(a20_pager_t *pager) { return pager->dead != 0; }
void a20_pager_vmo_set_dead_locked(a20_pager_t *pager) { pager->dead = 1; }
uint32_t a20_pager_vmo_count_locked(a20_pager_t *pager) { return pager->vmo_count; }
uint32_t a20_pager_vmo_capacity_locked(a20_pager_t *pager) { return pager->vmo_cap; }
struct vmo *a20_pager_vmo_slot_locked(a20_pager_t *pager, uint32_t index)
{ return pager->vmos[index]; }
void a20_pager_vmo_set_slot_locked(a20_pager_t *pager, uint32_t index,
                                   struct vmo *vmo)
{ pager->vmos[index] = vmo; }
void a20_pager_vmo_set_count_locked(a20_pager_t *pager, uint32_t count)
{ pager->vmo_count = count; }
struct vmo **a20_pager_vmo_publish_list_locked(a20_pager_t *pager,
                                                struct vmo **list,
                                                uint32_t cap)
{
    struct vmo **old = pager->vmos;
    pager->vmos = list;
    pager->vmo_cap = cap;
    return old;
}
struct vmo **a20_pager_vmo_alloc_list(uint32_t cap)
{
    if (!cap) return NULL;
#if SIZE_MAX <= UINT32_MAX
    if (cap > UINT32_MAX / sizeof(struct vmo *)) return NULL;
#endif
    return kmalloc((size_t)cap * sizeof(struct vmo *));
}
void a20_pager_vmo_free_list(struct vmo **list) { if (list) kfree(list); }
void a20_pager_vmo_ref_pager(a20_pager_t *pager) { a20_pager_ref(pager); }
void a20_pager_vmo_put_pager(a20_pager_t *pager) { a20_pager_put(pager); }
int a20_pager_vmo_try_ref_vmo(struct vmo *vmo)
{ return vmo && refcount_inc_not_zero(&vmo->refcount); }
void a20_pager_vmo_put_vmo(struct vmo *vmo) { vmo_release(vmo); }
void a20_pager_vmo_wake(struct vmo *vmo)
{
    unsigned woke = wait_queue_wake_all(&vmo->faulters, 0, PROC_WAKE_EVENT);
#ifdef CONFIG_VMO_PAGER_TEST
    if (woke)
        kinfo("PAGER_CLOSE_WAKE: waiters=%u\n", woke);
#else
    (void)woke;
#endif
}

void a20_pager_supply_meta(struct vmo *vmo, uint32_t *type, uint64_t *size)
{
    *type = vmo->type;
    *size = vmo->size;
}
int a20_pager_supply_lock(struct vmo *vmo, uint64_t *flags)
{
    if (!vmo || !flags) return -A20_ERR_INVALID_ARGUMENT;
    *flags = spin_lock_irqsave(&vmo->lock);
    return 0;
}
void a20_pager_supply_unlock(struct vmo *vmo, uint64_t flags)
{ spin_unlock_irqrestore(&vmo->lock, flags); }
a20_pager_t *a20_pager_supply_pager_locked(struct vmo *vmo)
{ return vmo->pager; }
uint64_t a20_pager_supply_dst_page(struct vmo *vmo, uint32_t index)
{ return vmo->pages[index]; }
uint64_t a20_pager_supply_src_page(struct vmo *vmo, uint32_t index)
{ return vmo_peek_page(vmo, index); }
uint64_t a20_pager_supply_alloc_page(void)
{ return pfa_alloc_page(); }
void a20_pager_supply_copy_install(struct vmo *vmo, uint32_t index,
                                   uint32_t src, uint32_t dst)
{
    memcpy(pfn_to_virt(dst), pfn_to_virt(src), PAGE_SIZE);
    vmo->pages[index] = dst;
    vmo->phys_size += PAGE_SIZE;
    a20_objstat_add(&g_a20_objstats.vmo_pages, 1);
}
void a20_pager_supply_wake(struct vmo *vmo, uint64_t first_page, int keyed)
{
    wait_queue_wake_all(&vmo->faulters, keyed ? first_page : 0, PROC_WAKE_EVENT);
}

static void a20_pager_free(a20_pager_t *p)
{
    if (!p) return;
    a20_pager_close_watch_t *watch = p->close_watch;
    if (p->requests)
        a20_channel_ep_clear_peer_close_hook(p->requests);
    if (watch) {
        spin_lock(&watch->lock);
        watch->pager = NULL;
        spin_unlock(&watch->lock);
        pager_watch_put(watch);
    }
    /* Rust pager associations keep this weak registry compact while live;
     * the allocation outlives the last attached VMO until this final put. */
    if (p->vmos)
        kfree(p->vmos);
    if (p->requests)
        a20_channel_ep_release(p->requests);
    kfree(p);
}

void a20_pager_ref(a20_pager_t *p)
{
    if (p) refcount_inc(&p->refcount);
}

void a20_pager_put(a20_pager_t *p)
{
    if (p && refcount_dec_and_test(&p->refcount))
        a20_pager_free(p);
}

/* Attach @vmo (must be VMO_PAGED) to @pager.  Takes a pager reference that is
 * dropped by a20_pager_detach_vmo() on VMO destruction. */
int a20_pager_attach_vmo(a20_pager_t *pager, struct vmo *vmo)
{
    return a20_pager_attach_vmo_rust(pager, vmo);
}

/* Detach @vmo from its pager; wakes all faulters (they fall back to the
 * zero-fill path via the no-pager branch). */
void a20_pager_detach_vmo(struct vmo *vmo)
{
    a20_pager_detach_vmo_rust(vmo);
}

/*
 * Enqueue a page-request message for @vmo page @index.  The message carries
 * the VMO as a handle so the pager can identify and supply the right object.
 * Returns 0 if queued, -A20_ERR_CANCELED if the request peer closed, or a
 * negative status on failure.
 */
int a20_pager_request_page(struct vmo *vmo, uint32_t index, int write_access)
{
    if (!vmo)
        return -A20_ERR_INVALID_ARGUMENT;

    /* The VMO association owns a pager reference. Pin it while holding the
     * VMO lock so detach/final-handle close cannot free the endpoint below. */
    spin_lock(&vmo->lock);
    if (index >= vmo->page_count) {
        spin_unlock(&vmo->lock);
        return -A20_ERR_INVALID_ARGUMENT;
    }
    a20_pager_t *pager = vmo->pager;
    if (!pager) {
        spin_unlock(&vmo->lock);
        return -EAGAIN;
    }
    a20_pager_ref(pager);
    spin_unlock(&vmo->lock);
    if (a20_channel_peer_closed(pager->requests)) {
        a20_pager_peer_closed_rust(pager);
        a20_pager_put(pager);
        return -A20_ERR_CANCELED;
    }

    a20_page_req_t req;
    req.kind = write_access ? A20_PAGE_REQ_WRITE : A20_PAGE_REQ_READ;
    req.reserved = 0;
    req.offset = (uint64_t)index * PAGE_SIZE;

    /* The message handle identifies the VMO to the pager (shared transfer
     * semantics: the faulting task's own handle is unaffected). */
    a20_ch_handle_info_t hinfo;
    memset(&hinfo, 0, sizeof(hinfo));
    hinfo.object = vmo;
    hinfo.type = A20_OBJ_MEMORY;
    hinfo.transfer_rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_MAP |
                            A20_RIGHT_STAT | A20_RIGHT_CONTROL | A20_RIGHT_DUP |
                            A20_RIGHT_TRANSFER;
    task_t *t = proc_current();
    hinfo.security_label = t ? a20_ht_get_label(task_get_a20_ht(t)) : 0;

    int rc = a20_channel_send_dwc(pager->requests, &req, sizeof(req), &hinfo, 1,
                                  NULL, A20_MSG_NONBLOCK, 0);
    if (rc == -A20_ERR_CANCELED)
        a20_pager_peer_closed_rust(pager);
    a20_pager_put(pager);
    return rc;
}

/*
 * Fill [vmo_offset, +len) of the PAGED @vmo from @src's [src_offset, +len),
 * then wake faulters for each supplied page.  Both offsets and @len must be
 * page aligned; @len pages must not exceed the VMO's logical size.
 */
int64_t a20_pager_supply_pages(a20_pager_t *pager, struct vmo *vmo,
                               struct vmo *src, uint64_t vmo_offset,
                               uint64_t src_offset, uint64_t len)
{
    return a20_pager_supply_pages_rust(pager, vmo, src, vmo_offset,
                                       src_offset, len);
}
