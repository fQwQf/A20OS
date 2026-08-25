/*
 * fscompat/fs/page_cache.h — 用户态 FS 宿主的页缓存桩。
 * ext4 关闭路径调用 page_cache_discard_unlinked；宿主无内核页缓存，为 no-op。
 */
#ifndef _FS_PAGE_CACHE_H
#define _FS_PAGE_CACHE_H

#include "fs/vfs.h"

void page_cache_discard_unlinked(vnode_t *vn);

#endif /* _FS_PAGE_CACHE_H */
