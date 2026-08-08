#include "fs/page_cache.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/perf.h"
#include "core/string.h"

static spinlock_t g_page_cache_lock = SPINLOCK_INIT;
static page_cache_page_t *g_pages;
static page_cache_page_t g_lru_head;
static page_cache_page_t g_lru_tail;
static page_cache_page_t *g_hash[PAGE_CACHE_HASH_BUCKETS];
static page_cache_page_t *g_dirty_pages;
static int g_initialized;

static inline void page_cache_account_scan(size_t entries)
{
    a20_perf_count(A20_PERF_PAGE_CACHE_SCAN_CALLS);
    a20_perf_add(A20_PERF_PAGE_CACHE_SCAN_ENTRIES, entries);
}

static unsigned page_cache_hash_key(vnode_t *vn, uint64_t index)
{
    uintptr_t v = (uintptr_t)vn;
    return (unsigned)((v >> 4) ^ index ^ (index >> 32)) &
           (PAGE_CACHE_HASH_BUCKETS - 1);
}

static void lru_remove(page_cache_page_t *page)
{
    page->prev->next = page->next;
    page->next->prev = page->prev;
    page->prev = NULL;
    page->next = NULL;
}

static void lru_insert_front(page_cache_page_t *page)
{
    page->next = g_lru_head.next;
    page->prev = &g_lru_head;
    g_lru_head.next->prev = page;
    g_lru_head.next = page;
}

static void hash_insert(page_cache_page_t *page)
{
    unsigned idx = page_cache_hash_key(page->vnode, page->index);
    page->hnext = g_hash[idx];
    g_hash[idx] = page;
}

static void hash_remove(page_cache_page_t *page)
{
    unsigned idx = page_cache_hash_key(page->vnode, page->index);
    page_cache_page_t **pp = &g_hash[idx];
    size_t visited = 0;
    while (*pp) {
        visited++;
        if (*pp == page) {
            *pp = page->hnext;
            page->hnext = NULL;
            page_cache_account_scan(visited);
            return;
        }
        pp = &(*pp)->hnext;
    }
    page_cache_account_scan(visited);
    page->hnext = NULL;
}

static page_cache_page_t *find_locked(vnode_t *vn, uint64_t index)
{
    size_t visited = 0;
    for (page_cache_page_t *p = g_hash[page_cache_hash_key(vn, index)];
         p; p = p->hnext) {
        visited++;
        if (p->valid && p->vnode == vn && p->index == index) {
            page_cache_account_scan(visited);
            return p;
        }
    }
    page_cache_account_scan(visited);
    return NULL;
}

static void mapping_insert_locked(page_cache_page_t *page)
{
    vnode_t *vn = page->vnode;
    page->mapping_prev = NULL;
    page->mapping_next = vn->cache_pages;
    if (vn->cache_pages)
        vn->cache_pages->mapping_prev = page;
    vn->cache_pages = page;
}

static void mapping_remove_locked(page_cache_page_t *page)
{
    vnode_t *vn = page->vnode;
    if (page->mapping_prev)
        page->mapping_prev->mapping_next = page->mapping_next;
    else if (vn)
        vn->cache_pages = page->mapping_next;
    if (page->mapping_next)
        page->mapping_next->mapping_prev = page->mapping_prev;
    page->mapping_prev = NULL;
    page->mapping_next = NULL;
}

static void dirty_insert_locked(page_cache_page_t *page)
{
    if (page->dirty || !page->vnode)
        return;
    vnode_t *vn = page->vnode;
    page->dirty_prev = NULL;
    page->dirty_next = vn->cache_dirty_pages;
    if (vn->cache_dirty_pages)
        vn->cache_dirty_pages->dirty_prev = page;
    vn->cache_dirty_pages = page;

    page->global_dirty_prev = NULL;
    page->global_dirty_next = g_dirty_pages;
    if (g_dirty_pages)
        g_dirty_pages->global_dirty_prev = page;
    g_dirty_pages = page;
    page->dirty = 1;
}

