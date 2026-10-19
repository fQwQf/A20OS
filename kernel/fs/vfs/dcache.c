#include "fs/vfs/dcache.h"
#include "core/lock.h"
#include "core/lock_counters.h"
#include "core/string.h"

/*
 * Dentry cache state engine selector.  Rust owns the slot table's state
 * transitions (name hash, hash-chain link/unlink, LRU, free list, exact-match
 * search with second chance, eviction policy, bucket drop).  The C adapter
 * keeps the global/bucket spinlocks, the vnode references and the retire
 * batch.  The C reference stays for VFS_DCACHE_STATE_IMPL=c.
 */
/* Provided by kernel/fs/vfs/dcache_state.rs.  Every entry needs the lock the
 * C contract names; none takes a lock itself. */
extern uint32_t a20_dcache_hash_key(uintptr_t mnt, uint64_t ino, const char *name);
extern void a20_dcache_init(void *entries, size_t n, int *hash, size_t hash_size,
                            int *free_list, int *free_count, int *lru_head,
                            int *lru_tail);
extern void a20_dcache_link_hash(void *entries, int *hash, int slot, uint32_t h);
extern void a20_dcache_unlink_hash(void *entries, int *hash, int slot, uint32_t h);
extern void a20_dcache_lru_remove(void *entries, int *lru_head, int *lru_tail, int slot);
extern void a20_dcache_lru_insert_front(void *entries, int *lru_head, int *lru_tail, int slot);
extern void a20_dcache_lru_touch(void *entries, int *lru_head, int *lru_tail, int slot);
extern int a20_dcache_alloc_free(void *entries, size_t n, int *free_list, int *free_count);
extern void a20_dcache_free_push(void *entries, int *free_list, int *free_count, int slot);
extern int a20_dcache_match(void *entries, int *hash, uint32_t h, uintptr_t mnt, uint64_t ino,
                            const char *name);
extern void a20_dcache_mark_accessed(void *entries, int slot);
extern int a20_dcache_evict_decide(int used, int accessed);
extern unsigned a20_dcache_drop_chain(void *entries, int *hash, uint32_t h, uintptr_t *retire,
                                      unsigned *retire_count, size_t retire_cap);

#if defined(CONFIG_VFS_DCACHE_STATE_RUST) && MAX_NAME_LEN != 256
#error "VFS_DCACHE_STATE_RUST mirror assumes MAX_NAME_LEN==256 (dcache is not built on the 32-byte profile)"
#endif

#define VFS_DCACHE_MAX 2048
#define VFS_DCACHE_HASH_BITS 9
#define VFS_DCACHE_HASH_SIZE (1U << VFS_DCACHE_HASH_BITS)
#define VFS_DCACHE_HASH_MASK (VFS_DCACHE_HASH_SIZE - 1)
/* Independent per-hash-group spinlocks.  A hit takes only its bucket lock;
 * slot allocation, the free list and the LRU remain under the single global
 * lock, whose critical sections are short and only run on insert/evict. */
#define VFS_DCACHE_BUCKET_LOCKS 64

typedef struct {
    int used;
    int hash_next;
    int hash_prev;
    int free_next;
    int lru_next;
    int lru_prev;
    mount_t *mnt;
    uint64_t parent_ino;
    char name[MAX_NAME_LEN];
    vnode_t *vn;
    /* Second-chance reference: set on hit under the bucket lock and cleared
     * by the evictor, so hits need not mutate the global LRU. */
    unsigned char accessed;
} vfs_dcache_entry_t;

static spinlock_t g_dcache_lock = SPINLOCK_INIT;
static spinlock_t g_dcache_bucket_locks[VFS_DCACHE_BUCKET_LOCKS];
static vfs_dcache_entry_t g_dcache[VFS_DCACHE_MAX];
static int g_dcache_hash[VFS_DCACHE_HASH_SIZE];
static int g_dcache_free_list;
static int g_dcache_free_count;
static int g_dcache_lru_head;
static int g_dcache_lru_tail;
static int g_dcache_initialized;

