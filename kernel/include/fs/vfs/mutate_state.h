#ifndef _FS_VFS_MUTATE_STATE_H
#define _FS_VFS_MUTATE_STATE_H

#include "core/types.h"

/*
 * VFS directory-mutation orchestration engine.
 *
 * The decision order that `vfs_unlink`, `vfs_rmdir` and `vfs_rename_flags`
 * used to run inline -- flag validation, operand resolution through the mount
 * table and the process root, the same-mount rule, the parent type/permission/
 * operation checks, the victim probe with the sticky-bit gate and
 * RENAME_NOREPLACE, and the ordered post-success side effects -- is owned by
 * kernel/fs/vfs/mutate_state.rs.  The adapter supplies the mechanical half
 * (path plumbing, vnode references, the filesystem lookup/unlink/rmdir/rename
 * calls, the permission and sticky checks, and the notification/quota/dcache
 * calls) through VfsMutateOps.
 *
 * Keep the field order in sync with the Rust mirror.
 */

struct vfs_mutate_ops {
    int (*join)(const char *cwd, const char *path, char *out, size_t outsz);
    int (*split_parent_name)(const char *path, char *parent, size_t parent_sz,
                             char *name, size_t name_sz, size_t max_path_len);
    int (*normalize_with_root)(char *path, const char *root, size_t max_path_len);
    int (*is_beneath)(const char *start, const char *resolved);
    void *(*find_mount)(const char *path);
    const char *(*strip_mount_prefix)(const char *path, const void *mnt);
    void *(*mnt_root)(const void *mnt);
    int (*mnt_type)(const void *mnt);
    void *(*lookup_path)(void *mnt, const char *rel);
    int (*lookup_errno)(void);

    void (*vnode_put)(void *vn);
    int (*vn_type)(const void *vn);
    uint64_t (*vn_ino)(const void *vn);
    size_t (*vn_size)(const void *vn);
    void *(*vn_mnt)(const void *vn);

    int (*has_lookup)(const void *dir);
    int (*lookup)(void *dir, const char *name, void **out);
    int (*has_unlink)(const void *dir);
    int (*has_rmdir)(const void *dir);
    int (*has_rename)(const void *dir);
    int (*do_unlink)(void *dir, const char *name);
    int (*do_rmdir)(void *dir, const char *name);
    int (*do_rename)(void *old_dir, const char *old_name, void *new_dir,
                     const char *new_name, uint32_t flags);

    int (*permission)(void *vn, int mask);
    int (*sticky_may_remove)(void *dir, void *victim);

    void (*notify_delete)(void *dir, const char *name, int is_dir);
    void (*notify_rename)(void *old_dir, const char *old_name, void *new_dir,
                          const char *new_name);
    void (*notify_self_delete)(void *vn);
    void (*quota_remove)(void *vn, size_t size);
    void (*dcache_invalidate)(void *dir, const char *name);
    void (*dcache_invalidate_all)(void);

    size_t max_path_len;
    size_t max_name_len;
};

int a20_vfs_unlink(const struct vfs_mutate_ops *ops, const char *cwd, const char *path);
int a20_vfs_rmdir(const struct vfs_mutate_ops *ops, const char *cwd, const char *path);
int a20_vfs_rename_flags(const struct vfs_mutate_ops *ops, const char *cwd,
                         const char *root, const char *old, const char *newpath,
                         uint32_t flags);

#endif /* _FS_VFS_MUTATE_STATE_H */