static void dirty_remove_locked(page_cache_page_t *page)
{
    if (!page->dirty)
        return;
    vnode_t *vn = page->vnode;
    if (page->dirty_prev)
        page->dirty_prev->dirty_next = page->dirty_next;
    else if (vn)
        vn->cache_dirty_pages = page->dirty_next;
    if (page->dirty_next)
        page->dirty_next->dirty_prev = page->dirty_prev;

    if (page->global_dirty_prev)
        page->global_dirty_prev->global_dirty_next = page->global_dirty_next;
    else
        g_dirty_pages = page->global_dirty_next;
    if (page->global_dirty_next)
        page->global_dirty_next->global_dirty_prev = page->global_dirty_prev;

    page->dirty_prev = NULL;
    page->dirty_next = NULL;
    page->global_dirty_prev = NULL;
    page->global_dirty_next = NULL;
    page->dirty = 0;
}

static vnode_t *detach_mapping_deferred_locked(page_cache_page_t *page)
{
    if (!page->valid)
        return NULL;
    dirty_remove_locked(page);
    mapping_remove_locked(page);
    hash_remove(page);
    vnode_t *vn = page->vnode;
    page->vnode = NULL;
    page->index = 0;
    page->valid = 0;
    page->dirty = 0;
    page->dirty_gen = 0;
    page->invalidate_gen++;
    page->uptodate = 0;
    return vn;
}

static page_cache_page_t *evict_locked(vnode_t **deferred_put)
{
    page_cache_page_t *page = g_lru_tail.prev;
    size_t visited = 0;
    while (page != &g_lru_head) {
        visited++;
        if (refcount_read(&page->ref_count) == 0 && !page->dirty &&
            pfn_valid(page->pfn) && pfa.meta[page->pfn].refcount <= 1) {
            if (page->valid)
                *deferred_put = detach_mapping_deferred_locked(page);
            refcount_set(&page->ref_count, 1);
            lru_remove(page);
            lru_insert_front(page);
            page_cache_account_scan(visited);
            return page;
        }
        page = page->prev;
    }
    page_cache_account_scan(visited);
    return NULL;
}

int page_cache_init(void)
{
    if (g_initialized)
        return 0;

    spin_init(&g_page_cache_lock);
    g_pages = kcalloc(PAGE_CACHE_MAX_PAGES, sizeof(page_cache_page_t));
    if (!g_pages)
        return -ENOMEM;

    g_lru_head.prev = NULL;
    g_lru_head.next = &g_lru_tail;
    g_lru_tail.prev = &g_lru_head;
    g_lru_tail.next = NULL;

    for (int i = 0; i < PAGE_CACHE_MAX_PAGES; i++) {
        g_pages[i].pfn = pfa_alloc_page();
        if (g_pages[i].pfn == PFN_NONE)
            return -ENOMEM;
        g_pages[i].data = pfn_to_virt(g_pages[i].pfn);
        refcount_set(&g_pages[i].ref_count, 0);
        mutex_init(&g_pages[i].fill_lock);
        lru_insert_front(&g_pages[i]);
    }
    g_initialized = 1;
    return 0;
}

page_cache_page_t *page_cache_get(vnode_t *vn, uint64_t index, int create)
{
    if (!g_initialized || !vn)
        return NULL;

    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    page_cache_page_t *page = find_locked(vn, index);
    if (page) {
        refcount_inc(&page->ref_count);
        lru_remove(page);
        lru_insert_front(page);
        spin_unlock_irqrestore(&g_page_cache_lock, flags);
        return page;
    }

    if (!create) {
        spin_unlock_irqrestore(&g_page_cache_lock, flags);
        return NULL;
    }

    vnode_t *deferred_put = NULL;
    page = evict_locked(&deferred_put);
    if (!page) {
        spin_unlock_irqrestore(&g_page_cache_lock, flags);
        return NULL;
    }
    page->vnode = vn;
    page->index = index;
    page->valid = 1;
    page->dirty = 0;
    page->dirty_gen = 0;
    page->invalidate_gen++;
    page->uptodate = 0;
    memset(page->data, 0, PAGE_SIZE);
    vnode_get(vn);
    hash_insert(page);
    mapping_insert_locked(page);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    if (deferred_put)
        vnode_put(deferred_put);
    return page;
}

void page_cache_put(page_cache_page_t *page)
{
    if (!page)
        return;
    if (refcount_read(&page->ref_count) > 0)
        refcount_dec_and_test(&page->ref_count);
}

void *page_cache_data(page_cache_page_t *page)
{
    return page ? page->data : NULL;
}

void page_cache_mark_uptodate(page_cache_page_t *page)
{
    if (page)
        __atomic_store_n(&page->uptodate, 1, __ATOMIC_RELEASE);
}

