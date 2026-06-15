#ifndef _RAMFS_H
#define _RAMFS_H

#include "fs/vfs.h"

vnode_t *ramfs_mount(mount_t *mnt);
vnode_t *ramfs_mount_empty(mount_t *mnt);
int      ramfs_populate_overlay(void);

#endif /* _RAMFS_H */
