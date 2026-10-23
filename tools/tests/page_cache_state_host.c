/*
 * Host differential harness for the page cache state engine
 * (PAGE_CACHE_STATE_IMPL).
 *
 * Drives the C reference model (the pre-migration helper logic from
 * kernel/fs/page_cache.c, reproduced below) and the Rust engine
 * (kernel/fs/page_cache_state.rs, linked as a host staticlib) through one
 * shared randomized op stream over two independent worlds, then compares the
 * observable descriptor/LRU/hash/mapping/dirty/free state after every op.
 *
 * Exit 0 on success; on mismatch prints the op index and both projections and
 * exits 1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NHASH 262144u
#define NBUCKET 1024u
#define NPAGE 96
#define NVN 4

typedef struct PPage {
    void *vnode;
    uint64_t index;
    int ref_count;
    int valid;
    int dirty;
    uint64_t dirty_gen;
    uint64_t invalidate_gen;
    int uptodate;
    unsigned char accessed;
    uint32_t pfn;
    void *data;
    struct PPage *prev, *next, *hnext;
    struct PPage *mapping_prev, *mapping_next;
    struct PPage *dirty_prev, *dirty_next;
    struct PPage *global_dirty_prev, *global_dirty_next;
    struct PPage *dirty_children[2];
    uintptr_t dirty_parent_color;
} PPage;

typedef struct VNode {
    uint64_t ino;
    int vtype;
    uint32_t mode, uid, gid;
    size_t size;
    int ref_count;
    void *parent;
    void *mnt;
    void *fs_data;
    void *ops;
    PPage *cache_pages;
    PPage *cache_dirty_pages;
    PPage *cache_dirty_tail;
    PPage *cache_dirty_root;
} VNode;

typedef struct PinCounter {
    size_t pinned;
    char pad[64 - sizeof(size_t)];
} PinCounter;

typedef struct World {
    PPage pages[NPAGE];
    PPage lru_head, lru_tail;
    PPage *free_head;
    PPage *hash[NHASH];
    PPage *global_dirty;
    size_t dirty_count;
    size_t valid_pages;
    VNode vn[NVN];
    PinCounter pinned[NBUCKET];
} World;

/* Rust ABI */
extern unsigned a20_pagecache_hash_key(VNode *, uint64_t);
extern void a20_pagecache_lru_remove(PPage *);
extern void a20_pagecache_lru_insert_front(PPage *, PPage *);
extern void a20_pagecache_lru_insert_tail(PPage *, PPage *);
extern void a20_pagecache_free_insert(PPage **, PPage *);
extern PPage *a20_pagecache_free_take(PPage **, PPage *);
extern void a20_pagecache_hash_insert(PPage **, PPage *);
extern void a20_pagecache_hash_remove(PPage **, PPage *, void (*)(size_t));
extern PPage *a20_pagecache_find(PPage **, VNode *, uint64_t, void (*)(size_t));
extern void a20_pagecache_mapping_insert(PPage *);
extern void a20_pagecache_mapping_remove(PPage *);
extern void a20_pagecache_dirty_insert(PPage *, PPage **, size_t *);
extern void a20_pagecache_dirty_remove(PPage *, PPage **, size_t *);
extern VNode *a20_pagecache_detach(PPage *, PPage **, size_t *, PPage **, size_t *,
                                   void (*)(size_t));
extern void a20_pagecache_pin(PPage *, size_t *);
extern int a20_pagecache_put(PPage *, size_t *);
extern size_t a20_pagecache_pinned_total(const size_t *);
extern PPage *a20_pagecache_evict_scan(PPage *, PPage *, size_t,
                                       int (*)(uint32_t), uint64_t (*)(uint32_t),
                                       void (*)(uint32_t, uint64_t), PPage **,
                                       PPage **, size_t *, size_t *,
                                       void (*)(size_t), VNode **);
extern size_t a20_pagecache_collect_dirty(VNode *, PPage *, size_t, PPage **,
                                          size_t *, void (*)(size_t));
extern size_t a20_pagecache_writeback_confirm(PPage **, size_t, VNode *, uint64_t,
                                              const uint64_t *, PPage **, size_t *);
extern size_t a20_pagecache_detach_unpinned_range(VNode *, uint64_t, uint64_t, int,
                                                  uint64_t (*)(uint32_t),
                                                  void (*)(uint32_t, uint64_t),
                                                  PPage **, PPage **, size_t *,
                                                  size_t *, void (*)(size_t),
                                                  size_t *);
