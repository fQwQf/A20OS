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

/* Internal mount_t.flags bit: the entry was duplicated by a mount-namespace
 * copy (unshare/clone CLONE_NEWNS) and shares its root vnode and fs_data
 * with the same mount in another namespace.  The flag tracks that sharing;
 * mount_t.ns_users is the authority on when the filesystem unmount
 * destructor may run. */
#define VFS_MOUNT_NS_SHARED 0x40000000

/* Drop one namespace's hold on a mount entry, running the filesystem
 * teardown only when the last holder releases it. */
struct mnt_namespace;
void     vfs_mount_fs_teardown(mount_t *mnt);
void     vfs_mount_namespace_teardown(struct mnt_namespace *ns);

#endif /* _FS_VFS_MOUNT_H */
