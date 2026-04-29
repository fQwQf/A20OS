#ifndef _FS_VFS_DCACHE_H
#define _FS_VFS_DCACHE_H

#include "fs/vfs.h"

vnode_t *vfs_dcache_lookup(vnode_t *dir, const char *name);
void     vfs_dcache_insert(vnode_t *dir, const char *name, vnode_t *vn);
void     vfs_dcache_invalidate_all(void);

#endif /* _FS_VFS_DCACHE_H */
