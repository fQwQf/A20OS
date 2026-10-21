#ifndef _FS_VFS_MOUNT_STATE_H
#define _FS_VFS_MOUNT_STATE_H

#include "core/types.h"

/*
 * VFS mount-table orchestration state.
 *
 * The umount(2) path matching and busy-rule admission, and the
 * move_mount(2) validation and commit sequence, mount-tree publication,
 * unmount phase ordering, and namespace mount-pin teardown ordering are
 * owned by kernel/fs/vfs/mount_state.rs. The adapter keeps C-layout mount
 * tables, per-mount counters, filesystem-specific destructors and primitive
 * VFS side effects, and supplies the per-mount facts.
 *
 * Keep the field order in sync with the Rust mirror.
 */

typedef struct vfs_mount_cand {
    const char *path;
    uint32_t root_users;
    uint32_t has_child;
    uint32_t cwd_users;
    uint32_t is_ns_root;
} vfs_mount_cand_t;

typedef struct vfs_umount_plan {
    int index;              /* matched candidate, or -1 */
    uint32_t detach_children;
    uint32_t invalidate_all;
} vfs_umount_plan_t;

typedef struct vfs_mount_match {
    const char *path;
    uint32_t ready;
    uint32_t teardown;
    uint32_t detached;
} vfs_mount_match_t;

int a20_vfs_umount_plan(const char *query, int flags, int check_busy,
                        const vfs_mount_cand_t *cands, size_t n,
                        vfs_umount_plan_t *out);

/* Longest-prefix visible-mount match over a snapshot; returns the best index
 * or -1.  Rust owns the visibility/propagation decision. */
int a20_vfs_find_mount(const char *query, int include_detached,
                       int external_root, const vfs_mount_match_t *cands,
                       size_t n);

/* Offset to add to `path` to strip the matched mount path prefix. */
size_t a20_vfs_strip_prefix(const char *path, const char *mnt_path);

/* Compute the parent path of a mount path; returns 1 on success, 0 for the
 * root / no separator. */
int a20_vfs_mount_parent_path(const char *path, char *out, size_t cap);
int a20_vfs_move_mount_check(const char *from, const char *to, int found,
                             const char *from_path, int from_detached,
                             int target_covered, int *out_noop);

/*
 * Mount-tree side effects.  The adapter exposes the tree links and the vnode
 * refcount primitives; Rust owns the unlink/attach/detach ordering so the
 * parent/sibling/mountpoint surgery and the vnode reference transfer are not
 * duplicated at every call site.
 */
struct vfs_mount_tree_ops {
    void  *(*parent)(void *mnt);
    void  *(*child)(void *mnt);
    void  *(*sibling)(void *mnt);
    void  *(*mp)(void *mnt);
    void   (*set_parent)(void *mnt, void *parent);
    void   (*set_child)(void *mnt, void *child);
    void   (*set_sibling)(void *mnt, void *sib);
    void   (*set_mp)(void *mnt, void *mp);
    void   (*set_detached)(void *mnt);
    void   (*vnode_get)(void *vnode);
    void   (*vnode_put)(void *vnode);
};

/* C exposes object layout and path-resolution services; Rust owns mount-tree
 * publication order and the parent-path decision. */
struct vfs_mount_link_ops {
    const char *(*path)(void *mnt);
    void *(*find_mount)(const char *path);
    void *(*resolve)(const char *path);
    void *(*vnode_mount)(void *vnode);
    void (*mark_ready)(void *mnt);
};

void a20_vfs_mount_link_tree(const struct vfs_mount_tree_ops *tree,
                             const struct vfs_mount_link_ops *ops,
                             void *mnt);

struct vfs_move_mount_ops {
    uint64_t (*lock)(void);
    void (*unlock)(uint64_t flags);
    int (*is_present)(void *mnt);
    int (*is_teardown)(void *mnt);
    void (*set_path)(void *mnt, const char *path);
    void (*invalidate_dcache)(void);
};