void page_cache_mark_dirty(page_cache_page_t *page)
{
    if (!page)
        return;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    if (page->valid) {
        /* Dirty data is authoritative even after an earlier invalidation. */
        __atomic_store_n(&page->uptodate, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&page->dirty_gen, 1, __ATOMIC_RELEASE);
        dirty_insert_locked(page);
    }
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
}

void page_cache_mark_clean(page_cache_page_t *page)
{
    if (!page)
        return;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    dirty_remove_locked(page);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
}

int page_cache_is_uptodate(page_cache_page_t *page)
{
    if (!page)
        return 0;
    return __atomic_load_n(&page->uptodate, __ATOMIC_ACQUIRE) != 0;
}

static int publish_uptodate(page_cache_page_t *page, uint64_t invalidate_gen)
{
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    int unchanged = page->valid && page->invalidate_gen == invalidate_gen;
    if (unchanged)
        __atomic_store_n(&page->uptodate, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    return unchanged;
}

static uint64_t snapshot_invalidate_gen(page_cache_page_t *page)
{
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    uint64_t invalidate_gen = page->invalidate_gen;
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    return invalidate_gen;
}

int page_cache_fill_vfile_page(vfile_t *vf, page_cache_page_t *page)
{
    if (!vf || !vf->ops || !vf->ops->read || !vf->ops->lseek)
        return -EINVAL;

    mutex_lock(&page->fill_lock);
    if (page_cache_is_uptodate(page)) {
        mutex_unlock(&page->fill_lock);
        return 0;
    }

    uint64_t page_base = page->index * PAGE_SIZE;
    void *data = page_cache_data(page);
    if (!data) {
        mutex_unlock(&page->fill_lock);
        return -ENOMEM;
    }

    uint64_t invalidate_gen;
retry:
    invalidate_gen = snapshot_invalidate_gen(page);
    if (vf->vnode->ops && vf->vnode->ops->readpage) {
        int r = vf->vnode->ops->readpage(vf->vnode, page->index,
                                         data, PAGE_SIZE);
        if (r < 0) {
            mutex_unlock(&page->fill_lock);
            return r;
        }
        if (!publish_uptodate(page, invalidate_gen))
            goto retry;
        mutex_unlock(&page->fill_lock);
        return r;
    }

    size_t saved = vf->offset;

    long seek_r = vf->ops->lseek(vf, (long)page_base, SEEK_SET);
    if (seek_r < 0) {
        mutex_unlock(&page->fill_lock);
        return (int)seek_r;
    }

    int r = vf->ops->read(vf, (char *)data, PAGE_SIZE);
    int restore_r = vf->ops->lseek(vf, (long)saved, SEEK_SET);
    if (restore_r < 0 && r >= 0)
        r = restore_r;
    if (r < 0) {
        mutex_unlock(&page->fill_lock);
        return r;
    }

    if ((size_t)r < PAGE_SIZE)
        memset((char *)data + r, 0, PAGE_SIZE - (size_t)r);
    if (!publish_uptodate(page, invalidate_gen))
        goto retry;
    mutex_unlock(&page->fill_lock);
    return r;
}

pfn_t page_cache_pfn(page_cache_page_t *page)
{
    return page ? page->pfn : PFN_NONE;
}

int page_cache_read_vfile(vfile_t *vf, char *buf, size_t count)
{
    if (!vf || !vf->vnode || !buf)
        return -EINVAL;
    if (!vf->ops || !vf->ops->read || !vf->ops->lseek)
        return -ENOSYS;
    if (vf->vnode->type != VFS_FT_REGULAR)
        return -EINVAL;
    if (count == 0)
        return 0;

    /*
     * Use the cached vnode->size directly.  File size is already updated by
     * write/truncate paths, so the expensive stat() call on every read was
     * a severe performance bottleneck for sequential I/O workloads.
     */
    size_t file_size = vf->vnode->size;
    size_t start = vf->offset;
    if (start >= file_size)
        return 0;
    if (count > file_size - start)
        count = file_size - start;

    size_t done = 0;
    while (done < count) {
        uint64_t pos = start + done;
        uint64_t index = pos / PAGE_SIZE;
        size_t page_off = (size_t)(pos % PAGE_SIZE);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > count - done)
            chunk = count - done;

        page_cache_page_t *page = page_cache_get(vf->vnode, index, 1);
        if (!page)
            break;

        if (!page_cache_is_uptodate(page)) {
            int r = page_cache_fill_vfile_page(vf, page);
            if (r < 0) {
                page_cache_put(page);
                if (done == 0)
                    return r;
                break;
            }
        }

        memcpy(buf + done, (char *)page_cache_data(page) + page_off, chunk);
        page_cache_put(page);
        done += chunk;
    }

    if (done > 0) {
        long seek_r = vf->ops->lseek(vf, (long)(start + done), SEEK_SET);
        if (seek_r < 0)
            vf->offset = start + done;
    }
    return (int)done;
}

