#ifndef _FS_PAGE_CACHE_H
#define _FS_PAGE_CACHE_H

#include "core/types.h"
#include "core/refcount.h"
#include "fs/vfs.h"
#include "mm/frame.h"

#define PAGE_CACHE_MAX_PAGES   262144
#define PAGE_CACHE_INITIAL_PAGES 2048
#define PAGE_CACHE_CHUNK_PAGES 1024
#define PAGE_CACHE_HASH_BUCKETS 262144
/* Independent per-hash-group spinlocks.  A warm hit only takes this bucket
 * lock (plus an atomic refcount), never the global page-cache lock, so
 * concurrent buffered readers on different buckets do not serialize. */
#define PAGE_CACHE_BUCKET_LOCKS 1024
#define PAGE_CACHE_FAULT_AROUND_PAGES 16
/* Sequential-read prefetch window used by page_cache_readahead_vfile().
 * Deliberately larger than the demand-fault window: compiler inputs are read
 * sequentially, so a 128 KiB burst lets ext4 merge one contiguous block
 * request instead of several 64 KiB round trips. */
#define PAGE_CACHE_READAHEAD_PAGES 32
#define PAGE_CACHE_WRITEBACK_BATCH_PAGES 256

/*
 * FILE_MMAP_PAGE_CACHE_CONTRACT:
 * - page_cache_get() pins a vnode/index page until page_cache_put(). Pinned or
 *   dirty pages are not reclaimable.
 * - Private file mmap faults copy cache contents into an anonymous frame; after
 *   the copy, the PTE no longer pins the page-cache page.
 * - Shared file mmap faults map the canonical page-cache page directly into the
 *   user page table. The mapping holds the pin until unmap, mremap move, or
 *   process teardown. Dirty bits from writable PTEs are synced into the page
 *   cache before fsync/msync/writeback so user writes reach the backing file.
 */

typedef struct page_cache_page {
    vnode_t *vnode;
    uint64_t index;
    refcount_t ref_count;
    int valid;
    int dirty;
    uint64_t dirty_gen;
    uint64_t invalidate_gen;
    int uptodate;
    /* Second-chance reference: set on hit under the bucket lock, cleared by
     * eviction.  Lets eviction approximate LRU without touching a global list
     * on every warm hit. */
    unsigned char accessed;
    mutex_t fill_lock;
    pfn_t pfn;
    void *data;
    struct page_cache_page *prev;
    struct page_cache_page *next;
    struct page_cache_page *hnext;
    struct page_cache_page *mapping_prev;
    struct page_cache_page *mapping_next;
    struct page_cache_page *dirty_prev;
    struct page_cache_page *dirty_next;
    struct page_cache_page *global_dirty_prev;
    struct page_cache_page *global_dirty_next;
} page_cache_page_t;

typedef struct page_cache_stats {
    size_t capacity;
    size_t allocated;
    size_t valid;
    size_t dirty;
    size_t pinned;
    size_t bytes;
} page_cache_stats_t;

typedef int (*page_cache_writepage_t)(vnode_t *vn, uint64_t index,
                                      const void *data, size_t len,
                                      void *ctx);

int  page_cache_init(void);
page_cache_page_t *page_cache_get(vnode_t *vn, uint64_t index, int create);
void page_cache_put(page_cache_page_t *page);
void *page_cache_data(page_cache_page_t *page);
void page_cache_mark_uptodate(page_cache_page_t *page);
void page_cache_mark_dirty(page_cache_page_t *page);
void page_cache_mark_clean(page_cache_page_t *page);
int  page_cache_is_uptodate(page_cache_page_t *page);
int  page_cache_read_vfile(vfile_t *vf, char *buf, size_t count);
int  page_cache_write_vfile(vfile_t *vf, const char *buf, size_t count);
int  page_cache_fill_vfile_page(vfile_t *vf, page_cache_page_t *page);
int  page_cache_fill_vfile_pages(vfile_t *vf, page_cache_page_t **pages,
                                 size_t count);
pfn_t page_cache_pfn(page_cache_page_t *page);
int  page_cache_writeback_vnode(vnode_t *vn, page_cache_writepage_t writepage,
                                void *ctx);
int  page_cache_writeback_all(page_cache_writepage_t writepage, void *ctx);
void page_cache_update_after_write(vnode_t *vn, uint64_t start,
                                   uint64_t old_size, const void *data,
                                   size_t len);
void page_cache_invalidate(vnode_t *vn);
/* Discard unpinned cached contents without writing dirty data.  Callers may
 * use this only after the final directory link has been removed, when the
 * bytes can no longer become persistent filesystem contents. */
void page_cache_discard_unlinked(vnode_t *vn);
void page_cache_invalidate_range(vnode_t *vn, uint64_t start_byte, uint64_t end_byte);
void page_cache_invalidate_uptodate_range(vnode_t *vn, uint64_t start_byte, uint64_t end_byte);
void page_cache_truncate(vnode_t *vn, uint64_t new_size);
size_t page_cache_drop_clean(void);
void page_cache_get_stats(page_cache_stats_t *stats);

/* Prefetch the page range [start_byte, start_byte + count) for @vf into the
 * page cache.  Existing/uptodate pages are left alone; missing pages are
 * filled.  Returns 0 on success or a negative errno.  Used by readahead(2). */
int page_cache_readahead(vfile_t *vf, uint64_t start_byte, size_t count);

/* Per-file cachestat: reports the number of resident, dirty and total bytes
 * for @vf.  Used by cachestat(2) and statfs accounting. */
void page_cache_file_stats(vfile_t *vf, size_t *resident, size_t *dirty);

/* Same counters restricted to the byte window [off_bytes, off_bytes+len_bytes):
 * pages whose cache index falls inside the window. */
void page_cache_file_range_stats(vfile_t *vf, uint64_t off_bytes,
                                 size_t len_bytes, size_t *resident,
                                 size_t *dirty);

#endif /* _FS_PAGE_CACHE_H */
