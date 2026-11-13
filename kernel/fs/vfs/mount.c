#include "fs/vfs/mount.h"
#include "fs/vfs/mount_state.h"
#include "fs/vfs/mntns.h"
#include "fs/vfs/dcache.h"
#include "core/string.h"
#include "mm/slab.h"

/*
 * The mount table lives inside the caller's mount namespace
 * (kernel/fs/vfs/mntns.c).  Every accessor below implicitly operates on the
 * current task's namespace — the initial namespace when there is no current
 * task (boot, kernel threads) — so existing VFS call sites need no changes.
 *
 * Two views of the same set of mounts are maintained side by side:
 *
 *   - the tree (mnt_parent / mnt_mp / mnt_child), which says where a mount
 *     is attached and who keeps it alive, and
 *   - the flattened path prefix (mount_t::path), which says which mount an
 *     absolute path resolves in.
 *
 * The tree is what umount's busy rules, pivot_root and mountinfo's parent
 * ids are computed from; the prefix is what the path walker matches.  They
 * are set together at mount time and moved together by move_mount, so they
 * cannot disagree about a live mount.
 */

static uint32_t g_mount_id_seq;

void vfs_mount_table_init(void)
{
    mntns_early_init();
}

int vfs_mount_count(void)
{
    uint64_t flags = mntns_mount_table_lock();
    mnt_namespace_t *ns = mntns_current();
    int count = 0;
    for (int i = 0; i < ns->nmounts; i++)
        count += ns->mounts[i]->publish_ready &&
                 !ns->mounts[i]->teardown_running;
    mntns_mount_table_unlock(flags);
    return count;
}

mount_t *vfs_mount_at(int index)
{
    mnt_namespace_t *ns = mntns_current();
    uint64_t flags = mntns_mount_table_lock();
    if (index < 0) {
        mntns_mount_table_unlock(flags);
        return NULL;
    }
    mount_t *mnt = NULL;
    for (int i = 0; i < ns->nmounts; i++) {
        if (!ns->mounts[i]->publish_ready ||
            ns->mounts[i]->teardown_running)
            continue;
        if (index-- == 0) {
            mnt = ns->mounts[i];
            break;
        }
    }
    mntns_mount_table_unlock(flags);
    return mnt;
}

mount_t *vfs_mount_alloc(void)
{
    mnt_namespace_t *ns = mntns_current();
    mount_t *mnt = (mount_t *)kmalloc(sizeof(*mnt));
    if (!mnt)
        return NULL;
    memset(mnt, 0, sizeof(*mnt));
    mnt->attached = 1;
    /* The namespace table itself is the source mount's initial ownership
     * reference. Namespace copies pin this object through shared_source. */
    mnt->ns_users = 1;
    /* One reference for the namespace table itself; process roots add to it
     * through vfs_mount_root_get().  An attached mount therefore never drops
     * below 1, which is what makes "is somebody's root?" a comparison
     * against a constant instead of a scan. */
    mnt->root_users = 1;
    uint64_t flags = mntns_mount_table_lock();
    if (ns->nmounts >= MNTNS_MAX_MOUNTS) {
        mntns_mount_table_unlock(flags);
        kfree(mnt);
        return NULL;
    }
    mnt->mnt_id = ++g_mount_id_seq;
    ns->mounts[ns->nmounts++] = mnt;
    mntns_mount_table_unlock(flags);
    return mnt;
}

/* ---- tree position -------------------------------------------------- */

/* Unlink @mnt from its parent's child list.  Safe to call on a root. */
/* Mechanical bridge for the Rust mount-tree side effects. */
static void *mt_parent(void *m) { return ((mount_t *)m)->mnt_parent; }
static void *mt_child(void *m) { return ((mount_t *)m)->mnt_child; }
static void *mt_sibling(void *m) { return ((mount_t *)m)->mnt_sibling; }
static void *mt_mp(void *m) { return ((mount_t *)m)->mnt_mp; }
static void mt_set_parent(void *m, void *v) { ((mount_t *)m)->mnt_parent = (mount_t *)v; }
static void mt_set_child(void *m, void *v) { ((mount_t *)m)->mnt_child = (mount_t *)v; }
static void mt_set_sibling(void *m, void *v) { ((mount_t *)m)->mnt_sibling = (mount_t *)v; }
static void mt_set_mp(void *m, void *v) { ((mount_t *)m)->mnt_mp = (vnode_t *)v; }
static void mt_set_detached(void *m) { ((mount_t *)m)->detached = 1; }
static void mt_vnode_get(void *v) { vnode_get((vnode_t *)v); }
static void mt_vnode_put(void *v) { vnode_put((vnode_t *)v); }
static void vfs_mount_mark_ready(mount_t *mnt);
static const char *mt_path(void *m) { return ((mount_t *)m)->path; }
static void *mt_find_mount(const char *path) { return vfs_find_mount(path); }
static void *mt_resolve(const char *path) { return vfs_resolve(path); }
static void *mt_vnode_mount(void *v) { return ((vnode_t *)v)->mnt; }
static uint64_t mt_table_lock(void) { return mntns_mount_table_lock(); }
static void mt_table_unlock(uint64_t flags) { mntns_mount_table_unlock(flags); }
static int mt_is_present(void *m)
{
    mount_t *mnt = (mount_t *)m;
    mnt_namespace_t *ns = mntns_current();
    for (int i = 0; i < ns->nmounts; i++)
        if (ns->mounts[i] == mnt) return 1;
    return 0;
}
static int mt_is_teardown(void *m) { return ((mount_t *)m)->teardown_running != 0; }
static void mt_set_path(void *m, const char *path)
{
    mount_t *mnt = (mount_t *)m;
    strncpy(mnt->path, path, MAX_PATH_LEN - 1);
    mnt->path[MAX_PATH_LEN - 1] = '\0';
}
static void mt_invalidate_dcache(void) { vfs_dcache_invalidate_all(); }
static void mt_mark_ready(void *m) { vfs_mount_mark_ready((mount_t *)m); }