extern size_t a20_pagecache_truncate(VNode *, uint64_t, uint64_t (*)(uint32_t),
                                     void (*)(uint32_t, uint64_t), PPage **,
                                     PPage **, size_t *, size_t *,
                                     void (*)(size_t));
extern VNode *a20_pagecache_reclaim_clean(PPage *, uint64_t (*)(uint32_t),
                                          void (*)(uint32_t, uint64_t),
                                          int (*)(uint32_t), PPage **, PPage **,
                                          size_t *, size_t *, PPage **,
                                          void (*)(size_t));

/* ---------------- shared instrumentation ---------------- */
static size_t g_scan_entries;
static void scan_cb(size_t n) { g_scan_entries += n; }
static int g_lock_calls, g_unlock_calls;
static uint64_t lock_cb(uint32_t blk)
{
    (void)blk;
    g_lock_calls++;
    return (uint64_t)g_lock_calls;
}
static void unlock_cb(uint32_t blk, uint64_t flags)
{
    (void)blk;
    (void)flags;
    g_unlock_calls++;
}
static unsigned char g_reclaimable[1024];
static int reclaimable_cb(uint32_t pfn)
{
    return pfn < 1024 && g_reclaimable[pfn];
}

/* ---------------- C reference model ---------------- */
static unsigned ref_hash_key(VNode *vn, uint64_t index)
{
    uintptr_t v = vn && vn->mnt ? (uintptr_t)vn->mnt ^ (uintptr_t)vn->ino
                                : (uintptr_t)vn;
    return (unsigned)((v >> 4) ^ index ^ (index >> 32)) & (NHASH - 1);
}

