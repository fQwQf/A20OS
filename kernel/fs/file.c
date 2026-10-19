#if !defined(CONFIG_MCU) || defined(CONFIG_NATIVE_FLAT_FILES)
#include "fs/file.h"
#include "fs/vfs.h"
#include "fs/vfs/shutdown.h"
#include "fs/vfs/mount.h"
#include "fs/vfs/mntns.h"
#include "fs/fdtable.h"
#include "core/fcntl.h"

#include "core/consts.h"
#include "core/lock.h"
#include "core/lock_counters.h"
#include "core/sync.h"
#include "core/string.h"
#ifdef CONFIG_UFS_CLOSE_TOKEN_SHUTDOWN_TRACE
#include "core/klog.h"
#endif
#include "mm/objcache.h"
#include "ipc/objstats.h"
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
#include "ipc/envelope.h"
#endif

extern int a20_vfile_close_begin(uint32_t *state);
extern int a20_vfile_close_begin_token(uint32_t *state, int token_capable);
extern int a20_vfile_close_finish(uint32_t *state, int status, int sent,
                                  int acked, int retry_safe);
extern int a20_vfile_close_is_done(const uint32_t *state);


/*
 * The vfile layer owns vfile allocation, identity and refcounting only.
 * There is no global file-number table: an fd is an index into a task's
 * files_struct (fs/fdtable.c), and every fd resolution goes through
 * fdtable_get_current_file_ref().  The former g_files[]/g_file_lock layer
 * (and its bucket locks) was removed with the per-task pointer model.
 */

#ifdef CONFIG_NATIVE_FLAT_FILES
#define VFILE_CACHE_LIMIT 0
#else
#define VFILE_CACHE_LIMIT 256
#endif
static obj_cache_t g_vfile_cache =
    OBJ_CACHE_INIT("vfile", vfile_t, VFILE_CACHE_LIMIT);
static size_t g_vfile_live;
static uint64_t g_vfile_next_identity;
static spinlock_t g_vfile_registry_lock = SPINLOCK_INIT;
static vfile_t *g_vfile_registry;
#define VFILE_SHUTDOWN_MAX VFS_MAX_OPEN
static vfile_t *g_vfile_shutdown_snapshot[VFILE_SHUTDOWN_MAX];
static int g_vfile_shutdown_count;
static int g_vfile_shutdown_snapshot_active;
static unsigned int g_vfile_finalizers_inflight;

/* Identity keys may remain in authorization tables until final vfile release.
 * Fail closed at exhaustion instead of wrapping and aliasing a live object. */
