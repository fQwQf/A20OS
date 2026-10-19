/*
 * A20OS — VFS path resolution
 *
 * This file was mechanically extracted from vfs.c. It resolves paths to vnodes.
 */
#include "fs/vfs.h"
#include "fs/vfs/dcache.h"
#include "fs/vfs/path.h"
#include "fs/vfs/stat_perm.h"
#include "fs/vfs/mount.h"
#include "fs/vfs/path_walk_state.h"
#include "fs/file.h"
#include "proc/proc.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/klog.h"


/* ============================================================
 * VFS path resolution → vnode
 * ============================================================ */




/* ---------------------------------------------------------------------------
 * Rust-owned walk (VFS_PATH_WALK_IMPL=rust)
 *
 * The engine owns the traversal and every decision in it; these callbacks are
 * the mechanical half -- vnode references, the filesystem lookup/readlink
 * calls, the dcache probe/insert, the permission check and the mount-table
 * lookups.  The string layer the walk consumes is the Rust-owned one in
 * path.c, so the walk and the layer it reads share an implementation.
 * ------------------------------------------------------------------------- */

static void walk_vnode_get(void *vn) { vnode_get((vnode_t *)vn); }
static void walk_vnode_put(void *vn) { vnode_put((vnode_t *)vn); }
static int walk_vn_type(const void *vn) { return ((const vnode_t *)vn)->type; }
static uint64_t walk_vn_ino(const void *vn) { return ((const vnode_t *)vn)->ino; }
static uint32_t walk_vn_link_flags(const void *vn) { return ((const vnode_t *)vn)->link_flags; }
static void *walk_vn_parent(const void *vn) { return ((const vnode_t *)vn)->parent; }
static void *walk_vn_mnt(const void *vn) { return ((const vnode_t *)vn)->mnt; }
static int walk_has_lookup(const void *dir)
{
    const vnode_t *d = (const vnode_t *)dir;
    return d->ops && d->ops->lookup ? 1 : 0;
}
static int walk_lookup(void *dir, const char *name, void **out)
{
    vnode_t *d = (vnode_t *)dir;
    vnode_t *n = NULL;
    int r = d->ops->lookup(d, name, &n);
    *out = n;
    return r;
}
static int walk_has_readlink(const void *vn)
{
    const vnode_t *v = (const vnode_t *)vn;
    return v->ops && v->ops->readlink ? 1 : 0;
}
static int walk_readlink(void *vn, char *buf, size_t sz)
{
    vnode_t *v = (vnode_t *)vn;
    return v->ops->readlink(v, buf, sz);
}
static int walk_permission(void *vn, int mask) { return vfs_vnode_permission((vnode_t *)vn, mask); }
static void *walk_dcache_lookup(void *dir, const char *name)
{
    return vfs_dcache_lookup((vnode_t *)dir, name);
}
static void walk_dcache_insert(void *dir, const char *name, void *vn)
{
    vfs_dcache_insert((vnode_t *)dir, name, (vnode_t *)vn);
}
static void *walk_find_mount(const char *path) { return vfs_find_mount(path); }
static void *walk_mount_parent(void *mnt) { return vfs_mount_parent((mount_t *)mnt); }
static const char *walk_strip_mount_prefix(const char *path, const void *mnt)
{
    return vfs_strip_mount_prefix(path, (const mount_t *)mnt);
}
static void *walk_mnt_root(const void *mnt) { return ((const mount_t *)mnt)->root; }
static const char *walk_mnt_path(const void *mnt) { return ((const mount_t *)mnt)->path; }
static void *walk_lookup_path(void *mnt, const char *rel)
{
    return vnode_lookup_path((mount_t *)mnt, rel);
}
static int walk_lookup_errno(void) { return vfs_lookup_errno(); }
static int walk_normalize_with_root(char *path, const char *root, size_t max_path_len)
{
    (void)max_path_len; /* path.c bounds with its own MAX_PATH_LEN */
    return vfs_path_normalize_absolute_with_root(path, root);
}
static int walk_is_beneath(const char *start, const char *resolved)
{
    return path_is_beneath(start, resolved);
}

