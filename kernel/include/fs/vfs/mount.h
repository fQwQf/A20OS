#ifndef _FS_VFS_MOUNT_H
#define _FS_VFS_MOUNT_H

#include "fs/vfs.h"

void     vfs_mount_table_init(void);
int      vfs_mount_count(void);
mount_t *vfs_mount_at(int index);
mount_t *vfs_mount_alloc(void);
void     vfs_mount_remove(mount_t *mnt);
mount_t *vfs_find_mount(const char *path);
const char *vfs_strip_mount_prefix(const char *path, const mount_t *mnt);

#endif /* _FS_VFS_MOUNT_H */