static int vfile_allocate_identity(uint64_t *identity)
{
    uint64_t current = __atomic_load_n(&g_vfile_next_identity,
                                       __ATOMIC_RELAXED);
    for (;;) {
        if (current == ~(uint64_t)0)
            return 0;
        uint64_t next = current + 1;
        if (__atomic_compare_exchange_n(&g_vfile_next_identity, &current,
                                        next, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
            *identity = next;
            return 1;
        }
    }
}

void file_table_init(void)
{
    obj_cache_init(&g_vfile_cache, "vfile", sizeof(vfile_t), VFILE_CACHE_LIMIT);
    g_vfile_live = 0;
    g_vfile_next_identity = 0;
    g_vfile_registry = NULL;
    g_vfile_shutdown_count = 0;
    g_vfile_shutdown_snapshot_active = 0;
    g_vfile_finalizers_inflight = 0;
}

size_t vfile_live_count(void)
{
    return __atomic_load_n(&g_vfile_live, __ATOMIC_RELAXED);
}

vfile_t *vfile_alloc(void)
{
    vfile_t *vf = (vfile_t *)obj_cache_alloc_zero(&g_vfile_cache);
    if (vf) {
        if (!vfile_allocate_identity(&vf->identity)) {
            obj_cache_free(&g_vfile_cache, vf);
            return NULL;
        }
        /* obj_cache_alloc_zero() already guarantees this, but make the
         * identity-bound envelope default explicit at object creation. */
        __atomic_store_n(&vf->envelope_kind, 0, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_vfile_live, 1, __ATOMIC_RELAXED);
        a20_objstat_add(&g_a20_objstats.vfiles, 1);
        mutex_init(&vf->offset_lock);
        mutex_init(&vf->backend_close_lock);
        vf->lease = F_UNLCK;
        /* Refcount arrives at 0 (the cache hands out zeroed memory) and the
         * producer owns the first reference it is about to hand away.  A
         * producer that publishes the vfile straight into an fd slot MUST
         * call vfile_ref_init(vf, 1): skipping it leaves every later
         * get/put pair balanced around 1 -> 0, so the first syscall that
         * resolves the fd finalizes the file underneath the slot. */
        vfile_ref_init(vf, 1);
    }
    return vf;
}

void vfile_free(vfile_t *vf)
{
    if (vf) {
        uint64_t flags = spin_lock_irqsave(&g_vfile_registry_lock);
        if (vf->shutdown_registered) {
            vfile_t **link = &g_vfile_registry;
            while (*link && *link != vf)
                link = &(*link)->shutdown_next;
            if (*link == vf)
                *link = vf->shutdown_next;
            vf->shutdown_next = NULL;
            vf->shutdown_registered = 0;
        }
        spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
        /* Authorization shadows are keyed by this unique open-file identity.
         * Reclaim them before the storage/identity can be reused. */
        env_vfile_retire(vf);
#endif
        __atomic_fetch_sub(&g_vfile_live, 1, __ATOMIC_RELAXED);
        a20_objstat_add(&g_a20_objstats.vfiles, -1);
    }
    obj_cache_free(&g_vfile_cache, vf);
}

void vfile_ref_init(vfile_t *vf, int refs)
{
    if (!vf)
        return;
    uint64_t flags = spin_lock_irqsave(&g_vfile_registry_lock);
    refcount_set(&vf->ref_count, refs);
    if (refs > 0) {
        if (!vf->shutdown_registered) {
            vf->shutdown_next = g_vfile_registry;
            g_vfile_registry = vf;
            vf->shutdown_registered = 1;
        }
    }
    spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
}

void vfile_get(vfile_t *vf)
{
    if (!vf)
        return;
    (void)refcount_inc_not_zero(&vf->ref_count);
}

int vfile_ref_read(vfile_t *vf)
{
    if (!vf)
        return 0;
    return refcount_read(&vf->ref_count);
}

int vfile_put_ref_only(vfile_t *vf)
{
    if (!vf)
        return 0;
    for (;;) {
        int refs = refcount_read(&vf->ref_count);
        if (refs <= 0)
            return 0;
        if (refs > 1) {
            int expected = refs;
            if (__atomic_compare_exchange_n(&vf->ref_count.value, &expected,
                                            refs - 1, 0, __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED))
                return 0;
            continue;
        }
        /* Serialize only the 1->0 transition with registry snapshots.  The
         * common ref get/put path remains atomic and lock-free. */
        uint64_t flags = spin_lock_irqsave(&g_vfile_registry_lock);
        for (;;) {
            refs = refcount_read(&vf->ref_count);
            if (refs > 1) {
                int expected = refs;
                if (__atomic_compare_exchange_n(&vf->ref_count.value,
                                                &expected, refs - 1, 0,
                                                __ATOMIC_ACQ_REL,
                                                __ATOMIC_RELAXED)) {
                    spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
                    return 0;
                }
                continue;
            }
            if (refs == 1) {
                int expected = 1;
                if (!__atomic_compare_exchange_n(&vf->ref_count.value,
                                                 &expected, 0, 0,
                                                 __ATOMIC_ACQ_REL,
                                                 __ATOMIC_RELAXED))
                    continue;
                g_vfile_finalizers_inflight++;
                spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
                return 1;
            }
            spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
            return 0;
        }
    }
}

void vfile_finalize_done(void)
{
    uint64_t flags = spin_lock_irqsave(&g_vfile_registry_lock);
    if (g_vfile_finalizers_inflight)
        g_vfile_finalizers_inflight--;
    spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
}

int vfile_shutdown_snapshot_begin(void)
{
    uint64_t flags = spin_lock_irqsave(&g_vfile_registry_lock);
    if (g_vfile_shutdown_snapshot_active) {
        spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
        return -EBUSY;
    }
    if (g_vfile_finalizers_inflight) {
        spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
        return -EBUSY;
    }
    int count = 0;
    for (vfile_t *vf = g_vfile_registry; vf; vf = vf->shutdown_next) {
        if (!vf->mount_open_counted || refcount_read(&vf->ref_count) == 0)
            continue;
        if (count == VFILE_SHUTDOWN_MAX) {
            spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
            return -EOVERFLOW;
        }
        count++;
    }
    count = 0;
    for (vfile_t *vf = g_vfile_registry; vf; vf = vf->shutdown_next) {
        if (!vf->mount_open_counted || refcount_read(&vf->ref_count) == 0)
            continue;
        refcount_inc(&vf->ref_count);
        g_vfile_shutdown_snapshot[count++] = vf;
    }
    g_vfile_shutdown_count = count;
    g_vfile_shutdown_snapshot_active = 1;
    spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
#ifdef CONFIG_UFS_CLOSE_TOKEN_SHUTDOWN_TRACE
    kinfo("[UFS-CT-TRACE] snapshot count=%d\n", count);
    for (int i = 0; i < count; i++) {
        vfile_t *vf = g_vfile_shutdown_snapshot[i];
        if (!vf) continue;
        kinfo("[UFS-CT-TRACE] snapshot vf=%lu refs=%d mounted=%u deferred=%u token_cap=%u close_state=%u\n",
              (unsigned long)vf->identity, refcount_read(&vf->ref_count),
              (unsigned)vf->mount_open_counted,
              (unsigned)vf->shutdown_deferred_close,
              (unsigned)vf->close_token_capable, vf->backend_close_state);
    }
#endif
    return count;
}

vfile_t *vfile_shutdown_snapshot_at(int index)
{
    uint64_t flags = spin_lock_irqsave(&g_vfile_registry_lock);
    vfile_t *vf = NULL;
    if (g_vfile_shutdown_snapshot_active && index >= 0 &&
        index < g_vfile_shutdown_count)
        vf = g_vfile_shutdown_snapshot[index];
    spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
    return vf; /* The snapshot owns a reference until end(). */
}

void vfile_shutdown_snapshot_end(void)
{
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_vfile_registry_lock);
        if (!g_vfile_shutdown_snapshot_active) {
            spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
            return;
        }
        if (g_vfile_shutdown_count == 0) {
            g_vfile_shutdown_snapshot_active = 0;
            spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
            return;
        }
        int index = --g_vfile_shutdown_count;
        vfile_t *vf = g_vfile_shutdown_snapshot[index];
        g_vfile_shutdown_snapshot[index] = NULL;
        spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
        vfs_put_file(vf);
    }
}

