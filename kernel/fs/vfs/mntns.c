/*
 * Mount namespace objects: refcounted per-namespace mount tables.
 * See kernel/include/fs/vfs/mntns.h for the reference model.
 */
#include "fs/vfs/mntns.h"
#include "fs/vfs/mount.h"
#include "proc/proc_internal.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "proc/proc.h"

static mnt_namespace_t  g_init_ns;
static mnt_namespace_t *g_ns_list;
static spinlock_t       g_mntns_lock = SPINLOCK_INIT;
static uint64_t         g_next_ino = MNTNS_INIT_INO + 1;
static uint64_t         g_teardown_pass;
static int              g_reaper_started;
/* Shutdown snapshots are deliberately bounded: if the namespace registry
 * ever exceeds this capacity, shutdown fails before closing admission. */
#define MNTNS_SHUTDOWN_MAX 256
static mnt_namespace_t *g_shutdown_namespaces[MNTNS_SHUTDOWN_MAX];
static int              g_shutdown_namespace_count;
static int              g_shutdown_active;
static unsigned int     g_namespace_create_inflight;
static unsigned int     g_vfs_mutations_inflight;
static unsigned int     g_vfs_fileops_inflight;

static int mntns_retry_pending_internal(int retry_busy);

int mntns_shutdown_active(void)
{
    return __atomic_load_n(&g_shutdown_active, __ATOMIC_ACQUIRE);
}