static const struct vfs_mount_tree_ops g_vfs_mount_tree_ops = {
    .parent = mt_parent,
    .child = mt_child,
    .sibling = mt_sibling,
    .mp = mt_mp,
    .set_parent = mt_set_parent,
    .set_child = mt_set_child,
    .set_sibling = mt_set_sibling,
    .set_mp = mt_set_mp,
    .set_detached = mt_set_detached,
    .vnode_get = mt_vnode_get,
    .vnode_put = mt_vnode_put,
};
_Static_assert(sizeof(g_vfs_mount_tree_ops) / sizeof(void *) == 11,
               "vfs mount tree mirror: ops table field count");

static const struct vfs_mount_link_ops g_vfs_mount_link_ops = {
    .path = mt_path,
    .find_mount = mt_find_mount,
    .resolve = mt_resolve,
    .vnode_mount = mt_vnode_mount,
    .mark_ready = mt_mark_ready,
};
_Static_assert(sizeof(g_vfs_mount_link_ops) / sizeof(void *) == 5,
               "vfs mount link mirror: ops table field count");

static const struct vfs_move_mount_ops g_vfs_move_mount_ops = {
    .lock = mt_table_lock,
    .unlock = mt_table_unlock,
    .is_present = mt_is_present,
    .is_teardown = mt_is_teardown,
    .set_path = mt_set_path,
    .invalidate_dcache = mt_invalidate_dcache,
};
_Static_assert(sizeof(g_vfs_move_mount_ops) / sizeof(void *) == 6,
               "vfs move mount mirror: ops table field count");

static int rm_count(void *ns) { return ((mnt_namespace_t *)ns)->nmounts; }
static void rm_set_count(void *ns, int count) { ((mnt_namespace_t *)ns)->nmounts = count; }
static void *rm_current_ns(void) { return mntns_current(); }
static void *rm_at(void *ns, int i) { return ((mnt_namespace_t *)ns)->mounts[i]; }
static void rm_set_at(void *ns, int i, void *m) { ((mnt_namespace_t *)ns)->mounts[i] = (mount_t *)m; }
static void *rm_dead_head(void *ns) { return ((mnt_namespace_t *)ns)->dead_mounts; }
static void rm_set_dead_head(void *ns, void *m) { ((mnt_namespace_t *)ns)->dead_mounts = (mount_t *)m; }
static void rm_set_dead_next(void *m, void *next) { ((mount_t *)m)->dead_next = (mount_t *)next; }
static void rm_set_attached(void *m, int attached) { ((mount_t *)m)->attached = attached; }
static void rm_clear_teardown(void *m) { ((mount_t *)m)->teardown_running = 0; }
static void *rm_mountpoint(void *m) { return ((mount_t *)m)->mnt_mp; }
static void rm_clear_mountpoint(void *m) { ((mount_t *)m)->mnt_mp = NULL; }
static void rm_vnode_put(void *v) { vnode_put((vnode_t *)v); }
static void rm_root_put(void *m) { vfs_mount_root_put((mount_t *)m); }
static const struct vfs_mount_remove_ops g_vfs_mount_remove_ops = {
    .lock = mt_table_lock, .unlock = mt_table_unlock, .count = rm_count,
    .set_count = rm_set_count,
    .current_ns = rm_current_ns, .at = rm_at, .set_at = rm_set_at,
    .dead_head = rm_dead_head, .set_dead_head = rm_set_dead_head,
    .set_dead_next = rm_set_dead_next, .set_attached = rm_set_attached,
    .clear_teardown = rm_clear_teardown, .mountpoint = rm_mountpoint,
    .clear_mountpoint = rm_clear_mountpoint, .vnode_put = rm_vnode_put,
    .root_put = rm_root_put,
};
_Static_assert(sizeof(g_vfs_mount_remove_ops) / sizeof(void *) == 16,
               "vfs mount remove mirror: ops table field count");