static page_cache_page_t *find_dirty_locked(vnode_t *vn)
{
    page_cache_page_t *page = vn ? vn->cache_dirty_pages : g_dirty_pages;
    page_cache_account_scan(page ? 1 : 0);
    if (!page)
        return NULL;
    refcount_inc(&page->ref_count);
    return page;
}

static int page_cache_writeback_common(vnode_t *vn,
                                       page_cache_writepage_t writepage,
                                       void *ctx)
{
    if (!g_initialized)
        return 0;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
        page_cache_page_t *page = find_dirty_locked(vn);
        if (!page) {
            spin_unlock_irqrestore(&g_page_cache_lock, flags);
            return 0;
        }

        vnode_t *page_vn = page->vnode;
        uint64_t index = page->index;
        void *data = page->data;
        uint64_t dirty_gen = __atomic_load_n(&page->dirty_gen,
                                             __ATOMIC_ACQUIRE);
        spin_unlock_irqrestore(&g_page_cache_lock, flags);

        page_cache_writepage_t cb = writepage;
        int r = 0;
        if (cb) {
            r = cb(page_vn, index, data, PAGE_SIZE, ctx);
        } else if (page_vn && page_vn->ops && page_vn->ops->writepage) {
            (void)ctx;
            r = page_vn->ops->writepage(page_vn, index, data, PAGE_SIZE);
        } else {
            page_cache_put(page);
            return -ENOSYS;
        }

        flags = spin_lock_irqsave(&g_page_cache_lock);
        /* Only clear dirty if nobody re-dirtied this page during I/O. */
        if (r >= 0 && page->valid && page->vnode == page_vn &&
            page->index == index &&
            __atomic_load_n(&page->dirty_gen, __ATOMIC_ACQUIRE) == dirty_gen)
            dirty_remove_locked(page);
        spin_unlock_irqrestore(&g_page_cache_lock, flags);

        page_cache_put(page);
        if (r < 0)
            return r;
    }
}

int page_cache_writeback_vnode(vnode_t *vn, page_cache_writepage_t writepage,
                               void *ctx)
{
    if (!vn)
        return -EINVAL;
    return page_cache_writeback_common(vn, writepage, ctx);
}

int page_cache_writeback_all(page_cache_writepage_t writepage, void *ctx)
{
    return page_cache_writeback_common(NULL, writepage, ctx);
}

void page_cache_invalidate(vnode_t *vn)
{
    if (!g_initialized || !vn)
        return;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t detached = 0;
    size_t visited = 0;
    for (page_cache_page_t *page = vn->cache_pages, *next; page; page = next) {
        visited++;
        next = page->mapping_next;
        if (refcount_read(&page->ref_count) == 0) {
            detach_mapping_deferred_locked(page);
            detached++;
        }
    }
    page_cache_account_scan(visited);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    for (size_t i = 0; i < detached; i++)
        vnode_put(vn);
}

void page_cache_invalidate_range(vnode_t *vn, uint64_t start_byte,
                                 uint64_t end_byte)
{
    if (!g_initialized || !vn || end_byte <= start_byte)
        return;
    uint64_t first_idx = start_byte / PAGE_SIZE;
    uint64_t last_idx  = (end_byte - 1) / PAGE_SIZE;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t detached = 0;
    size_t visited = 0;
    for (page_cache_page_t *page = vn->cache_pages, *next; page; page = next) {
        visited++;
        next = page->mapping_next;
        if (page->index < first_idx || page->index > last_idx)
            continue;
        if (refcount_read(&page->ref_count) == 0) {
            detach_mapping_deferred_locked(page);
            detached++;
        }
    }
    page_cache_account_scan(visited);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    for (size_t i = 0; i < detached; i++)
        vnode_put(vn);
}

