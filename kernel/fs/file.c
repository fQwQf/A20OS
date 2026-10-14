#if !defined(CONFIG_MCU) || defined(CONFIG_NATIVE_FLAT_FILES)
#include "fs/file.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "core/fcntl.h"

#include "core/consts.h"
#include "core/lock.h"
#include "core/lock_counters.h"
#include "core/sync.h"
#include "core/string.h"
#include "mm/objcache.h"
#include "ipc/objstats.h"
#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
#include "ipc/envelope.h"
#endif

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
    refcount_set(&vf->ref_count, refs);
}

void vfile_get(vfile_t *vf)
{
    if (!vf)
        return;
    refcount_inc(&vf->ref_count);
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
    return refcount_dec_and_test(&vf->ref_count);
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
