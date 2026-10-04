#include "fs/vfs/mount.h"
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
    return mntns_current()->nmounts;
}

mount_t *vfs_mount_at(int index)
{
    mnt_namespace_t *ns = mntns_current();
    if (index < 0 || index >= ns->nmounts)
        return NULL;
    return ns->mounts[index];
}

mount_t *vfs_mount_alloc(void)
{
    mnt_namespace_t *ns = mntns_current();
    if (ns->nmounts >= MNTNS_MAX_MOUNTS)
        return NULL;
    mount_t *mnt = (mount_t *)kmalloc(sizeof(*mnt));
    if (!mnt)
        return NULL;
    memset(mnt, 0, sizeof(*mnt));
    mnt->attached = 1;
    /* One reference for the namespace table itself; process roots add to it
     * through vfs_mount_root_get().  An attached mount therefore never drops
     * below 1, which is what makes "is somebody's root?" a comparison
     * against a constant instead of a scan. */
    mnt->root_users = 1;
    mnt->mnt_id = ++g_mount_id_seq;
    ns->mounts[ns->nmounts++] = mnt;
    return mnt;
}

/* ---- tree position -------------------------------------------------- */

/* Unlink @mnt from its parent's child list.  Safe to call on a root. */
static void mount_tree_unlink(mount_t *mnt)
{
    mount_t *parent = mnt->mnt_parent;
    mnt->mnt_sibling = NULL;
    if (!parent)
        return;
    mount_t **pp = &parent->mnt_child;
    while (*pp) {
        if (*pp == mnt) {
            *pp = NULL;
            break;
        }
        pp = &(*pp)->mnt_sibling;
    }
}

/*
 * Attach @mnt under @parent covering the vnode @mp (NULL for a namespace
 * root).  The mount takes a reference on @mp and drops the one it held on
 * its previous mountpoint, so re-parenting never leaks or dangles.
 */
int vfs_mount_attach(mount_t *mnt, mount_t *parent, vnode_t *mp)
{
    if (!mnt)
        return -EINVAL;
    mount_tree_unlink(mnt);
    if (mnt->mnt_mp)
        vnode_put(mnt->mnt_mp);
    mnt->mnt_mp = mp;
    if (mp)
        vnode_get(mp);
    mnt->mnt_parent = parent;
    if (parent) {
        mnt->mnt_sibling = parent->mnt_child;
        parent->mnt_child = mnt;
    }
    return 0;
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
    if (!mnt || mnt->mnt_parent || mnt->mnt_child)
        return;                 /* already positioned */

    const char *path = mnt->path;
    size_t plen = strlen(path);
    while (plen > 1 && path[plen - 1] == '/')
        plen--;

    /* Namespace root: no parent, no mountpoint. */
    if (plen <= 1) {
        mnt->mnt_parent = NULL;
        return;
    }

    char parent_path[MAX_PATH_LEN];
    if (plen >= sizeof(parent_path))
        return;
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';
    char *slash = strrchr(parent_path, '/');
    if (!slash)
        return;
    if (slash == parent_path)
        parent_path[1] = '\0';
    else
        *slash = '\0';

    mount_t *parent = vfs_find_mount(parent_path);
    if (!parent)
        return;                 /* orphan: path resolution still works */

    /* The mountpoint vnode is the mount point resolved inside the parent,
     * which is the last component of @path.  It may legitimately fail to
     * resolve for synthetic filesystems; the mount still works, it just has
     * no object to hang its position on. */
    vnode_t *mp = vfs_resolve(path);
    if (mp && mp->mnt == parent) {
        vfs_mount_attach(mnt, parent, mp);
        vnode_put(mp);
    } else {
        if (mp)
            vnode_put(mp);
        mnt->mnt_parent = parent;
        mnt->mnt_sibling = parent->mnt_child;
        parent->mnt_child = mnt;
    }
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

/*
 * Cut @mnt out of the tree without consulting the busy rules.  This is the
 * pivot_root path, and it is exactly the operation that has to be allowed
 * to make a busy root unreachable: the object stays in the table so
 * mountinfo can still report it, but flagged VFS_MOUNT_DETACHED so no
 * absolute path resolves through it again.  Open file descriptors into the
 * old root keep working, which is the whole point.
 */
void vfs_mount_detach_root(mount_t *mnt)
{
    if (!mnt)
        return;
    mount_tree_unlink(mnt);
    mnt->mnt_parent = NULL;
    mnt->flags |= VFS_MOUNT_DETACHED;
}

void vfs_mount_remove(mount_t *mnt)
{
    if (!mnt)
        return;
    mnt_namespace_t *ns = mntns_current();
    int idx = -1;
    for (int i = 0; i < ns->nmounts; i++) {
        if (ns->mounts[i] == mnt) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return;
    /* Detach without freeing: the table previously compacted an inline array
     * here, which repointed every vnode->mnt, dcache, quota and xattr
     * reference at the mount that shifted into the slot.  The heap object is
     * parked on the namespace graveyard instead and freed when the namespace
     * dies, so stale holders read a frozen, coherent mount instead of
     * someone else's. */
    mount_tree_unlink(mnt);
    mnt->attached = 0;
    if (mnt->mnt_mp) {
        vnode_put(mnt->mnt_mp);
        mnt->mnt_mp = NULL;
    }
    mnt->dead_next = ns->dead_mounts;
    ns->dead_mounts = mnt;
    for (int i = idx; i < ns->nmounts - 1; i++)
        ns->mounts[i] = ns->mounts[i + 1];
    ns->mounts[ns->nmounts - 1] = NULL;
    ns->nmounts--;
    vfs_mount_root_put(mnt); /* the namespace table's own reference */
}

mount_t *vfs_find_mount(const char *path)
{
    mnt_namespace_t *ns = mntns_current();
    mount_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < ns->nmounts; i++) {
        mount_t *m = ns->mounts[i];
        /* A detached mount (pivot_root's old root) is unreachable by path. */
        if (m->flags & VFS_MOUNT_DETACHED)
            continue;
        size_t len = strlen(m->path);
        if (strncmp(path, m->path, len) == 0 &&
            (len == 1 || path[len] == '\0' || path[len] == '/') &&
            (len > best_len
#ifdef CONFIG_EXTERNAL_ROOT
             /* Only external-root kernels replace the bootstrap ramfs at
              * /.  Preserve the original first-match behavior for every
              * other build and for equal-length non-root mount points. */
             || (len == 1 && best_len == 1)
#endif
            )) {
            best = m;
            best_len = len;
        }
    }
    return best;
}

/* Same match as vfs_find_mount() but ignoring VFS_MOUNT_DETACHED, for the
 * umount path and for callers that need to name a mount pivot_root cut
 * loose. */
mount_t *vfs_find_mount_including_detached(const char *path)
{
    mnt_namespace_t *ns = mntns_current();
    mount_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < ns->nmounts; i++) {
        mount_t *m = ns->mounts[i];
        size_t len = strlen(m->path);
        if (strncmp(path, m->path, len) == 0 &&
            (len == 1 || path[len] == '\0' || path[len] == '/') &&
            len > best_len) {
            best = m;
            best_len = len;
        }
    }
    return best;
}