static const struct vfs_walk_ops g_walk_ops = {
    .vnode_get = walk_vnode_get,
    .vnode_put = walk_vnode_put,
    .vn_type = walk_vn_type,
    .vn_ino = walk_vn_ino,
    .vn_link_flags = walk_vn_link_flags,
    .vn_parent = walk_vn_parent,
    .vn_mnt = walk_vn_mnt,
    .has_lookup = walk_has_lookup,
    .lookup = walk_lookup,
    .has_readlink = walk_has_readlink,
    .readlink = walk_readlink,
    .permission = walk_permission,
    .dcache_lookup = walk_dcache_lookup,
    .dcache_insert = walk_dcache_insert,
    .find_mount = walk_find_mount,
    .mount_parent = walk_mount_parent,
    .strip_mount_prefix = walk_strip_mount_prefix,
    .mnt_root = walk_mnt_root,
    .mnt_path = walk_mnt_path,
    .lookup_path = walk_lookup_path,
    .lookup_errno = walk_lookup_errno,
    .normalize_with_root = walk_normalize_with_root,
    .is_beneath = walk_is_beneath,
    .max_path_len = MAX_PATH_LEN,
    .max_name_len = MAX_NAME_LEN,
    .max_symlinks = MAX_SYMLINKS,
};


/* Resolve an absolute path within a vnode tree, honouring openat2 resolve
 * flags.  On success returns a referenced vnode and writes the fully-resolved
 * absolute path to resolved_out.  On failure returns NULL and sets *lookup_err.
 */
vnode_t *vnode_lookup_path_openat2(const char *path,
                                   const char *start,
                                   const char *root_path,
                                   uint64_t resolve,
                                   char *resolved_out,
                                   size_t resolved_out_sz,
                                   int *lookup_err) {
    return (vnode_t *)a20_vfs_walk_path(&g_walk_ops, path, start, root_path, resolve,
                                        resolved_out, resolved_out_sz, lookup_err);
}

/* Resolve an absolute path within a vnode tree */
/* Ordinary lookup shares the mount-aware symlink walk with openat2. The
 * explicit mount preserves the chosen path when a filesystem is bind-mounted. */
vnode_t *vnode_lookup_path(mount_t *mnt, const char *path) {
    if (!mnt || !mnt->root) return NULL;
    vfs_set_lookup_errno(0);
    if (!path || !*path) {
        vnode_get(mnt->root);
        return mnt->root;
    }
    while (*path == '/') path++;
    char absolute[MAX_PATH_LEN], resolved[MAX_PATH_LEN];
    if (vfs_path_join(mnt->path, path, absolute, sizeof(absolute)) < 0) {
        vfs_set_lookup_errno(-ENAMETOOLONG);
        return NULL;
    }
    task_t *task = proc_current();
    const char *root = task && task->fs.root_path[0] ? task->fs.root_path : "/";
    int error = 0;
    vnode_t *vn = vnode_lookup_path_openat2(absolute, mnt->path, root, 0,
                                          resolved, sizeof(resolved), &error);
    if (!vn) vfs_set_lookup_errno(error);
    return vn;
}

vnode_t *vfs_resolve(const char *path) {
    task_t *cur = proc_current();
    const char *cwd = (cur && cur->fs.cwd[0]) ? cur->fs.cwd : "/";
    return vfs_resolve_at(path, cwd);
}

vnode_t *vfs_resolve_no_follow(const char *path) {
    return vfs_resolve_no_follow_final(path);
}

vnode_t *vfs_resolve_at(const char *path, const char *cwd) {
    char resolved[MAX_PATH_LEN];

    if (vfs_path_join(cwd, path, resolved, sizeof(resolved)) < 0)
        return NULL;

    task_t *cur = proc_current();
    const char *root = (cur && cur->fs.root_path[0]) ? cur->fs.root_path : "/";
    if (strcmp(root, "/") != 0 && !path_is_beneath(root, resolved)) {
        char rooted[MAX_PATH_LEN];
        if (strcmp(resolved, "/") == 0)
            snprintf(rooted, sizeof(rooted), "%s", root);
        else
            snprintf(rooted, sizeof(rooted), "%s%s", root, resolved);
        strncpy(resolved, rooted, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }
    if (vfs_path_normalize_absolute_with_root(resolved, root) < 0)
        return NULL;

    /* Find best matching mount */
    mount_t *mnt = vfs_find_mount(resolved);
    if (!mnt) return NULL;

    const char *rel = vfs_strip_mount_prefix(resolved, mnt);
    return vnode_lookup_path(mnt, rel);
}
