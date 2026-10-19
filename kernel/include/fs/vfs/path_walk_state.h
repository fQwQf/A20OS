#ifndef _FS_VFS_PATH_WALK_STATE_H
#define _FS_VFS_PATH_WALK_STATE_H

#include "core/types.h"

/*
 * VFS path-resolution walk engine.
 *
 * The walk that `vnode_lookup_path_openat2` used to run inline -- component
 * iteration, `.`/`..` and mount crossing, symlink following with its RESOLVE_*
 * decisions and loop limit, the permission/type checks in their original order,
 * the RESOLVE_NO_XDEV/RESOLVE_BENEATH boundary checks, `resolved_out`
 * bookkeeping and the errno of every failure -- is owned by
 * kernel/fs/vfs/path_walk_state.rs.  The adapter below supplies the mechanical
 * half (vnode references, filesystem `ops->lookup`/`ops->readlink`, the dcache
 * probe/insert, the permission check and the mount-table lookups) through
 * VfsWalkOps, plus the string layer the walk consumes.
 *
 * Keep the field order in sync with the Rust mirror; the host harness asserts
 * the two layouts agree.
 */

struct vfs_walk_ops {
    void (*vnode_get)(void *vn);
    void (*vnode_put)(void *vn);
    int (*vn_type)(const void *vn);
    uint64_t (*vn_ino)(const void *vn);
    uint32_t (*vn_link_flags)(const void *vn);
    void *(*vn_parent)(const void *vn);
    void *(*vn_mnt)(const void *vn);
    /* dir->ops && dir->ops->lookup. */
    int (*has_lookup)(const void *dir);
    /* dir->ops->lookup(dir, name, &out); 0 with *out set on success. */
    int (*lookup)(void *dir, const char *name, void **out);
    /* vn->ops && vn->ops->readlink. */
    int (*has_readlink)(const void *vn);
    /* vn->ops->readlink(vn, buf, sz); byte count or negative errno. */
    int (*readlink)(void *vn, char *buf, size_t sz);
    int (*permission)(void *vn, int mask);
    /* Referenced vnode, or NULL. */
    void *(*dcache_lookup)(void *dir, const char *name);
    void (*dcache_insert)(void *dir, const char *name, void *vn);
    void *(*find_mount)(const char *path);
    void *(*mount_parent)(void *mnt);
    /* Pointer into `path` (valid for the call), or `path` itself. */
    const char *(*strip_mount_prefix)(const char *path, const void *mnt);
    void *(*mnt_root)(const void *mnt);
    const char *(*mnt_path)(const void *mnt);
    /* vnode_lookup_path(mnt, rel): referenced vnode or NULL.  Re-enters the
     * walk, which is what makes the mount-crossing `..` case work. */
    void *(*lookup_path)(void *mnt, const char *rel);
    int (*lookup_errno)(void);
    /* The Rust-owned string layer. */
    int (*normalize_with_root)(char *path, const char *root, size_t max_path_len);
    int (*is_beneath)(const char *start, const char *resolved);
    size_t max_path_len;
    size_t max_name_len;
    int max_symlinks;
};

/* Resolve `path` to a referenced vnode.  On success the caller owns the
 * returned reference; on failure NULL is returned and *lookup_err is set. */
void *a20_vfs_walk_path(const struct vfs_walk_ops *ops, const char *path,
                        const char *start, const char *root_path, uint64_t resolve,
                        char *resolved_out, size_t resolved_out_sz, int *lookup_err);

#endif /* _FS_VFS_PATH_WALK_STATE_H */
