#ifndef _FS_VFS_MOUNT_H
#define _FS_VFS_MOUNT_H

#include "fs/vfs.h"

void     vfs_mount_table_init(void);
int      vfs_mount_count(void);
mount_t *vfs_mount_at(int index);
mount_t *vfs_mount_alloc(void);
void     vfs_mount_remove(mount_t *mnt);
mount_t *vfs_find_mount(const char *path);
mount_t *vfs_mount_parent(mount_t *mnt);
const char *vfs_strip_mount_prefix(const char *path, const mount_t *mnt);

/* move_mount(2) support: repoint the mount whose path matches @from to
 * @to.  Returns 0 on success or a negative errno. */
int      vfs_move_mount(const char *from, const char *to);

#endif /* _FS_VFS_MOUNT_H */