static void ref_lru_remove(PPage *p)
{
    p->prev->next = p->next;
    p->next->prev = p->prev;
    p->prev = p->next = NULL;
}
static void ref_lru_front(PPage *head, PPage *p)
{
    p->next = head->next;
    p->prev = head;
    head->next->prev = p;
    head->next = p;
}
static void ref_lru_tail(PPage *tail, PPage *p)
{
    p->prev = tail->prev;
    p->next = tail;
    tail->prev->next = p;
    tail->prev = p;
}
static void ref_free_insert(PPage **head, PPage *p)
{
    p->hnext = *head;
    *head = p;
}
static PPage *ref_free_take(World *w)
{
    PPage *p = w->free_head;
    if (!p)
        return NULL;
    w->free_head = p->hnext;
    p->hnext = NULL;
    p->ref_count = 1;
    ref_lru_remove(p);
    ref_lru_front(&w->lru_head, p);
    return p;
}
static void ref_hash_insert(World *w, PPage *p)
{
    unsigned idx = ref_hash_key((VNode *)p->vnode, p->index);
    p->hnext = w->hash[idx];
    w->hash[idx] = p;
}
static void ref_hash_remove(World *w, PPage *p)
{
    unsigned idx = ref_hash_key((VNode *)p->vnode, p->index);
    PPage **pp = &w->hash[idx];
    size_t visited = 0;
    while (*pp) {
        visited++;
        if (*pp == p) {
            *pp = p->hnext;
            p->hnext = NULL;
            scan_cb(visited);
            return;
        }
        pp = &(*pp)->hnext;
    }
    scan_cb(visited);
    p->hnext = NULL;
}
static PPage *ref_find(World *w, VNode *vn, uint64_t index)
{
    unsigned idx = ref_hash_key(vn, index);
    size_t visited = 0;
    for (PPage *p = w->hash[idx]; p; p = p->hnext) {
        visited++;
        if (p->valid && p->vnode == vn && p->index == index) {
            scan_cb(visited);
            return p;
        }
    }
    scan_cb(visited);
    return NULL;
}
static void ref_mapping_insert(PPage *p)
{
    VNode *vn = (VNode *)p->vnode;
    p->mapping_prev = NULL;
    p->mapping_next = vn->cache_pages;
    if (vn->cache_pages)
        vn->cache_pages->mapping_prev = p;
    vn->cache_pages = p;
}
static void ref_mapping_remove(PPage *p)
{
    VNode *vn = (VNode *)p->vnode;
    if (p->mapping_prev)
        p->mapping_prev->mapping_next = p->mapping_next;
    else if (vn)
        vn->cache_pages = p->mapping_next;
    if (p->mapping_next)
        p->mapping_next->mapping_prev = p->mapping_prev;
    p->mapping_prev = p->mapping_next = NULL;
}
static void ref_dirty_insert(World *w, PPage *p)
{
    if (p->dirty || !p->vnode)
        return;
    VNode *vn = (VNode *)p->vnode;
    PPage *head = vn->cache_dirty_pages, *tail = vn->cache_dirty_tail;
    if (!head) {
        p->dirty_prev = p->dirty_next = NULL;
        vn->cache_dirty_pages = vn->cache_dirty_tail = p;
    } else if (p->index >= head->index) {
        p->dirty_prev = NULL;
        p->dirty_next = head;
        head->dirty_prev = p;
        vn->cache_dirty_pages = p;
    } else if (p->index <= tail->index) {
        p->dirty_prev = tail;
        p->dirty_next = NULL;
        tail->dirty_next = p;
        vn->cache_dirty_tail = p;
    } else {
        PPage *at = head;
        while (at->dirty_next && at->dirty_next->index > p->index)
            at = at->dirty_next;
        p->dirty_prev = at;
        p->dirty_next = at->dirty_next;
        at->dirty_next->dirty_prev = p;
        at->dirty_next = p;
    }
    p->global_dirty_prev = NULL;
    p->global_dirty_next = w->global_dirty;
    if (w->global_dirty)
        w->global_dirty->global_dirty_prev = p;
    w->global_dirty = p;
    p->dirty = 1;
    w->dirty_count++;
}
static void ref_dirty_remove(World *w, PPage *p)
{
    if (!p->dirty)
        return;
    VNode *vn = (VNode *)p->vnode;
    if (p->dirty_prev)
        p->dirty_prev->dirty_next = p->dirty_next;
    else if (vn)
        vn->cache_dirty_pages = p->dirty_next;
    if (p->dirty_next)
        p->dirty_next->dirty_prev = p->dirty_prev;
    else if (vn)
        vn->cache_dirty_tail = p->dirty_prev;
    if (p->global_dirty_prev)
        p->global_dirty_prev->global_dirty_next = p->global_dirty_next;
    else
        w->global_dirty = p->global_dirty_next;
    if (p->global_dirty_next)
        p->global_dirty_next->global_dirty_prev = p->global_dirty_prev;
    p->dirty_prev = p->dirty_next = NULL;
    p->global_dirty_prev = p->global_dirty_next = NULL;
    p->dirty = 0;
    w->dirty_count--;
}
static VNode *ref_detach(World *w, PPage *p)
{
    if (!p->valid)
        return NULL;
    ref_dirty_remove(w, p);
    ref_mapping_remove(p);
    ref_hash_remove(w, p);
    VNode *vn = (VNode *)p->vnode;
    p->vnode = NULL;
    p->index = 0;
    p->valid = 0;
    p->dirty = 0;
    p->dirty_gen = 0;
    p->invalidate_gen++;
    p->uptodate = 0;
    w->valid_pages--;
    return vn;
}
static void ref_pin(World *w, PPage *p)
{
    if (p->ref_count++ == 0) {
        unsigned idx = ref_hash_key((VNode *)p->vnode, p->index);
        __atomic_fetch_add(&w->pinned[idx & (NBUCKET - 1)].pinned, 1,
                           __ATOMIC_RELAXED);
    }
}
static int ref_put(World *w, PPage *p)
{
    if (p->ref_count > 0 && --p->ref_count == 0) {
        unsigned idx = ref_hash_key((VNode *)p->vnode, p->index);
        __atomic_fetch_sub(&w->pinned[idx & (NBUCKET - 1)].pinned, 1,
                           __ATOMIC_RELAXED);
        return 1;
    }
    return 0;
}
static PPage *ref_evict(World *w, size_t budget, VNode **deferred)
{
    PPage *page = w->lru_tail.prev;
    size_t visited = 0;
    *deferred = NULL;
    while (page != &w->lru_head && visited < budget) {
        visited++;
        PPage *next = page->prev;
        ref_lru_remove(page);
        ref_lru_front(&w->lru_head, page);
        if (page->ref_count == 0 && !page->dirty &&
            reclaimable_cb(page->pfn)) {
            if (page->valid) {
                unsigned idx = ref_hash_key((VNode *)page->vnode, page->index);
                uint64_t bflags = lock_cb(idx & (NBUCKET - 1));
                if (page->valid && page->ref_count == 0 && !page->dirty) {
                    if (page->accessed) {
                        page->accessed = 0;
                        unlock_cb(idx & (NBUCKET - 1), bflags);
                        page = next;
                        continue;
                    }
                    *deferred = ref_detach(w, page);
                }
                unlock_cb(idx & (NBUCKET - 1), bflags);
                if (!page->valid) {
                    page->ref_count = 1;
                    ref_lru_remove(page);
                    ref_lru_front(&w->lru_head, page);
                    scan_cb(visited);
                    return page;
                }
                page = next;
                continue;
            }
            page->ref_count = 1;
            ref_lru_remove(page);
            ref_lru_front(&w->lru_head, page);
            scan_cb(visited);
            return page;
        }
        page = next;
    }
    scan_cb(visited);
    return NULL;
}