int vfile_backend_close(vfile_t *vf)
{
    if (!vf) return -EINVAL;
    mutex_lock(&vf->backend_close_lock);
    int begin = a20_vfile_close_begin_token(&vf->backend_close_state,
                                            vf->close_token_capable);
    if (begin == -EALREADY || a20_vfile_close_is_done(&vf->backend_close_state)) {
        vf->backend_close_done = 1;
        mutex_unlock(&vf->backend_close_lock);
        return 0;
    }
    if (begin < 0) {
        mutex_unlock(&vf->backend_close_lock);
        return begin;
    }
#ifdef CONFIG_UFS_CLOSE_TOKEN_SHUTDOWN_TRACE
    kinfo("[UFS-CT-TRACE] backend_close begin vf=%lu refs=%d mounted=%u deferred=%u token_cap=%u close_state=%u\n",
          (unsigned long)vf->identity, refcount_read(&vf->ref_count),
          (unsigned)vf->mount_open_counted,
          (unsigned)vf->shutdown_deferred_close,
          (unsigned)vf->close_token_capable, vf->backend_close_state);
#endif
    vf->close_attempt_sent = 0;
    vf->close_attempt_acked = 0;
    vf->close_attempt_retry_safe = 0;
    int status = vf->ops && vf->ops->close ? vf->ops->close(vf) : 0;
    if (status == 0 && !vf->close_attempt_acked)
        vf->close_attempt_acked = 1; /* local backend close completed */
    int ret = a20_vfile_close_finish(&vf->backend_close_state, status,
                                    vf->close_attempt_sent,
                                    vf->close_attempt_acked,
                                    vf->close_attempt_retry_safe);
    if (ret == 0)
        vf->backend_close_done = 1;
#ifdef CONFIG_UFS_CLOSE_TOKEN_SHUTDOWN_TRACE
    kinfo("[UFS-CT-TRACE] backend_close end vf=%lu status=%d sent=%u acked=%u retry_safe=%u ret=%d done=%u state=%u\n",
          (unsigned long)vf->identity, status,
          (unsigned)vf->close_attempt_sent,
          (unsigned)vf->close_attempt_acked,
          (unsigned)vf->close_attempt_retry_safe, ret,
          (unsigned)vf->backend_close_done, vf->backend_close_state);
#endif
    mutex_unlock(&vf->backend_close_lock);
    return ret;
}

