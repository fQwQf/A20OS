#include "fs/vfs/dcache.h"
#include "core/lock.h"
#include "core/lock_counters.h"
#include "core/string.h"

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
    uint32_t h = (uint32_t)(uintptr_t)mnt ^ (uint32_t)ino;
    for (const char *p = name; *p; p++)
        h = h * 31 + (uint8_t)*p;
    return h & VFS_DCACHE_HASH_MASK;
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
    vfs_dcache_entry_t *e = &g_dcache[slot];
    if (e->hash_prev >= 0)
        g_dcache[e->hash_prev].hash_next = e->hash_next;
    else {
        g_dcache_hash[h] = e->hash_next;
    }
    if (e->hash_next >= 0)
        g_dcache[e->hash_next].hash_prev = e->hash_prev;
    e->hash_next = -1;
    e->hash_prev = -1;
}

static void dcache_link_hash(int slot, uint32_t h)
{
    g_dcache[slot].hash_next = g_dcache_hash[h];
    g_dcache[slot].hash_prev = -1;
    if (g_dcache_hash[h] >= 0)
        g_dcache[g_dcache_hash[h]].hash_prev = slot;
    g_dcache_hash[h] = slot;
}

static void dcache_init_locked(void)
{
    if (g_dcache_initialized)
        return;
    for (int i = 0; i < VFS_DCACHE_MAX; i++) {
        g_dcache[i].hash_next = -1;
        g_dcache[i].hash_prev = -1;
        g_dcache[i].free_next = i + 1;
        g_dcache[i].lru_next = -1;
        g_dcache[i].lru_prev = -1;
        g_dcache[i].used = 0;
        g_dcache[i].accessed = 0;
    }
    g_dcache[VFS_DCACHE_MAX - 1].free_next = -1;
    for (int i = 0; i < (int)VFS_DCACHE_HASH_SIZE; i++)
        g_dcache_hash[i] = -1;
    g_dcache_free_list = 0;
    g_dcache_free_count = VFS_DCACHE_MAX;
    g_dcache_lru_head = -1;
    g_dcache_lru_tail = -1;
    g_dcache_initialized = 1;
    lock_counters_register(&g_dcache_lock, "dcache");
}

static void dcache_lru_remove(int slot)
{
    vfs_dcache_entry_t *e = &g_dcache[slot];
    if (e->lru_prev >= 0)
        g_dcache[e->lru_prev].lru_next = e->lru_next;
    else
        g_dcache_lru_head = e->lru_next;
    if (e->lru_next >= 0)
        g_dcache[e->lru_next].lru_prev = e->lru_prev;
    else
        g_dcache_lru_tail = e->lru_prev;
    e->lru_next = -1;
    e->lru_prev = -1;
}

static void dcache_lru_insert_front(int slot)
{
    vfs_dcache_entry_t *e = &g_dcache[slot];
    e->lru_prev = -1;
    e->lru_next = g_dcache_lru_head;
    if (g_dcache_lru_head >= 0)
        g_dcache[g_dcache_lru_head].lru_prev = slot;
    else
        g_dcache_lru_tail = slot;
    g_dcache_lru_head = slot;
}

static void dcache_lru_touch(int slot)
{
    if (g_dcache_lru_head == slot)
        return;
    dcache_lru_remove(slot);
    dcache_lru_insert_front(slot);
}

/* Caller holds g_dcache_lock.  Free-list path needs no bucket lock; the
 * second-chance LRU path takes the evicted slot's bucket lock so the
 * vnode_get() in a concurrent lookup is atomic against slot reuse. */
static int dcache_alloc_slot(vnode_t **old_vn)
{
    if (g_dcache_free_count > 0) {
        int slot = g_dcache_free_list;
        if (slot < 0 || slot >= VFS_DCACHE_MAX)
            return -1;
        g_dcache_free_list = g_dcache[slot].free_next;
        g_dcache[slot].free_next = -1;
        g_dcache_free_count--;
        return slot;
    }

    for (;;) {
        int slot = g_dcache_lru_tail;
        if (slot < 0 || slot >= VFS_DCACHE_MAX)
            return -1;
        vfs_dcache_entry_t *e = &g_dcache[slot];
        if (!e->used) {
            dcache_lru_remove(slot);
            return slot;
        }
        uint32_t h = dcache_hash_key(e->mnt, e->parent_ino, e->name);
        uint64_t bf = dcache_bucket_lock_irqsave(h);
        if (e->used && e->accessed) {
            e->accessed = 0;
            dcache_bucket_unlock_irqrestore(h, bf);
            dcache_lru_touch(slot);
            continue;
        }
        *old_vn = e->vn;
        dcache_unlink_hash(slot, h);
        dcache_lru_remove(slot);
        dcache_bucket_unlock_irqrestore(h, bf);
        return slot;
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
    for (int i = g_dcache_hash[h]; i >= 0; i = g_dcache[i].hash_next) {
        vfs_dcache_entry_t *e = &g_dcache[i];
        if (e->used && e->mnt == dir->mnt && e->parent_ino == dir->ino &&
            strcmp(e->name, name) == 0 && vnode_get_unless_zero(e->vn)) {
            e->accessed = 1;
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
            e->free_next = g_dcache_free_list;
            g_dcache_free_list = i;
            g_dcache_free_count++;
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

static void dcache_retire_push(vnode_t *vn)
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
    for (int i = g_dcache_hash[h]; i >= 0; ) {
        vfs_dcache_entry_t *e = &g_dcache[i];
        int next = e->hash_next;
        if (e->used) {
            if (e->vn)
                dcache_retire_push(e->vn);
            e->used = 0;
            e->vn = NULL;
            e->accessed = 0;
            e->hash_next = -1;
            e->hash_prev = -1;
        }
        i = next;
    }
    g_dcache_hash[h] = -1;
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