int a20_vfs_move_mount_commit(const struct vfs_mount_tree_ops *tree,
                              const struct vfs_move_mount_ops *ops,
                              void *mnt, void *parent, void *mountpoint,
                              const char *path);

struct vfs_umount_execute_ops {
    int (*teardown)(void *mnt);
    void (*invalidate_all)(void);
    void (*drop_time_meta)(void *mnt);
    void (*remove)(void *mnt);
    void (*retry_pending)(void);
};

int a20_vfs_umount_execute(const struct vfs_mount_tree_ops *tree,
                           const struct vfs_umount_execute_ops *ops,
                           void *mnt, const struct vfs_umount_plan *plan);
int vfs_mount_execute_umount(const struct vfs_umount_execute_ops *ops,
                             void *mnt, const struct vfs_umount_plan *plan);

struct vfs_mount_namespace_teardown_ops {
    int (*mount_count)(void *ns);
    void *(*mount_at)(void *ns, int index);
    void *(*dead_head)(void *ns);
    void *(*dead_next)(void *mnt);
    void *(*shared_source)(void *mnt);
    int (*fs_teardown)(void *mnt);
    int (*decrement_source_users)(void *source);
    void (*restore_source_users)(void *source);
    void (*clear_shared_source)(void *mnt);
};
int a20_vfs_mount_namespace_teardown(
    const struct vfs_mount_namespace_teardown_ops *ops, void *ns);

struct vfs_mount_remove_ops {
    uint64_t (*lock)(void);
    void (*unlock)(uint64_t flags);
    int (*count)(void *ns);
    void (*set_count)(void *ns, int count);
    void *(*current_ns)(void);
    void *(*at)(void *ns, int index);
    void (*set_at)(void *ns, int index, void *mnt);
    void *(*dead_head)(void *ns);
    void (*set_dead_head)(void *ns, void *mnt);
    void (*set_dead_next)(void *mnt, void *next);
    void (*set_attached)(void *mnt, int attached);
    void (*clear_teardown)(void *mnt);
    void *(*mountpoint)(void *mnt);
    void (*clear_mountpoint)(void *mnt);
    void (*vnode_put)(void *vnode);
    void (*root_put)(void *mnt);
};
int a20_vfs_mount_remove(const struct vfs_mount_tree_ops *tree,
                         const struct vfs_mount_remove_ops *ops, void *mnt);

struct vfs_mount_fs_teardown_ops {
    uint64_t (*lock)(void);
    void (*unlock)(uint64_t flags);
    void *(*shared_source)(void *mnt);
    int (*users)(void *mnt);
    int (*is_running)(void *mnt);
    void (*set_running)(void *mnt, int running);
    int (*kind)(void *mnt);
    void *(*root)(void *mnt);
    int (*unmount_backend)(int kind, void *root);
    void (*set_users_zero)(void *mnt);
    void (*trace)(void *mnt, int phase, int users);
};
int a20_vfs_mount_fs_teardown(const struct vfs_mount_fs_teardown_ops *ops,
                              void *mnt);

struct vfs_mount_dispatch_ops {
    int (*update_existing)(const char *path, int flags);
    int (*mount_cgroup)(const char *dev, const char *path, const char *fstype,
                        int flags, const char *data);
    int (*mount_virtual)(const char *dev, const char *path, const char *fstype,
                         int flags, const char *data);
    int (*mount_ram)(const char *dev, const char *path, const char *fstype,
                     int flags, const char *data);
    int (*mount_block)(int dev_idx, int part_num, const char *path,
                       const char *fstype, int flags, const char *data);
};
int a20_vfs_mount_dispatch(const struct vfs_mount_dispatch_ops *ops,
                           const char *dev, const char *path,
                           const char *fstype, int flags, const char *data);

void a20_vfs_mount_unlink(const struct vfs_mount_tree_ops *ops, void *mnt);
int  a20_vfs_mount_attach(const struct vfs_mount_tree_ops *ops, void *mnt,
                          void *parent, void *mp);
void a20_vfs_mount_detach_root(const struct vfs_mount_tree_ops *ops, void *mnt);

#endif /* _FS_VFS_MOUNT_STATE_H */