static uint32_t dcache_hash_key(mount_t *mnt, uint64_t ino, const char *name)
{
    return a20_dcache_hash_key((uintptr_t)mnt, ino, name);
}

/* Lock ordering: callers may hold g_dcache_lock and then a bucket lock, but
 * never acquire g_dcache_lock while holding a bucket lock. */
static inline uint64_t dcache_bucket_lock_irqsave(uint32_t h)
{
    return spin_lock_irqsave(&g_dcache_bucket_locks[h & (VFS_DCACHE_BUCKET_LOCKS - 1)]);
}

static inline void dcache_bucket_unlock_irqrestore(uint32_t h, uint64_t flags)
{
    spin_unlock_irqrestore(&g_dcache_bucket_locks[h & (VFS_DCACHE_BUCKET_LOCKS - 1)],
                           flags);
}

/* Callers must hold the entry's bucket lock for the hash links and
 * g_dcache_lock for the LRU/free structures. */
static void dcache_unlink_hash(int slot, uint32_t h)
{
    a20_dcache_unlink_hash(g_dcache, g_dcache_hash, slot, h);
}

static void dcache_link_hash(int slot, uint32_t h)
{
    a20_dcache_link_hash(g_dcache, g_dcache_hash, slot, h);
}

static void dcache_init_locked(void)
{
    if (g_dcache_initialized)
        return;
    a20_dcache_init(g_dcache, VFS_DCACHE_MAX, g_dcache_hash,
                    VFS_DCACHE_HASH_SIZE, &g_dcache_free_list,
                    &g_dcache_free_count, &g_dcache_lru_head,
                    &g_dcache_lru_tail);
    g_dcache_initialized = 1;
    lock_counters_register(&g_dcache_lock, "dcache");
}

static void dcache_lru_remove(int slot)
{
    a20_dcache_lru_remove(g_dcache, &g_dcache_lru_head, &g_dcache_lru_tail, slot);
}

static void dcache_lru_insert_front(int slot)
{
    a20_dcache_lru_insert_front(g_dcache, &g_dcache_lru_head, &g_dcache_lru_tail, slot);
}

static void dcache_lru_touch(int slot)
{
    a20_dcache_lru_touch(g_dcache, &g_dcache_lru_head, &g_dcache_lru_tail, slot);
}

/* Caller holds g_dcache_lock.  Free-list path needs no bucket lock; the
 * second-chance LRU path takes the evicted slot's bucket lock so the
 * vnode_get() in a concurrent lookup is atomic against slot reuse. */
static int dcache_alloc_slot(vnode_t **old_vn)
{
    int slot = a20_dcache_alloc_free(g_dcache, VFS_DCACHE_MAX,
                                     &g_dcache_free_list, &g_dcache_free_count);
    if (slot >= 0)
        return slot;

    for (;;) {
        int victim = g_dcache_lru_tail;
        if (victim < 0 || victim >= VFS_DCACHE_MAX)
            return -1;
        vfs_dcache_entry_t *e = &g_dcache[victim];
        if (!e->used) {
            dcache_lru_remove(victim);
            return victim;
        }
        uint32_t h = dcache_hash_key(e->mnt, e->parent_ino, e->name);
        uint64_t bf = dcache_bucket_lock_irqsave(h);
        int decision = a20_dcache_evict_decide(e->used, e->accessed);
        if (decision == 2) { /* second chance */
            e->accessed = 0;
            dcache_bucket_unlock_irqrestore(h, bf);
            dcache_lru_touch(victim);
            continue;
        }
        *old_vn = e->vn;
        dcache_unlink_hash(victim, h);
        dcache_lru_remove(victim);
        dcache_bucket_unlock_irqrestore(h, bf);
        return victim;
    }
}

static int vfs_dcache_enabled_for(vnode_t *dir)
{
    if (!dir || !dir->mnt)
        return 0;
    return dir->mnt->type == FS_TYPE_RAMFS ||
           dir->mnt->type == FS_TYPE_FAT32 ||
           dir->mnt->type == FS_TYPE_EXT4;
}