int vfs_mount_execute_umount(const struct vfs_umount_execute_ops *ops,
                             void *mnt, const struct vfs_umount_plan *plan)
{
    return a20_vfs_umount_execute(&g_vfs_mount_tree_ops, ops, mnt, plan);
}

static void vfs_mount_mark_ready(mount_t *mnt)
{
    uint64_t flags = mntns_mount_table_lock();
    mnt->publish_ready = 1;
    mntns_mount_table_unlock(flags);
}

/*
 * Attach @mnt under @parent covering the vnode @mp (NULL for a namespace
 * root).  The mount takes a reference on @mp and drops the one it held on
 * its previous mountpoint, so re-parenting never leaks or dangles.
 */
int vfs_mount_attach(mount_t *mnt, mount_t *parent, vnode_t *mp)
{
    return a20_vfs_mount_attach(&g_vfs_mount_tree_ops, mnt, parent, mp);
}

mount_t *vfs_mount_first_child(mount_t *mnt)
{
    return mnt ? mnt->mnt_child : NULL;
}

mount_t *vfs_mount_next_sibling(mount_t *mnt)
{
    return mnt ? mnt->mnt_sibling : NULL;
}

/*
 * Give a freshly configured mount its place in the tree.  Called once per
 * mount, after path/root are filled in and before the mount becomes
 * reachable: the parent is whichever mount already covers the mount point,
 * and the mountpoint vnode is that path resolved inside the parent.
 *
 * The parent must be found by prefix before the new mount is visible, which
 * is exactly what vfs_find_mount() does here -- the new entry's own path
 * would otherwise match itself.
 */
void vfs_mount_link_tree(mount_t *mnt)
{
    a20_vfs_mount_link_tree(&g_vfs_mount_tree_ops,
                            &g_vfs_mount_link_ops, mnt);
}

/* True when @ancestor is @mnt or any of its ancestors. */
bool vfs_mount_is_ancestor_of(mount_t *ancestor, mount_t *mnt)
{
    for (mount_t *m = mnt; m; m = m->mnt_parent) {
        if (m == ancestor)
            return true;
    }
    return false;
}

/* ---- root pinning ---------------------------------------------------- */

void vfs_mount_root_get(mount_t *mnt)
{
    if (mnt)
        __atomic_add_fetch(&mnt->root_users, 1, __ATOMIC_RELAXED);
}

void vfs_mount_root_put(mount_t *mnt)
{
    if (!mnt)
        return;
    if (__atomic_sub_fetch(&mnt->root_users, 1, __ATOMIC_RELAXED) < 0)
        mnt->root_users = 0;
}

int vfs_mount_root_users(mount_t *mnt)
{
    return mnt ? __atomic_load_n(&mnt->root_users, __ATOMIC_RELAXED) : 0;
}

void vfs_mount_cwd_get(mount_t *mnt)
{
    if (mnt)
        __atomic_add_fetch(&mnt->cwd_users, 1, __ATOMIC_RELAXED);
}

void vfs_mount_cwd_put(mount_t *mnt)
{
    if (!mnt)
        return;
    if (__atomic_sub_fetch(&mnt->cwd_users, 1, __ATOMIC_RELAXED) < 0)
        mnt->cwd_users = 0;
}

int vfs_mount_cwd_users(mount_t *mnt)
{
    return mnt ? __atomic_load_n(&mnt->cwd_users, __ATOMIC_RELAXED) : 0;
}

void vfs_mount_vfile_get(mount_t *mnt)
{
    if (!mnt)
        return;
    uint64_t flags = mntns_mount_table_lock();
    if (mnt->open_vfiles != (unsigned int)-1)
        mnt->open_vfiles++;
    mntns_mount_table_unlock(flags);
}

void vfs_mount_vfile_put(mount_t *mnt)
{
    if (!mnt)
        return;
    uint64_t flags = mntns_mount_table_lock();
    if (mnt->open_vfiles)
        mnt->open_vfiles--;
    mntns_mount_table_unlock(flags);
}

/*
 * Cut @mnt out of the tree without consulting the busy rules.  This is the
 * pivot_root path, and it is exactly the operation that has to be allowed
 * to make a busy root unreachable: the object stays in the table so
 * mountinfo can still report it, but marked detached so no
 * absolute path resolves through it again.  Open file descriptors into the
 * old root keep working, which is the whole point.
 */
void vfs_mount_detach_root(mount_t *mnt)
{
    a20_vfs_mount_detach_root(&g_vfs_mount_tree_ops, mnt);
}

