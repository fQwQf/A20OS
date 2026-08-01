#ifndef _FS_NTFS_H
#define _FS_NTFS_H

#include "fs/vfs.h"

struct bcache;

/*
 * NTFS read/write filesystem
 * Mounts a block device cache and exposes NTFS directories, files and the
 * ability to create, write, truncate and unlink regular files.
 */
vnode_t *ntfs_mount(struct bcache *bc);
void     ntfs_unmount(vnode_t *root);

#endif /* _FS_NTFS_H */