/* ---------------- worlds/helpers ---------------- */
static void world_init(World *w)
{
    memset(w, 0, sizeof(*w));
    w->lru_head.next = &w->lru_tail;
    w->lru_tail.prev = &w->lru_head;
    for (int i = 0; i < NVN; i++) {
        w->vn[i].ino = (uint64_t)(i + 1);
        w->vn[i].vtype = 1;
        /* A mounted vnode always carries a non-null mount (ramfs/procfs assign
         * it; ext4/fat/ntfs/uxfs/isofs inherit it), so the hash key is
         * (mnt ^ ino) and never the vnode address.  Both worlds must compute
         * the same bucket for the same (vnode, index): with a null mnt the key
         * degenerates to the stack address of that world's vnode array, the two
         * worlds land in different buckets, and the shared hnext field then
         * makes the free list observably diverge for reasons that have nothing
         * to do with the engine under test. */
        w->vn[i].mnt = (void *)(uintptr_t)(0x4000u + (unsigned)i * 0x40u);
    }
    for (int i = 0; i < NPAGE; i++) {
        w->pages[i].pfn = (uint32_t)i;
        w->pages[i].data = &w->pages[i];
        g_reclaimable[i] = 1;
        ref_lru_tail(&w->lru_tail, &w->pages[i]);
        ref_free_insert(&w->free_head, &w->pages[i]);
    }
}

static int page_index(World *w, PPage *p)
{
    if (!p)
        return -1;
    return (int)(p - w->pages);
}

/* Per-page bookkeeping shared by both worlds (operations are only issued in
 * states where the state machine's preconditions hold). */
typedef struct Flags {
    int in_free;
    int in_hash;
    int dirty;
} Flags;
static Flags g_flags[NPAGE];

static void proj(World *w, char *buf, size_t cap)
{
    size_t off = 0;
    off += (size_t)snprintf(buf + off, cap - off,
                            "valid=%zu dirty=%zu free=%d gd=%d gp=%zu\n",
                            w->valid_pages, w->dirty_count,
                            page_index(w, w->free_head),
                            page_index(w, w->global_dirty),
                            a20_pagecache_pinned_total((const size_t *)w->pinned));
    for (int i = 0; i < NPAGE && off < cap; i++) {
        PPage *p = &w->pages[i];
        off += (size_t)snprintf(buf + off, cap - off,
                                "%d:v%d h%d d%d r%d idx%llu acc%d pfn%u "
                                "pp%d pn%d hp%d mp%d mn%d dp%d dn%d gp%d gn%d\n",
                                i, p->valid, g_flags[i].in_hash, p->dirty,
                                p->ref_count, (unsigned long long)p->index,
                                p->accessed, p->pfn, page_index(w, p->prev),
                                page_index(w, p->next), page_index(w, p->hnext),
                                page_index(w, p->mapping_prev),
                                page_index(w, p->mapping_next),
                                page_index(w, p->dirty_prev),
                                page_index(w, p->dirty_next),
                                page_index(w, p->global_dirty_prev),
                                page_index(w, p->global_dirty_next));
    }
    for (int i = 0; i < NVN && off < cap; i++) {
        off += (size_t)snprintf(buf + off, cap - off, "vn%d m%d dm%d dt%d\n",
                                i, page_index(w, w->vn[i].cache_pages),
                                page_index(w, w->vn[i].cache_dirty_pages),
                                page_index(w, w->vn[i].cache_dirty_tail));
    }
}

