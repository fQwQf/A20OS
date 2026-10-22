#ifndef _FS_VFS_MOUNT_H
#define _FS_VFS_MOUNT_H

#include "fs/vfs.h"

void     vfs_mount_table_init(void);
int      vfs_mount_count(void);
mount_t *vfs_mount_at(int index);
mount_t *vfs_mount_alloc(void);
void     vfs_mount_remove(mount_t *mnt);
mount_t *vfs_find_mount(const char *path);
/* Like vfs_find_mount() but also matches mounts pivot_root has detached, so
 * the umount path can still name the old root. */
mount_t *vfs_find_mount_including_detached(const char *path);
mount_t *vfs_mount_parent(mount_t *mnt);
const char *vfs_strip_mount_prefix(const char *path, const mount_t *mnt);

/* ---- mount tree ------------------------------------------------------ */
/* Attach @mnt under @parent covering @mp (NULL for a namespace root).  The
 * mount takes its own reference on @mp and releases the previous one. */
int      vfs_mount_attach(mount_t *mnt, mount_t *parent, struct vnode *mp);
/* Position a freshly configured mount in the tree (parent + mountpoint),
 * deriving both from mnt->path.  Idempotent. */
void     vfs_mount_link_tree(mount_t *mnt);
mount_t *vfs_mount_first_child(mount_t *mnt);
mount_t *vfs_mount_next_sibling(mount_t *mnt);
bool     vfs_mount_is_ancestor_of(mount_t *ancestor, mount_t *mnt);

/* Process-root pinning.  root_users counts the namespace table's reference
 * plus one per process rooted in this mount, so "is somebody's root?" is
 * vfs_mount_root_users(mnt) > 1. */
void     vfs_mount_root_get(mount_t *mnt);
void     vfs_mount_root_put(mount_t *mnt);
int      vfs_mount_root_users(mount_t *mnt);
/* Same accounting for the per-process current working directory. */
void     vfs_mount_cwd_get(mount_t *mnt);
void     vfs_mount_cwd_put(mount_t *mnt);
int      vfs_mount_cwd_users(mount_t *mnt);
void     vfs_mount_vfile_get(mount_t *mnt);
void     vfs_mount_vfile_put(mount_t *mnt);
/* Cut @mnt out of the tree bypassing the busy rules (pivot_root).  The
 * mount stays listed in mountinfo but no path resolves through it. */
void     vfs_mount_detach_root(mount_t *mnt);

/* Mount identity for /proc/self/mountinfo and statmount(2). */
uint32_t vfs_mount_id_for(const char *path);
uint32_t vfs_mount_parent_id(mount_t *mnt);

/* move_mount(2) support: re-parent the mount whose path matches @from onto
 * the mount covering @to.  Returns 0 on success or a negative errno. */
int      vfs_move_mount(const char *from, const char *to);

/* fsmount(2) without a target: build the filesystem but leave it out of the
 * tree, so no path resolves through it until vfs_mount_attach_path() gives it
 * a place.  A detached mount is invisible to path lookup and to mountinfo.
 * On success *out_mnt is the caller's reference; the caller must either
 * attach it or destroy it with vfs_mount_fs_teardown()+vfs_mount_remove(). */
int      vfs_mount_detached(const char *dev, const char *fstype, int flags,
                            const char *data, mount_t **out_mnt);

/* Attach a mount created by vfs_mount_detached() at the absolute @path.  The
 * mount must still be detached.  Returns 0 or a negative errno. */
int      vfs_mount_attach_path(mount_t *mnt, const char *path);

/* umount2(2) flags.  The values are the Linux MNT_* bits so the syscall can
 * forward its argument unchanged. */
#define VFS_UMOUNT_FORCE  0x1
#define VFS_UMOUNT_DETACH 0x2
#define VFS_UMOUNT_EXPIRE 0x4

int      vfs_umount_flags(const char *path, int flags);

/* Internal mount_t.flags bit: the entry was duplicated by a mount-namespace
 * copy (unshare/clone CLONE_NEWNS) and shares its root vnode and fs_data
 * with the same mount in another namespace.  The flag tracks that sharing;
 * mount_t.ns_users is the authority on when the filesystem unmount
 * destructor may run. */
#define VFS_MOUNT_NS_SHARED 0x40000000

/* Drop one namespace's hold on a mount entry. Source unmount is busy while
 * copied namespaces still hold it; copied-entry teardown drops its explicit
 * shared_source reference. */
struct mnt_namespace;
int      vfs_mount_fs_teardown(mount_t *mnt);
int      vfs_mount_namespace_teardown(struct mnt_namespace *ns);

#endif /* _FS_VFS_MOUNT_H */
