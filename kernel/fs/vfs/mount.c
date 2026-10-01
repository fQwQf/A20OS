#include "fs/vfs/mount.h"
#include "fs/vfs/mntns.h"
#include "core/string.h"
#include "mm/slab.h"

/*
 * The mount table lives inside the caller's mount namespace
 * (kernel/fs/vfs/mntns.c).  Every accessor below implicitly operates on the
 * current task's namespace — the initial namespace when there is no current
 * task (boot, kernel threads) — so existing VFS call sites need no changes.
 */

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
    ns->mounts[ns->nmounts++] = mnt;
    return mnt;
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
    mnt->dead_next = ns->dead_mounts;
    ns->dead_mounts = mnt;
    for (int i = idx; i < ns->nmounts - 1; i++)
        ns->mounts[i] = ns->mounts[i + 1];
    ns->mounts[ns->nmounts - 1] = NULL;
    ns->nmounts--;
}

mount_t *vfs_find_mount(const char *path)
{
    mnt_namespace_t *ns = mntns_current();
    mount_t *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < ns->nmounts; i++) {
        size_t len = strlen(ns->mounts[i]->path);
        if (strncmp(path, ns->mounts[i]->path, len) == 0 &&
            (len == 1 || path[len] == '\0' || path[len] == '/') &&
            (len > best_len
#ifdef CONFIG_EXTERNAL_ROOT
             /* Only external-root kernels replace the bootstrap ramfs at
              * /.  Preserve the original first-match behavior for every
              * other build and for equal-length non-root mount points. */
             || (len == 1 && best_len == 1)
#endif
            )) {
            best = ns->mounts[i];
            best_len = len;
        }
    }
    return best;
}

int vfs_move_mount(const char *from, const char *to)
{
    if (!from || !to)
        return -EINVAL;
    mount_t *mnt = vfs_find_mount(from);
    if (!mnt)
        return -ENOENT;
    if (vfs_find_mount(to))
        return -EBUSY;
    strncpy(mnt->path, to, MAX_PATH_LEN - 1);
    mnt->path[MAX_PATH_LEN - 1] = '\0';
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
    if (!mnt || strcmp(mnt->path, "/") == 0)
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