static uint64_t rng_state;
static uint64_t rnd(void)
{
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return rng_state >> 17;
}

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 20000;
    rng_state = 0x9e3779b97f4a7c15ull;
    World c, r;
    world_init(&c);
    world_init(&r);
    memset(g_flags, 0, sizeof(g_flags));
    for (int i = 0; i < NPAGE; i++)
        g_flags[i].in_free = 1;

    for (unsigned step = 0; step < rounds; step++) {
        uint64_t op = rnd() % 12;
        int pi = (int)(rnd() % NPAGE);
        PPage *cp = &c.pages[pi], *rp = &r.pages[pi];
        VNode *cvn = &c.vn[rnd() % NVN];
        VNode *rvn = &r.vn[cvn - c.vn];

        switch (op) {
        case 0: /* publish: free page -> hash+mapping */
            if (g_flags[pi].in_hash || cp->valid)
                break;
            cp->vnode = cvn; rp->vnode = rvn;
            cp->index = rp->index = rnd() % 4096;
            cp->valid = rp->valid = 1;
            ref_hash_insert(&c, cp);
            a20_pagecache_hash_insert(r.hash, rp);
            ref_mapping_insert(cp);
            a20_pagecache_mapping_insert(rp);
            c.valid_pages++; r.valid_pages++;
            g_flags[pi].in_hash = 1;
            if (ref_find(&c, cvn, cp->index) != cp ||
                a20_pagecache_find(r.hash, rvn, rp->index, scan_cb) != rp)
                goto mismatch;
            break;
        case 1: /* detach */
            if (!g_flags[pi].in_hash)
                break;
            {
                VNode *a = ref_detach(&c, cp);
                VNode *b = a20_pagecache_detach(rp, &r.global_dirty, &r.dirty_count,
                                                r.hash, &r.valid_pages, scan_cb);
                g_flags[pi].in_hash = 0;
                g_flags[pi].dirty = 0;
                if ((a == NULL) != (b == NULL))
                    goto mismatch;
            }
            break;
        case 2: /* dirty insert */
            if (!g_flags[pi].in_hash)
                break;
            ref_dirty_insert(&c, cp);
            a20_pagecache_dirty_insert(rp, &r.global_dirty, &r.dirty_count);
            g_flags[pi].dirty = 1;
            break;
        case 3: /* dirty remove */
            if (!g_flags[pi].in_hash)
                break;
            ref_dirty_remove(&c, cp);
            a20_pagecache_dirty_remove(rp, &r.global_dirty, &r.dirty_count);
            g_flags[pi].dirty = 0;
            break;
        case 4: /* pin */
            ref_pin(&c, cp);
            a20_pagecache_pin(rp, (size_t *)r.pinned);
            break;
        case 5: /* put */
            if (cp->ref_count > 0) {
                (void)ref_put(&c, cp);
                (void)a20_pagecache_put(rp, (size_t *)r.pinned);
            }
            break;
        case 6: /* free insert */
            if (g_flags[pi].in_free)
                break;
            ref_free_insert(&c.free_head, cp);
            a20_pagecache_free_insert(&r.free_head, rp);
            g_flags[pi].in_free = 1;
            break;
        case 7: /* free take */
            if (!c.free_head)
                break;
            {
                PPage *a = ref_free_take(&c);
                PPage *b = a20_pagecache_free_take(&r.free_head, &r.lru_head);
                g_flags[page_index(&c, a)].in_free = 0;
                if (page_index(&c, a) != page_index(&r, b))
                    goto mismatch;
            }
            break;
        case 8: /* evict scan */
            {
                VNode *da = NULL, *db = NULL;
                PPage *a = ref_evict(&c, 128, &da);
                PPage *b = a20_pagecache_evict_scan(
                    &r.lru_head, &r.lru_tail, 128, reclaimable_cb, lock_cb,
                    unlock_cb, r.hash, &r.global_dirty, &r.dirty_count,
                    &r.valid_pages, scan_cb, &db);
                if (page_index(&c, a) != page_index(&r, b))
                    goto mismatch;
                if (a) {
                    int ai = page_index(&c, a);
                    g_flags[ai].in_free = 0;
                    g_flags[ai].in_hash = 0;
                    g_flags[ai].dirty = 0;
                }
            }
            break;
        case 9: /* collect dirty batch + unpin */
            {
                PPage *a[64], *b[64];
                memset(a, 0, sizeof(a));
                memset(b, 0, sizeof(b));
                VNode *use_c = (rnd() & 1) ? cvn : NULL;
                VNode *use_r = use_c ? &r.vn[use_c - c.vn] : NULL;
                size_t na = 0, nb = 0;
                /* Reference collector. */
                {
                    PPage *p = use_c ? use_c->cache_dirty_tail
                                     : (c.global_dirty && c.global_dirty->vnode
                                            ? ((VNode *)c.global_dirty->vnode)->cache_dirty_tail
                                            : NULL);
                    if (p) {
                        VNode *bv = (VNode *)p->vnode;
                        uint64_t ni = p->index;
                        while (p && na < 64 && p->vnode == bv && p->index == ni) {
                            ref_pin(&c, p);
                            a[na++] = p;
                            ni++;
                            p = p->dirty_prev;
                        }
                    }
                }
                nb = a20_pagecache_collect_dirty(use_r, r.global_dirty, 64, b,
                                                 (size_t *)r.pinned, scan_cb);
                if (na != nb)
                    goto mismatch;
                for (size_t i = 0; i < na; i++) {
                    if (page_index(&c, a[i]) != page_index(&r, b[i]))
                        goto mismatch;
                    (void)ref_put(&c, a[i]);
                    (void)a20_pagecache_put(b[i], (size_t *)r.pinned);
                }
            }
            break;
        case 10: /* writeback confirm on the dirty pages of vn 0 */
            {
                PPage *pa[64], *pb[64];
                uint64_t gens[64];
                size_t n = 0;
                for (int i = 0; i < NPAGE && n < 64; i++) {
                    if (c.pages[i].valid && c.pages[i].vnode == &c.vn[0] &&
                        c.pages[i].dirty) {
                        pa[n] = &c.pages[i];
                        pb[n] = &r.pages[i];
                        gens[n] = c.pages[i].dirty_gen;
                        n++;
                    }
                }
                if (n) {
                    /* Reference clear. */
                    for (size_t i = 0; i < n; i++) {
                        if (pa[i]->valid && pa[i]->vnode == (void *)&c.vn[0] &&
                            pa[i]->index == pa[0]->index + i &&
                            pa[i]->dirty_gen == gens[i])
                            ref_dirty_remove(&c, pa[i]);
                    }
                    size_t got = a20_pagecache_writeback_confirm(
                        pb, n, &r.vn[0], pa[0]->index, gens, &r.global_dirty,
                        &r.dirty_count);
                    size_t want = 0;
                    (void)got;
                    for (size_t i = 0; i < n; i++)
                        if (!c.pages[page_index(&c, pa[i])].dirty)
                            want++;
                    (void)want;
                    for (size_t i = 0; i < n; i++)
                        g_flags[page_index(&c, pa[i])].dirty =
                            c.pages[page_index(&c, pa[i])].dirty;
                }
            }
            break;
        case 11: /* reclaim clean */
            if (g_flags[pi].in_hash && !cp->dirty && cp->ref_count == 0) {
                VNode *a = a20_pagecache_reclaim_clean(
                    rp, lock_cb, unlock_cb, reclaimable_cb, r.hash,
                    &r.global_dirty, &r.dirty_count, &r.valid_pages,
                    &r.free_head, scan_cb);
                if (a) {
                    ref_dirty_remove(&c, cp);
                    ref_mapping_remove(cp);
                    ref_hash_remove(&c, cp);
                    cp->vnode = NULL; cp->valid = 0; cp->dirty = 0;
                    cp->index = 0; cp->invalidate_gen++; cp->uptodate = 0;
                    c.valid_pages--;
                    ref_free_insert(&c.free_head, cp);
                    g_flags[pi].in_hash = 0;
                    g_flags[pi].dirty = 0;
                    g_flags[pi].in_free = 1;
                }
            }
            break;
        }
        continue;
    mismatch:
        {
            static char ba[65536], bb[65536];
            proj(&c, ba, sizeof(ba));
            proj(&r, bb, sizeof(bb));
            fprintf(stderr, "mismatch at step %u op %llu page %d\nC:\n%s\nRUST:\n%s\n",
                    step, (unsigned long long)op, pi, ba, bb);
            return 1;
        }
    }
    printf("page cache state C/Rust differential PASS (%u ops, scan=%zu, locks=%d/%d)\n",
           rounds, g_scan_entries, g_lock_calls, g_unlock_calls);
    return 0;
}
