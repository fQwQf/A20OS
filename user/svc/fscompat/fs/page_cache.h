/*
 * fscompat/fs/page_cache.h — a page cache stub for the user-space FS host.
 * The ext4 close path calls page_cache_discard_unlinked; the host has no
 * kernel page cache, so it is a no-op.
 */
#ifndef _FS_PAGE_CACHE_H
#define _FS_PAGE_CACHE_H

#include "fs/vfs.h"

void page_cache_discard_unlinked(vnode_t *vn);

#endif /* _FS_PAGE_CACHE_H */
