#ifndef _RAMFS_H
#define _RAMFS_H

#include "fs/vfs.h"

vnode_t *ramfs_mount(mount_t *mnt);
vnode_t *ramfs_mount_empty(mount_t *mnt);
vfile_t *ramfs_open_vnode(vnode_t *vn, int flags);

#endif /* _RAMFS_H */