vnode_t *vfs_dcache_lookup(vnode_t *dir, const char *name)
{
    if (!vfs_dcache_enabled_for(dir) || !name || !*name)
        return NULL;

    uint32_t h = dcache_hash_key(dir->mnt, dir->ino, name);
    uint64_t bf = dcache_bucket_lock_irqsave(h);
    if (!g_dcache_initialized) {
        dcache_bucket_unlock_irqrestore(h, bf);
        uint64_t flags = spin_lock_irqsave(&g_dcache_lock);
        dcache_init_locked();
        spin_unlock_irqrestore(&g_dcache_lock, flags);
        bf = dcache_bucket_lock_irqsave(h);
    }
    int slot = a20_dcache_match(g_dcache, g_dcache_hash, h, (uintptr_t)dir->mnt,
                                dir->ino, name);
    if (slot >= 0) {
        vfs_dcache_entry_t *e = &g_dcache[slot];
        if (vnode_get_unless_zero(e->vn)) {
            a20_dcache_mark_accessed(g_dcache, slot);
            vnode_t *vn = e->vn;
            dcache_bucket_unlock_irqrestore(h, bf);
            return vn;
        }
    }
    dcache_bucket_unlock_irqrestore(h, bf);
    return NULL;
}

void vfs_dcache_insert(vnode_t *dir, const char *name, vnode_t *vn)
{
    if (!vfs_dcache_enabled_for(dir) || !name || !*name || !vn)
        return;

    uint32_t h = dcache_hash_key(dir->mnt, dir->ino, name);

    uint64_t flags = spin_lock_irqsave(&g_dcache_lock);
    dcache_init_locked();
    uint64_t bf = dcache_bucket_lock_irqsave(h);
    for (int i = g_dcache_hash[h]; i >= 0; i = g_dcache[i].hash_next) {
        vfs_dcache_entry_t *e = &g_dcache[i];
        if (e->used && e->mnt == dir->mnt && e->parent_ino == dir->ino &&
            strcmp(e->name, name) == 0) {
            e->accessed = 1;
            dcache_bucket_unlock_irqrestore(h, bf);
            spin_unlock_irqrestore(&g_dcache_lock, flags);
            return;
        }
    }
    dcache_bucket_unlock_irqrestore(h, bf);

    /* Slot allocation may take the evicted slot's bucket lock (possibly the
     * same bucket as h), so it must not run while holding h.  Inserts are
     * serialized by g_dcache_lock, so a concurrent lookup only ever observes
     * a fully linked entry and can never see a partially initialised one. */
    vnode_t *old_vn = NULL;
    int slot = dcache_alloc_slot(&old_vn);
    if (slot < 0) {
        spin_unlock_irqrestore(&g_dcache_lock, flags);
        return;
    }

    vfs_dcache_entry_t *e = &g_dcache[slot];
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->mnt = dir->mnt;
    e->parent_ino = dir->ino;
    strncpy(e->name, name, MAX_NAME_LEN - 1);
    e->vn = vn;
    e->accessed = 1;
    e->hash_next = -1;
    e->hash_prev = -1;
    e->free_next = -1;
    e->lru_next = -1;
    e->lru_prev = -1;
    bf = dcache_bucket_lock_irqsave(h);
    dcache_link_hash(slot, h);
    dcache_lru_insert_front(slot);
    vnode_get(vn);
    dcache_bucket_unlock_irqrestore(h, bf);
    spin_unlock_irqrestore(&g_dcache_lock, flags);

    if (old_vn)
        vnode_put(old_vn);
}