void page_cache_invalidate_uptodate_range(vnode_t *vn, uint64_t start_byte,
                                           uint64_t end_byte)
{
    if (!g_initialized || !vn || end_byte <= start_byte)
        return;
    uint64_t first_idx = start_byte / PAGE_SIZE;
    uint64_t last_idx  = (end_byte - 1) / PAGE_SIZE;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t detached = 0;
    size_t visited = 0;
    for (page_cache_page_t *page = vn->cache_pages, *next; page; page = next) {
        visited++;
        next = page->mapping_next;
        if (page->index < first_idx || page->index > last_idx)
            continue;
        if (refcount_read(&page->ref_count) == 0) {
            detach_mapping_deferred_locked(page);
            detached++;
        } else {
            page->invalidate_gen++;
            __atomic_store_n(&page->uptodate, 0, __ATOMIC_RELEASE);
        }
    }
    page_cache_account_scan(visited);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    for (size_t i = 0; i < detached; i++)
        vnode_put(vn);
}

void page_cache_truncate(vnode_t *vn, uint64_t new_size)
{
    if (!g_initialized || !vn)
        return;
    uint64_t eof_index = new_size / PAGE_SIZE;
    size_t eof_offset = new_size % PAGE_SIZE;
    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    size_t detached = 0;
    size_t visited = 0;
    for (page_cache_page_t *page = vn->cache_pages, *next; page; page = next) {
        visited++;
        next = page->mapping_next;
        if (page->index < eof_index)
            continue;
        int partial_eof_page = eof_offset && page->index == eof_index;
        if (refcount_read(&page->ref_count) == 0) {
            detach_mapping_deferred_locked(page);
            detached++;
        } else if (partial_eof_page) {
            memset((char *)page->data + eof_offset, 0,
                   PAGE_SIZE - eof_offset);
            page->invalidate_gen++;
        } else {
            /*
             * Page is pinned by a concurrent reader/writer.  We cannot
             * detach it, but we MUST invalidate its content so that if
             * the file is later extended, the stale old data is never
             * returned.  Zero the data and mark non-uptodate/non-dirty.
             */
            memset(page->data, 0, PAGE_SIZE);
            page->invalidate_gen++;
            __atomic_store_n(&page->uptodate, 0, __ATOMIC_RELEASE);
            dirty_remove_locked(page);
        }
    }
    page_cache_account_scan(visited);
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
    for (size_t i = 0; i < detached; i++)
        vnode_put(vn);
}

size_t page_cache_drop_clean(void)
{
    if (!g_initialized)
        return 0;

    size_t dropped = 0;
    size_t visited = 0;
    int cursor = 0;
    while (cursor < PAGE_CACHE_MAX_PAGES) {
        vnode_t *held[64];
        int held_count = 0;
        uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
        while (cursor < PAGE_CACHE_MAX_PAGES && held_count < 64) {
            page_cache_page_t *page = &g_pages[cursor++];
            visited++;
            if (!page->valid || page->dirty ||
                refcount_read(&page->ref_count) != 0 ||
                !pfn_valid(page->pfn) ||
                pfa.meta[page->pfn].refcount > 1)
                continue;
            vnode_t *vn = detach_mapping_deferred_locked(page);
            if (vn)
                held[held_count++] = vn;
            dropped++;
        }
        spin_unlock_irqrestore(&g_page_cache_lock, flags);

        /* A final vnode_put() may enter the filesystem and acquire sleeping
         * locks.  Transfer the cache references out of the spinlocked region
         * before releasing them. */
        for (int i = 0; i < held_count; i++)
            vnode_put(held[i]);
    }
    page_cache_account_scan(visited);
    return dropped;
}

void page_cache_get_stats(page_cache_stats_t *stats)
{
    if (!stats)
        return;
    memset(stats, 0, sizeof(*stats));
    stats->capacity = PAGE_CACHE_MAX_PAGES;
    stats->bytes = (size_t)PAGE_CACHE_MAX_PAGES * PAGE_SIZE;
    if (!g_initialized)
        return;

    uint64_t flags = spin_lock_irqsave(&g_page_cache_lock);
    for (int i = 0; i < PAGE_CACHE_MAX_PAGES; i++) {
        page_cache_page_t *page = &g_pages[i];
        if (!page->valid)
            continue;
        stats->valid++;
        if (page->dirty)
            stats->dirty++;
        if (refcount_read(&page->ref_count) > 0)
            stats->pinned++;
    }
    spin_unlock_irqrestore(&g_page_cache_lock, flags);
}
