/*
 * Mount namespace objects: refcounted per-namespace mount tables.
 * See kernel/include/fs/vfs/mntns.h for the reference model.
 */
#include "fs/vfs/mntns.h"
#include "fs/vfs/mount.h"
#include "proc/proc_internal.h"
#include "core/lock.h"
#include "core/string.h"
#include "mm/slab.h"

static mnt_namespace_t  g_init_ns;
static mnt_namespace_t *g_ns_list;
static spinlock_t       g_mntns_lock = SPINLOCK_INIT;
static uint64_t         g_next_ino = MNTNS_INIT_INO + 1;

void mntns_early_init(void)
{
    memset(&g_init_ns, 0, sizeof(g_init_ns));
    g_init_ns.ino = MNTNS_INIT_INO;
    refcount_set(&g_init_ns.refs, 1);  /* pinned: never freed */
    g_ns_list = &g_init_ns;
}

mnt_namespace_t *mntns_current(void)
{
    task_t *t = proc_current();
    if (t) {
        /* All writers take g_mntns_lock; read with matching acquire so the
         * pointer and the table it points at are seen consistently. */
        mnt_namespace_t *ns = (mnt_namespace_t *)__atomic_load_n(
            &t->mnt_ns, __ATOMIC_ACQUIRE);
        if (ns)
            return ns;
    }
    return &g_init_ns;
}

uint64_t mntns_task_ino(const task_t *t)
{
    if (t && t->mnt_ns)
        return ((const mnt_namespace_t *)t->mnt_ns)->ino;
    return MNTNS_INIT_INO;
}

mnt_namespace_t *mntns_task_get(task_t *t)
{
    if (!t)
        return &g_init_ns;
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    mnt_namespace_t *ns = (mnt_namespace_t *)t->mnt_ns;
    if (ns)
        refcount_inc(&ns->refs);
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    return ns ? ns : &g_init_ns;
}

static mnt_namespace_t *mntns_alloc(void)
{
    mnt_namespace_t *ns = (mnt_namespace_t *)kmalloc(sizeof(*ns));
    if (!ns)
        return NULL;
    memset(ns, 0, sizeof(*ns));
    refcount_set(&ns->refs, 1);
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    ns->ino = g_next_ino++;
    ns->next = g_ns_list;
    g_ns_list = ns;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    return ns;
}

/* Deep-copy the mount table: entries are duplicated, root vnodes and
 * fs_data stay shared with the source namespace.  The source entry keeps the
 * real holder count (ns_users: itself + every copy) and is the only entry
 * allowed to run the filesystem teardown; each copy is marked
 * VFS_MOUNT_NS_SHARED so an umount there only detaches the local table entry.
 * Because a copy cannot reach back to its source to decrement the count, the
 * count is deliberately conservative: a shared filesystem is destroyed when
 * the source unmounts it, and a source that still lists copies simply keeps
 * the fs alive rather than risking a use-after-free. */
static void mntns_copy_mounts(mnt_namespace_t *dst, mnt_namespace_t *src)
{
    for (int i = 0; i < src->nmounts; i++) {
        mount_t *mnt = (mount_t *)kmalloc(sizeof(*mnt));
        if (!mnt) {
            /* Near-OOM: keep the partially copied table rather than failing
             * the whole namespace; the copy simply sees fewer mounts. */
            dst->nmounts = i;
            return;
        }
        *mnt = *src->mounts[i];
        mnt->dead_next = NULL;
        src->mounts[i]->ns_users++;
        src->mounts[i]->flags |= VFS_MOUNT_NS_SHARED;
        mnt->ns_users = 0;  /* copy: detach-only, never destroys */
        mnt->flags |= VFS_MOUNT_NS_SHARED;
        dst->mounts[i] = mnt;
    }
    dst->nmounts = src->nmounts;
}

int mntns_unshare(task_t *t)
{
    if (!t)
        return -ESRCH;
    mnt_namespace_t *cur = t->mnt_ns ? (mnt_namespace_t *)t->mnt_ns
                                     : &g_init_ns;
    mnt_namespace_t *ns = mntns_alloc();
    if (!ns)
        return -ENOMEM;
    mntns_copy_mounts(ns, cur);
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    mnt_namespace_t *old = (mnt_namespace_t *)t->mnt_ns;
    t->mnt_ns = ns;  /* takes over the fresh reference */
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    if (old)
        mntns_put(old);
    return 0;
}

int mntns_fork(task_t *child, task_t *parent, uint64_t clone_flags)
{
    if (!child)
        return -EINVAL;
    mnt_namespace_t *pns = (parent && parent->mnt_ns)
                               ? (mnt_namespace_t *)parent->mnt_ns
                               : &g_init_ns;
    if (clone_flags & CLONE_NEWNS) {
        mnt_namespace_t *ns = mntns_alloc();
        if (!ns)
            return -ENOMEM;
        mntns_copy_mounts(ns, pns);
        child->mnt_ns = ns;
        return 0;
    }
    if (pns == &g_init_ns) {
        child->mnt_ns = NULL;  /* initial namespace: no reference needed */
        return 0;
    }
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    refcount_inc(&pns->refs);
    child->mnt_ns = pns;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    return 0;
}

int mntns_join(task_t *t, mnt_namespace_t *ns)
{
    if (!t || !ns)
        return -EINVAL;
    int to_init = (ns == &g_init_ns);
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    mnt_namespace_t *old = (mnt_namespace_t *)t->mnt_ns;
    /* NULL means the initial namespace; its reference is not tracked. */
    t->mnt_ns = to_init ? NULL : ns;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    if (to_init) {
        /* The caller's pin on the initial namespace is not a counted one. */
        return 0;
    }
    if (old)
        mntns_put(old);
    return 0;
}

void mntns_put(mnt_namespace_t *ns)
{
    if (!ns || ns == &g_init_ns)
        return;
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    if (!refcount_dec_and_test(&ns->refs)) {
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        return;
    }
    mnt_namespace_t **pp = &g_ns_list;
    while (*pp) {
        if (*pp == ns) {
            *pp = ns->next;
            break;
        }
        pp = &(*pp)->next;
    }
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    /* Release this namespace's holds on its mounts before freeing the table,
     * so a shared filesystem still gets its teardown from the last holder. */
    vfs_mount_namespace_teardown(ns);
    for (int i = 0; i < ns->nmounts; i++)
        kfree(ns->mounts[i]);
    mount_t *dead = ns->dead_mounts;
    while (dead) {
        mount_t *next = dead->dead_next;
        kfree(dead);
        dead = next;
    }
    kfree(ns);
}

void mntns_release_task(task_t *t)
{
    if (!t)
        return;
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    mnt_namespace_t *old = (mnt_namespace_t *)t->mnt_ns;
    t->mnt_ns = NULL;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    if (old)
        mntns_put(old);
}