int vfs_move_mount(const char *from, const char *to)
{
    if (!from || !to || from[0] != '/' || to[0] != '/')
        return -EINVAL;
    mount_t *mnt = vfs_find_mount(from);
    if (!mnt)
        return -ENOENT;
    if (mnt->flags & VFS_MOUNT_DETACHED)
        return -EINVAL;
    /* Moving a mount re-parents it: the target must not already be covered
     * by another mount, or the move would silently merge two filesystems
     * into one path. */
    if (vfs_find_mount(to))
        return -EBUSY;
    if (strcmp(mnt->path, to) == 0)
        return 0;
    /* Refuse to move a mount underneath itself. */
    if (strncmp(to, mnt->path, strlen(mnt->path)) == 0 &&
        (to[strlen(mnt->path)] == '\0' || to[strlen(mnt->path)] == '/'))
        return -EINVAL;

    char old[MAX_PATH_LEN];
    strncpy(old, mnt->path, sizeof(old) - 1);
    old[sizeof(old) - 1] = '\0';

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

    strncpy(mnt->path, to, MAX_PATH_LEN - 1);
    mnt->path[MAX_PATH_LEN - 1] = '\0';
    vfs_mount_attach(mnt, new_parent == mnt ? NULL : new_parent, new_mp);
    if (new_mp)
        vnode_put(new_mp); /* the mount holds its own reference now */
    (void)old;
    vfs_dcache_invalidate_all();
    return 0;
}

const char *vfs_strip_mount_prefix(const char *path, const mount_t *mnt)
{
    size_t len = strlen(mnt->path);
    if (strncmp(path, mnt->path, len) == 0) {
        const char *rest = path + len;
        if (*rest == '/')
            rest++;
        return rest;
    }
    return path;
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
    strncpy(parent, mnt->path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    size_t len = strlen(parent);
    while (len > 1 && parent[len - 1] == '/')
        parent[--len] = '\0';
    char *slash = strrchr(parent, '/');
    if (!slash) return NULL;
    if (slash == parent) {
        parent[1] = '\0';
    } else {
        *slash = '\0';
    }
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