void vfs_mount_remove(mount_t *mnt)
{
    a20_vfs_mount_remove(&g_vfs_mount_tree_ops, &g_vfs_mount_remove_ops, mnt);
}

/* Snapshot the visible mounts and let the engine pick the longest matching
 * prefix; C keeps the table walk and the lock, the visibility/propagation
 * decision is Rust-owned.  The captured `sel[]` keeps the pointer->object
 * mapping intact after the lock is dropped. */
static mount_t *vfs_find_mount_common(const char *path, int include_detached,
                                      int external_root)
{
    mnt_namespace_t *ns = mntns_current();
    vfs_mount_match_t cands[MNTNS_MAX_MOUNTS];
    mount_t *sel[MNTNS_MAX_MOUNTS];
    int n = 0;
    uint64_t flags = mntns_mount_table_lock();
    for (int i = 0; i < ns->nmounts && n < MNTNS_MAX_MOUNTS; i++) {
        mount_t *m = ns->mounts[i];
        sel[n] = m;
        cands[n].path = m->path;
        cands[n].ready = m->publish_ready;
        cands[n].teardown = m->teardown_running;
        cands[n].detached = m->detached;
        n++;
    }
    mntns_mount_table_unlock(flags);
    int idx = a20_vfs_find_mount(path, include_detached, external_root, cands,
                                 (size_t)n);
    return idx >= 0 ? sel[idx] : NULL;
}
#ifdef CONFIG_EXTERNAL_ROOT
#define VFS_EXTERNAL_ROOT_TIE 1
#else
#define VFS_EXTERNAL_ROOT_TIE 0
#endif

mount_t *vfs_find_mount(const char *path)
{
    return vfs_find_mount_common(path, 0, VFS_EXTERNAL_ROOT_TIE);
}

/* Same match as vfs_find_mount() but including detached mounts, for the
 * umount path and for callers that need to name a mount pivot_root cut
 * loose. */
mount_t *vfs_find_mount_including_detached(const char *path)
{
    return vfs_find_mount_common(path, 1, 0);
}

int vfs_move_mount(const char *from, const char *to)
{
    mount_t *mnt = NULL;
    int covered = 0;
    if (from && to && from[0] == '/' && to[0] == '/') {
        mnt = vfs_find_mount(from);
        covered = vfs_find_mount(to) != NULL;
    }
    int noop = 0;
    int vr = a20_vfs_move_mount_check(from, to, mnt != NULL,
                                      mnt ? mnt->path : NULL,
                                      mnt ? (mnt->detached != 0) : 0,
                                      covered, &noop);
    if (vr < 0)
        return vr;
    if (noop)
        return 0;

    /* The new parent is the mount that covers the destination; the new
     * mountpoint is the destination vnode resolved inside it. */
    mount_t *new_parent = vfs_find_mount(to);
    vnode_t *new_mp = NULL;
    if (new_parent && strcmp(new_parent->path, to) != 0) {
        vnode_t *vn = vfs_resolve(to);
        if (!vn)
            return -ENOENT;
        new_mp = vn;
    } else if (new_parent) {
        new_mp = new_parent->root;
        if (new_mp)
            vnode_get(new_mp);
    }
    if (!new_parent) {
        if (new_mp)
            vnode_put(new_mp);
        return -ENOENT;
    }

    int commit = a20_vfs_move_mount_commit(&g_vfs_mount_tree_ops,
                                           &g_vfs_move_mount_ops, mnt,
                                           new_parent == mnt ? NULL : new_parent,
                                           new_mp, to);
    if (new_mp)
        vnode_put(new_mp); /* the mount holds its own reference now */
    return commit;
}

const char *vfs_strip_mount_prefix(const char *path, const mount_t *mnt)
{
    return path + a20_vfs_strip_prefix(path, mnt->path);
}

mount_t *vfs_mount_parent(mount_t *mnt)
{
    if (!mnt)
        return NULL;
    /* The tree is authoritative once the mount is attached to it. */
    if (mnt->mnt_parent || mnt->mnt_child)
        return mnt->mnt_parent;
    if (strcmp(mnt->path, "/") == 0)
        return NULL;

    char parent[MAX_PATH_LEN];
    if (!a20_vfs_mount_parent_path(mnt->path, parent, sizeof(parent)))
        return NULL;
    return vfs_find_mount(parent);
}

/* Mount id of the mount that covers @path, 0 when there is none.  Used by
 * mountinfo and statmount(2). */
uint32_t vfs_mount_id_for(const char *path)
{
    mount_t *mnt = vfs_find_mount(path);
    return mnt ? mnt->mnt_id : 0;
}

uint32_t vfs_mount_parent_id(mount_t *mnt)
{
    if (!mnt || !mnt->mnt_parent)
        return 0;
    return mnt->mnt_parent->mnt_id;
}