int vfile_shutdown_close_mounted(void)
{
    uint64_t flags = spin_lock_irqsave(&g_vfile_registry_lock);
    int count = g_vfile_shutdown_snapshot_active ? g_vfile_shutdown_count : -1;
    spin_unlock_irqrestore(&g_vfile_registry_lock, flags);
    if (count < 0)
        return -EINVAL;
    for (int i = 0; i < count; i++) {
        vfile_t *vf = vfile_shutdown_snapshot_at(i);
        if (!vf)
            return -EIO;
#ifdef CONFIG_UFS_CLOSE_TOKEN_SHUTDOWN_TRACE
        kinfo("[UFS-CT-TRACE] shutdown_close vf=%lu mounted=%u deferred=%u done=%u state=%u\n",
              (unsigned long)vf->identity,
              (unsigned)vf->mount_open_counted,
              (unsigned)vf->shutdown_deferred_close,
              (unsigned)vf->backend_close_done,
              vf->backend_close_state);
#endif
        if (!vf->mount_open_counted || vf->backend_close_done)
            continue;
        int ret = vfile_backend_close(vf);
        if (ret < 0) {
            vf->shutdown_deferred_close = 1;
            return ret;
        }
        if (vf->mount_open_counted && vf->vnode && vf->vnode->mnt) {
            vf->mount_open_counted = 0;
            vfs_mount_vfile_put(vf->vnode->mnt);
        }
        if (vf->shutdown_deferred_close) {
            vf->shutdown_deferred_close = 0;
            /* The descriptor was already consumed by an earlier close;
             * this is its private retained reference. The snapshot pin keeps
             * the object alive until end(). */
            vfs_put_file(vf);
        }
    }
    return 0;
}

/*
 * fd resolution.  The signatures predate the per-task pointer model and are
 * kept so the whole tree keeps one fd vocabulary: @fd is the calling task's
 * fd, resolved through its files_struct.
 */
vfile_t *vfs_get_file_ref(int fd)
{
    return fdtable_get_current_file_ref(fd);
}

void vfs_put_file_ref(int fd, vfile_t *vf)
{
    (void)fd;
    vfs_put_file(vf);
}

void vfs_put_file(vfile_t *vf)
{
    if (!vf)
        return;
    if (vfile_put_ref_only(vf))
        vfs_finalize_closed_vfile(vf);
}

#endif