int mntns_shutdown_begin(void)
{
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    if (g_shutdown_active || g_namespace_create_inflight ||
        g_vfs_mutations_inflight || g_vfs_fileops_inflight) {
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        return -EBUSY;
    }
    int count = 0;
    for (mnt_namespace_t *ns = g_ns_list; ns; ns = ns->next) {
        if (count == MNTNS_SHUTDOWN_MAX || ns->teardown_running ||
            ns->teardown_pending) {
            spin_unlock_irqrestore(&g_mntns_lock, flags);
            return -EBUSY;
        }
        for (int i = 0; i < ns->nmounts; i++) {
            mount_t *mnt = ns->mounts[i];
            if (mnt && mnt->teardown_running) {
                spin_unlock_irqrestore(&g_mntns_lock, flags);
                return -EBUSY;
            }
        }
        count++;
    }
    __atomic_store_n(&g_shutdown_namespace_count, 0, __ATOMIC_RELAXED);
    for (mnt_namespace_t *ns = g_ns_list; ns; ns = ns->next) {
        refcount_inc(&ns->refs);
        g_shutdown_namespaces[g_shutdown_namespace_count++] = ns;
    }
    /* Publish only after the complete pinned array is ready. */
    __atomic_store_n(&g_shutdown_active, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    return 0;
}

int mntns_shutdown_namespace_count(void)
{
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    int count = g_shutdown_active ? g_shutdown_namespace_count : 0;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    return count;
}

mnt_namespace_t *mntns_shutdown_namespace_at(int index)
{
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    mnt_namespace_t *ns = NULL;
    if (g_shutdown_active && index >= 0 && index < g_shutdown_namespace_count)
        ns = g_shutdown_namespaces[index];
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    return ns;
}

int mntns_mutation_enter(void)
{
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    if (g_shutdown_active) {
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        return -ESHUTDOWN;
    }
    g_vfs_mutations_inflight++;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    return 0;
}

void mntns_mutation_leave(void)
{
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    if (g_vfs_mutations_inflight)
        g_vfs_mutations_inflight--;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
}

int mntns_fileop_enter(void)
{
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    if (g_shutdown_active) {
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        return -ESHUTDOWN;
    }
    g_vfs_fileops_inflight++;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    return 0;
}

void mntns_fileop_leave(void)
{
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    if (g_vfs_fileops_inflight)
        g_vfs_fileops_inflight--;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
}

void mntns_shutdown_abort(void)
{
    mnt_namespace_t *snapshot[MNTNS_SHUTDOWN_MAX];
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    if (!g_shutdown_active) {
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        return;
    }
    int count = g_shutdown_namespace_count;
    for (int i = 0; i < count; i++) {
        snapshot[i] = g_shutdown_namespaces[i];
        g_shutdown_namespaces[i] = NULL;
    }
    __atomic_store_n(&g_shutdown_namespace_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_shutdown_active, 0, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    /* Release outside the registry lock: a last put can perform filesystem
     * teardown and sleep. */
    for (int i = 0; i < count; i++)
        if (snapshot[i] == &g_init_ns) {
            uint64_t init_flags = spin_lock_irqsave(&g_mntns_lock);
            (void)refcount_dec_and_test(&g_init_ns.refs);
            spin_unlock_irqrestore(&g_mntns_lock, init_flags);
        } else {
            mntns_put(snapshot[i]);
        }
}

static void mntns_create_done(void)
{
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    if (g_namespace_create_inflight)
        g_namespace_create_inflight--;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
}

static void mntns_free(mnt_namespace_t *ns)
{
#ifdef CONFIG_MNTNS_TEST
    int source_mount_released = 0;
    printf("MNTNS_TEST: FREE ino=%llu refs=%u pending=%d nmounts=%d\n",
           (unsigned long long)ns->ino, refcount_read(&ns->refs),
           ns->teardown_pending, ns->nmounts);
    for (int i = 0; i < ns->nmounts; i++) {
        mount_t *mnt = ns->mounts[i];
        if (mnt && (strcmp(mnt->path, "/tmp/ns_shared") == 0 ||
                    mnt->shared_source))
            printf("MNTNS_TEST: FREE_MOUNT path=%s users=%d flags=%x shared=%d\n",
                   mnt->path, __atomic_load_n(&mnt->ns_users, __ATOMIC_ACQUIRE),
                   mnt->flags, mnt->shared_source != NULL);
        if (mnt && !mnt->shared_source &&
            !(mnt->flags & VFS_MOUNT_NS_SHARED) &&
            __atomic_load_n(&mnt->ns_users, __ATOMIC_ACQUIRE) == 0 &&
            strcmp(mnt->path, "/tmp/ns_shared") == 0) {
            source_mount_released = 1;
            break;
        }
    }
    if (!source_mount_released) {
        for (mount_t *mnt = ns->dead_mounts; mnt; mnt = mnt->dead_next) {
            if (strcmp(mnt->path, "/tmp/ns_shared") == 0 || mnt->shared_source)
                printf("MNTNS_TEST: FREE_DEAD path=%s users=%d flags=%x shared=%d\n",
                       mnt->path, __atomic_load_n(&mnt->ns_users, __ATOMIC_ACQUIRE),
                       mnt->flags, mnt->shared_source != NULL);
            if (!mnt->shared_source &&
                !(mnt->flags & VFS_MOUNT_NS_SHARED) &&
                __atomic_load_n(&mnt->ns_users, __ATOMIC_ACQUIRE) == 0 &&
                strcmp(mnt->path, "/tmp/ns_shared") == 0) {
                source_mount_released = 1;
                break;
            }
        }
    }
    if (source_mount_released && refcount_read(&ns->refs) == 0 &&
        !ns->teardown_pending)
        printf("MNTNS_TEST: SOURCE_MOUNT_RELEASED users=0 namespace_refs=0 pending=0\n");
#endif
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

static void mntns_remove_locked(mnt_namespace_t *ns)
{
    mnt_namespace_t **pp = &g_ns_list;
    while (*pp) {
        if (*pp == ns) {
            *pp = ns->next;
            ns->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

#ifdef CONFIG_MNTNS_TEST
static int mntns_test_tracks_shared_path(const mnt_namespace_t *ns)
{
    for (int i = 0; i < ns->nmounts; i++) {
        mount_t *mnt = ns->mounts[i];
        if (mnt && (strcmp(mnt->path, "/tmp/ns_shared") == 0 ||
                    mnt->shared_source))
            return 1;
    }
    for (mount_t *mnt = ns->dead_mounts; mnt; mnt = mnt->dead_next) {
        if (strcmp(mnt->path, "/tmp/ns_shared") == 0 || mnt->shared_source)
            return 1;
    }
    return 0;
}
#endif

static void mntns_reaper_thread(void)
{
    for (;;) {
        (void)mntns_retry_pending_internal(0);
        uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
        int pending = 0;
        for (mnt_namespace_t *ns = g_ns_list; ns; ns = ns->next) {
            if (ns->teardown_pending && ns->teardown_retryable) {
                pending = 1;
                break;
            }
        }
        if (!pending)
            g_reaper_started = 0;
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        if (!pending)
            return;
        proc_sleep_until(timer_get_ticks() + 100);
    }
}

static void mntns_start_reaper(void)
{
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    if (g_reaper_started) {
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        return;
    }
    g_reaper_started = 1;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    if (proc_alloc_detached(mntns_reaper_thread) < 0) {
        flags = spin_lock_irqsave(&g_mntns_lock);
        g_reaper_started = 0;
        spin_unlock_irqrestore(&g_mntns_lock, flags);
    }
}

void mntns_early_init(void)
{
    memset(&g_init_ns, 0, sizeof(g_init_ns));
    g_init_ns.ino = MNTNS_INIT_INO;
    refcount_set(&g_init_ns.refs, 1);  /* pinned: never freed */
    g_ns_list = &g_init_ns;
}

uint64_t mntns_mount_table_lock(void)
{
    return spin_lock_irqsave(&g_mntns_lock);
}

void mntns_mount_table_unlock(uint64_t flags)
{
    spin_unlock_irqrestore(&g_mntns_lock, flags);
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
    if (!t)
        return MNTNS_INIT_INO;
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    const mnt_namespace_t *ns = (const mnt_namespace_t *)t->mnt_ns;
    uint64_t ino = ns ? ns->ino : MNTNS_INIT_INO;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    return ino;
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
    if (g_shutdown_active) {
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        kfree(ns);
        return NULL;
    }
    g_namespace_create_inflight++;
    ns->ino = g_next_ino++;
    ns->next = g_ns_list;
    g_ns_list = ns;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    return ns;
}

/* Deep-copy a stable snapshot of the mount table: root vnodes and fs_data
 * remain shared, but each copied table entry pins the original source mount.
 * The source entry owns filesystem teardown and cannot be reclaimed while
 * child namespaces hold these explicit pins. The mount-table lock serializes
 * this snapshot against teardown admission and table removal. Entries already
 * being torn down are omitted from the new snapshot, while copied entries
 * release their source pin only when their namespace is destroyed, including
 * when the entry already sits on that namespace's graveyard list.
 */
static void mntns_copy_mounts(mnt_namespace_t *dst, mnt_namespace_t *src)
{
    mount_t *sources[MNTNS_MAX_MOUNTS];
    int count = 0;
    uint64_t flags = mntns_mount_table_lock();
    for (int i = 0; i < src->nmounts; i++) {
        mount_t *entry = src->mounts[i];
        if (!entry || !entry->publish_ready || entry->teardown_running)
            continue;
        mount_t *source = entry->shared_source ? entry->shared_source : entry;
        __atomic_add_fetch(&source->ns_users, 1, __ATOMIC_RELAXED);
#ifdef CONFIG_MNTNS_TEST
        if (strcmp(entry->path, "/tmp/ns_shared") == 0)
            printf("MNTNS_TEST: COPY source_ns=%llu source_users=%d\n",
                   (unsigned long long)src->ino,
                   __atomic_load_n(&source->ns_users, __ATOMIC_ACQUIRE));
#endif
        sources[count++] = source;
    }
    mntns_mount_table_unlock(flags);

    for (int i = 0; i < count; i++) {
        mount_t *mnt = (mount_t *)kmalloc(sizeof(*mnt));
        if (!mnt) {
            /* Preserve the historical partial-copy behavior, but return all
             * source pins for entries that did not make it into the table. */
            for (int j = i; j < count; j++)
                __atomic_sub_fetch(&sources[j]->ns_users, 1, __ATOMIC_ACQ_REL);
            dst->nmounts = i;
            return;
        }
        *mnt = *sources[i];
        mnt->dead_next = NULL;
        mnt->ns_users = 0;  /* copies never run the filesystem destructor */
        mnt->flags |= VFS_MOUNT_NS_SHARED;
        mnt->shared_source = sources[i];
        mnt->teardown_running = 0;
        dst->mounts[i] = mnt;
    }
    dst->nmounts = count;
}

int mntns_unshare(task_t *t)
{
    if (!t)
        return -ESRCH;
    mnt_namespace_t *cur = t->mnt_ns ? (mnt_namespace_t *)t->mnt_ns
                                     : &g_init_ns;
    mnt_namespace_t *ns = mntns_alloc();
    if (!ns)
        return mntns_shutdown_active() ? -ESHUTDOWN : -ENOMEM;
    mntns_copy_mounts(ns, cur);
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    if (g_shutdown_active) {
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        mntns_create_done();
        mntns_put(ns);
        return -ESHUTDOWN;
    }
    mnt_namespace_t *old = (mnt_namespace_t *)t->mnt_ns;
    t->mnt_ns = ns;  /* takes over the fresh reference */
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    mntns_create_done();
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
            return mntns_shutdown_active() ? -ESHUTDOWN : -ENOMEM;
        mntns_copy_mounts(ns, pns);
        uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
        if (g_shutdown_active) {
            spin_unlock_irqrestore(&g_mntns_lock, flags);
            mntns_create_done();
            mntns_put(ns);
            return -ESHUTDOWN;
        }
        child->mnt_ns = ns;
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        mntns_create_done();
        return 0;
    }
    if (pns == &g_init_ns) {
        uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
        if (g_shutdown_active) {
            spin_unlock_irqrestore(&g_mntns_lock, flags);
            return -ESHUTDOWN;
        }
        child->mnt_ns = NULL;  /* initial namespace: no reference needed */
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        return 0;
    }
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    if (g_shutdown_active) {
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        return -ESHUTDOWN;
    }
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
    if (g_shutdown_active) {
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        return -ESHUTDOWN;
    }
    mnt_namespace_t *old = (mnt_namespace_t *)t->mnt_ns;
    /* NULL means the initial namespace; its reference is not tracked. */
    t->mnt_ns = to_init ? NULL : ns;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    if (to_init) {
        /* The initial namespace is statically pinned, but changing membership
         * still releases the caller's reference to the old namespace. */
        if (old)
            mntns_put(old);
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
    /* Reserve one internal reference before dropping the registry lock.  If
     * filesystem teardown fails (for example a journal flush error), this
     * pin keeps every mount/root pointer valid until retry succeeds. */
    refcount_set(&ns->refs, 1);
    ns->teardown_pending = 1;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    int teardown = mntns_retry_pending_internal(1);
    uint64_t retry_flags = spin_lock_irqsave(&g_mntns_lock);
    int pending = 0;
    for (mnt_namespace_t *it = g_ns_list; it; it = it->next) {
        if (it != &g_init_ns && it->teardown_pending &&
            it->teardown_retryable) {
            pending = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&g_mntns_lock, retry_flags);
    if (pending && teardown != -EBUSY)
        mntns_start_reaper();
}

static int mntns_retry_pending_internal(int retry_busy)
{
    int result = 0;
    int retry_after_shared_drop = 0;
    int allow_busy_retry = retry_busy;
    uint64_t flags = spin_lock_irqsave(&g_mntns_lock);
    uint64_t pass = ++g_teardown_pass;
    if (pass == 0)
        pass = ++g_teardown_pass;
    spin_unlock_irqrestore(&g_mntns_lock, flags);
    for (;;) {
        flags = spin_lock_irqsave(&g_mntns_lock);
        mnt_namespace_t *ns = NULL;
        int released_shared_source = 0;
        for (mnt_namespace_t *it = g_ns_list; it; it = it->next) {
            if (it != &g_init_ns && it->teardown_pending &&
                !it->teardown_running &&
                it->teardown_attempt_pass != pass &&
                (allow_busy_retry || it->teardown_retryable)) {
                it->teardown_running = 1;
                it->teardown_attempt_pass = pass;
                ns = it;
                for (int i = 0; i < it->nmounts; i++)
                    released_shared_source |= it->mounts[i] &&
                                              it->mounts[i]->shared_source;
                for (mount_t *mnt = it->dead_mounts; mnt;
                     mnt = mnt->dead_next)
                    released_shared_source |= mnt->shared_source != NULL;
                break;
            }
        }
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        if (!ns && retry_after_shared_drop) {
            /* A child namespace's pins can be the only thing keeping a
             * previously EBUSY source alive. Retry one new pass after that
             * child has actually completed and released its shared entries.
             * Each namespace is attempted at most once per pass, so a
             * permanent destructor failure cannot spin here. */
            retry_after_shared_drop = 0;
            allow_busy_retry = 1;
            flags = spin_lock_irqsave(&g_mntns_lock);
            pass = ++g_teardown_pass;
            if (pass == 0)
                pass = ++g_teardown_pass;
            spin_unlock_irqrestore(&g_mntns_lock, flags);
            continue;
        }
        if (!ns)
            return result;

#ifdef CONFIG_MNTNS_TEST
        if (mntns_test_tracks_shared_path(ns))
            printf("MNTNS_TEST: RETRY ino=%llu refs=%u pass=%llu busy=%d\n",
                   (unsigned long long)ns->ino, refcount_read(&ns->refs),
                   (unsigned long long)pass, allow_busy_retry);
#endif
        int ret = vfs_mount_namespace_teardown(ns);
#ifdef CONFIG_MNTNS_TEST
        printf("MNTNS_TEST: RETRY_RESULT ino=%llu result=%d tracks=%d nmounts=%d\n",
               (unsigned long long)ns->ino, ret,
               mntns_test_tracks_shared_path(ns), ns->nmounts);
#endif
        flags = spin_lock_irqsave(&g_mntns_lock);
        ns->teardown_running = 0;
        if (ret == 0) {
            ns->teardown_pending = 0;
            ns->teardown_retryable = 0;
            mntns_remove_locked(ns);
            refcount_set(&ns->refs, 0);
        } else {
            ns->teardown_retryable = ret != -EBUSY;
        }
        spin_unlock_irqrestore(&g_mntns_lock, flags);
        if (ret == 0) {
            mntns_free(ns);
            retry_after_shared_drop |= released_shared_source;
        } else if (result == 0 || (result == -EBUSY && ret != -EBUSY))
            result = ret;
    }
}

void mntns_retry_pending(void)
{
    (void)mntns_retry_pending_internal(1);
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