void vfs_dcache_invalidate(vnode_t *dir, const char *name)
{
    if (!dir || !name) return;
    uint32_t h = dcache_hash_key(dir->mnt, dir->ino, name);

    uint64_t flags = spin_lock_irqsave(&g_dcache_lock);
    dcache_init_locked();
    uint64_t bf = dcache_bucket_lock_irqsave(h);
    for (int i = g_dcache_hash[h]; i >= 0; i = g_dcache[i].hash_next) {
        vfs_dcache_entry_t *e = &g_dcache[i];
        if (e->used && e->mnt == dir->mnt && e->parent_ino == dir->ino &&
            strcmp(e->name, name) == 0) {
            vnode_t *vn = e->vn;
            dcache_unlink_hash(i, h);
            dcache_lru_remove(i);
            memset(e, 0, sizeof(*e));
            e->hash_next = -1;
            e->hash_prev = -1;
            e->lru_next = -1;
            e->lru_prev = -1;
            a20_dcache_free_push(g_dcache, &g_dcache_free_list,
                                 &g_dcache_free_count, i);
            dcache_bucket_unlock_irqrestore(h, bf);
            spin_unlock_irqrestore(&g_dcache_lock, flags);
            if (vn) vnode_put(vn);
            return;
        }
    }
    dcache_bucket_unlock_irqrestore(h, bf);
    spin_unlock_irqrestore(&g_dcache_lock, flags);
}

/* Serialises invalidate_all() against itself and holds the batch of retired
 * vnode references, which is too large for the kernel stack.  Nothing on any
 * lookup path takes it, so it can never be part of a cycle. */
static spinlock_t g_dcache_invalidate_lock = SPINLOCK_INIT;
static vnode_t *g_dcache_retire[VFS_DCACHE_MAX];
static unsigned g_dcache_retire_count;

static void __attribute__((unused)) dcache_retire_push(vnode_t *vn)
{
    g_dcache_retire[g_dcache_retire_count++] = vn;
}

static void dcache_retire_drain(void)
{
    unsigned n = g_dcache_retire_count;
    g_dcache_retire_count = 0;
    for (unsigned i = 0; i < n; i++)
        vnode_put(g_dcache_retire[i]);
}

/* Unlink one hash chain and hand its vnode references to the retire batch.
 * Caller holds the bucket lock that covers @h and g_dcache_invalidate_lock.
 * The slot is left on the LRU as an unused entry, which is what
 * dcache_alloc_slot() already knows how to reclaim, so dropping the whole
 * table needs no LRU or free-list pass under the global lock. */
static void dcache_drop_chain(unsigned h)
{
    a20_dcache_drop_chain(g_dcache, g_dcache_hash, h, (uintptr_t *)g_dcache_retire,
                          &g_dcache_retire_count, VFS_DCACHE_MAX);
}

void vfs_dcache_invalidate_all(void)
{
    /*
     * Retire one bucket at a time.  Collecting the whole table first meant
     * every VFS_DCACHE_BUCKET_LOCKS lock was held, with interrupts disabled,
     * across all the acquisitions and a VFS_DCACHE_MAX-entry scan, so an
     * interrupt that reached any lookup spun on a lock its own interrupted
     * context was holding.  Emptying a bucket and releasing it before moving
     * on bounds the masked region to a single bucket, and an entry linked
     * after its bucket has been emptied is a cache entry created during the
     * invalidation, which is exactly as valid as one that survived it.
     */
    uint64_t inv = spin_lock_irqsave(&g_dcache_invalidate_lock);
    for (unsigned b = 0; b < VFS_DCACHE_BUCKET_LOCKS; b++) {
        uint64_t flags = spin_lock_irqsave(&g_dcache_lock);
        dcache_init_locked();
        spin_unlock_irqrestore(&g_dcache_lock, flags);

        uint64_t bf = spin_lock_irqsave(&g_dcache_bucket_locks[b]);
        for (unsigned h = b; h < VFS_DCACHE_HASH_SIZE;
             h += VFS_DCACHE_BUCKET_LOCKS)
            dcache_drop_chain(h);
        spin_unlock_irqrestore(&g_dcache_bucket_locks[b], bf);

        /* vnode_put() must not run under the cache locks, and both are
         * released above, so this is the one point where a reference can be
         * dropped with interrupts back on. */
        dcache_retire_drain();
    }
    spin_unlock_irqrestore(&g_dcache_invalidate_lock, inv);
}
