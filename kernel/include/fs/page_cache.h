#ifndef _FS_PAGE_CACHE_H
#define _FS_PAGE_CACHE_H

#include "core/types.h"
#include "core/refcount.h"
#include "fs/vfs.h"
#include "mm/frame.h"

#define PAGE_CACHE_MAX_PAGES   2048
#define PAGE_CACHE_HASH_BUCKETS 8192

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
    int uptodate;
    pfn_t pfn;
    void *data;
    struct page_cache_page *prev;
    struct page_cache_page *next;
    struct page_cache_page *hnext;
} page_cache_page_t;

typedef struct page_cache_stats {
    size_t capacity;
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
int  page_cache_fill_vfile_page(vfile_t *vf, page_cache_page_t *page);
pfn_t page_cache_pfn(page_cache_page_t *page);
int  page_cache_writeback_vnode(vnode_t *vn, page_cache_writepage_t writepage,
                                void *ctx);
int  page_cache_writeback_all(page_cache_writepage_t writepage, void *ctx);
void page_cache_invalidate(vnode_t *vn);
void page_cache_invalidate_range(vnode_t *vn, uint64_t start_byte, uint64_t end_byte);
void page_cache_invalidate_uptodate_range(vnode_t *vn, uint64_t start_byte, uint64_t end_byte);
void page_cache_truncate(vnode_t *vn, uint64_t new_size);
void page_cache_get_stats(page_cache_stats_t *stats);

#endif /* _FS_PAGE_CACHE_H */
