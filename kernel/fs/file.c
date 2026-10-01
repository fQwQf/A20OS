#include "fs/file.h"
#include "core/fcntl.h"

#include "core/consts.h"
#include "core/lock.h"
#include "core/lock_counters.h"
#include "core/sync.h"
#include "core/string.h"
#include "mm/objcache.h"
#include "ipc/objstats.h"

#define GFILE_MAX VFS_MAX_OPEN
#define GFILE_WORDS ((GFILE_MAX + 63) / 64)

static vfile_t *g_files[GFILE_MAX];
static uint32_t g_file_slot_refs[GFILE_MAX];
static uint64_t g_file_mask[GFILE_WORDS];
static int g_file_next = 3;

/* The table was guarded by one global spinlock, which serialized every
 * get/ref/close of every fd across all CPUs behind the same lock — the fd
 * hot path (read/write/close resolve their fd here on every syscall).  The
 * per-slot state now lives under one of VFILE_BUCKET_COUNT bucket locks
 * picked by a multiplicative hash of the gfd, so unrelated fds proceed in
 * parallel; the free bitmap and the allocation cursor get their own lock.
 *
 * Lock order: bucket lock -> g_file_alloc_lock (only file_close_prepare's
 * note-free nests them).  No path takes a bucket lock while holding the
 * alloc lock: allocators reserve the slot under the alloc lock, release it,
 * then install under the bucket lock — a reserved-but-uninstalled slot is
 * unreachable because its gfd is only published after installation. */
#define VFILE_BUCKET_BITS 7
#define VFILE_BUCKET_COUNT (1u << VFILE_BUCKET_BITS)
static spinlock_t g_file_bucket_locks[VFILE_BUCKET_COUNT];
static spinlock_t g_file_alloc_lock = SPINLOCK_INIT;

static inline spinlock_t *file_bucket_lock(int fd)
{
    uint32_t h = (uint32_t)fd * 2654435761u;
    return &g_file_bucket_locks[h >> (32 - VFILE_BUCKET_BITS)];
}
static obj_cache_t g_vfile_cache = OBJ_CACHE_INIT("vfile", vfile_t, 256);
static size_t g_vfile_live;
static uint64_t g_vfile_next_identity;

extern void a20_eventq_on_vfile_destroy(int fd);

static inline void file_mask_set(int fd)
{
    g_file_mask[fd >> 6] |= 1ULL << (fd & 63);
}

static inline void file_mask_clear(int fd)
{
    g_file_mask[fd >> 6] &= ~(1ULL << (fd & 63));
}

static int file_ctz64(uint64_t bits)
{
    int n = 0;
    while ((bits & 1ULL) == 0) {
        bits >>= 1;
        n++;
    }
    return n;
}

static int file_find_free_from(int minfd)
{
    if (minfd < 0)
        minfd = 0;
    if (minfd >= GFILE_MAX)
        return -1;
    for (int word = minfd >> 6; word < GFILE_WORDS; word++) {
        uint64_t free_bits = ~g_file_mask[word];
        if (word == (minfd >> 6))
            free_bits &= ~0ULL << (minfd & 63);
        if (word == GFILE_WORDS - 1 && (GFILE_MAX & 63))
            free_bits &= (1ULL << (GFILE_MAX & 63)) - 1;
        if (free_bits)
            return (word << 6) + file_ctz64(free_bits);
    }
    return -1;
}

static void file_note_alloc(int fd)
{
    file_mask_set(fd);
    if (fd >= g_file_next)
        g_file_next = file_find_free_from(fd + 1);
}

static void file_note_free(int fd)
{
    file_mask_clear(fd);
    if (g_file_next < 0 || fd < g_file_next)
        g_file_next = fd;
}

void file_table_init(void)
{
    for (uint32_t i = 0; i < VFILE_BUCKET_COUNT; i++) {
        spin_init(&g_file_bucket_locks[i]);
        lock_counters_register(&g_file_bucket_locks[i], "vfile_bucket");
    }
    spin_init(&g_file_alloc_lock);
    lock_counters_register(&g_file_alloc_lock, "vfile_alloc");
    obj_cache_init(&g_vfile_cache, "vfile", sizeof(vfile_t), 256);
    memset(g_files, 0, sizeof(g_files));
    memset(g_file_slot_refs, 0, sizeof(g_file_slot_refs));
    memset(g_file_mask, 0, sizeof(g_file_mask));
    g_file_next = 3;
    g_vfile_live = 0;
    g_vfile_next_identity = 0;
}

size_t file_open_fd_count(void)
{
    size_t count = 0;
    uint64_t flags = spin_lock_irqsave(&g_file_alloc_lock);
    for (int word = 0; word < GFILE_WORDS; word++)
        count += (size_t)__builtin_popcountll(g_file_mask[word]);
    spin_unlock_irqrestore(&g_file_alloc_lock, flags);
    return count;
}

size_t vfile_live_count(void)
{
    return __atomic_load_n(&g_vfile_live, __ATOMIC_RELAXED);
}

vfile_t *vfile_alloc(void)
{
    vfile_t *vf = (vfile_t *)obj_cache_alloc_zero(&g_vfile_cache);
    if (vf) {
        vf->identity = __atomic_add_fetch(&g_vfile_next_identity, 1,
                                          __ATOMIC_RELAXED);
        if (!vf->identity)
            vf->identity = __atomic_add_fetch(&g_vfile_next_identity, 1,
                                              __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_vfile_live, 1, __ATOMIC_RELAXED);
        a20_objstat_add(&g_a20_objstats.vfiles, 1);
        mutex_init(&vf->offset_lock);
        vf->lease = F_UNLCK;
    }
    return vf;
}

void vfile_free(vfile_t *vf)
{
    if (vf) {
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

int file_install_at(int fd, vfile_t *vf)
{
    if (fd < 0 || fd >= GFILE_MAX || !vf)
        return -EBADF;
    spinlock_t *bucket = file_bucket_lock(fd);
    uint64_t flags = spin_lock_irqsave(bucket);
    if (g_files[fd]) {
        spin_unlock_irqrestore(bucket, flags);
        return -EBUSY;
    }
    g_files[fd] = vf;
    g_file_slot_refs[fd] = 1;
    spin_unlock_irqrestore(bucket, flags);
    uint64_t alloc_flags = spin_lock_irqsave(&g_file_alloc_lock);
    file_note_alloc(fd);
    spin_unlock_irqrestore(&g_file_alloc_lock, alloc_flags);
    return fd;
}

int vfs_alloc_fd(vfile_t *vf)
{
    if (!vf) return -EINVAL;
    uint64_t alloc_flags = spin_lock_irqsave(&g_file_alloc_lock);
    int gfd = file_find_free_from(g_file_next);
    if (gfd < 0)
        gfd = file_find_free_from(3);
    if (gfd >= 0)
        file_note_alloc(gfd);
    spin_unlock_irqrestore(&g_file_alloc_lock, alloc_flags);
    if (gfd < 0)
        return -EMFILE;
    /* The reserved gfd is not published anywhere until the install below
     * completes, so no reader can observe the in-between state. */
    spinlock_t *bucket = file_bucket_lock(gfd);
    uint64_t flags = spin_lock_irqsave(bucket);
    g_files[gfd] = vf;
    g_file_slot_refs[gfd] = 1;
    spin_unlock_irqrestore(bucket, flags);
    return gfd;
}

vfile_t *vfs_get_file(int fd)
{
    if (fd < 0 || fd >= GFILE_MAX) return NULL;
    spinlock_t *bucket = file_bucket_lock(fd);
    uint64_t flags = spin_lock_irqsave(bucket);
    vfile_t *vf = g_files[fd];
    spin_unlock_irqrestore(bucket, flags);
    return vf;
}

vfile_t *vfs_get_file_ref(int fd)
{
    if (fd < 0 || fd >= GFILE_MAX)
        return NULL;
    spinlock_t *bucket = file_bucket_lock(fd);
    uint64_t flags = spin_lock_irqsave(bucket);
    vfile_t *vf = g_files[fd];
    if (vf)
        vfile_get(vf);
    spin_unlock_irqrestore(bucket, flags);
    return vf;
}

int vfs_ref_fd(int fd)
{
    if (fd < 0 || fd >= GFILE_MAX)
        return -EBADF;
    spinlock_t *bucket = file_bucket_lock(fd);
    uint64_t flags = spin_lock_irqsave(bucket);
    if (!g_files[fd] || g_file_slot_refs[fd] == ~(uint32_t)0) {
        spin_unlock_irqrestore(bucket, flags);
        return -EBADF;
    }
    g_file_slot_refs[fd]++;
    spin_unlock_irqrestore(bucket, flags);
    return 0;
}

int file_close_prepare(int fd, vfile_t **closed)
{
    if (closed) *closed = NULL;
    if (fd < 0 || fd >= GFILE_MAX) return -EBADF;

    spinlock_t *bucket = file_bucket_lock(fd);
    uint64_t flags = spin_lock_irqsave(bucket);
    vfile_t *vf = g_files[fd];
    if (!vf) {
        spin_unlock_irqrestore(bucket, flags);
        return -EBADF;
    }

    if (--g_file_slot_refs[fd] == 0) {
        a20_eventq_on_vfile_destroy(fd);
        g_files[fd] = NULL;
        uint64_t alloc_flags = spin_lock_irqsave(&g_file_alloc_lock);
        file_note_free(fd);
        spin_unlock_irqrestore(&g_file_alloc_lock, alloc_flags);
        if (vfile_put_ref_only(vf) && closed)
            *closed = vf;
    }
    spin_unlock_irqrestore(bucket, flags);
    return 0;
}

int file_put_ref_prepare(int fd, vfile_t *vf, vfile_t **closed)
{
    if (closed)
        *closed = NULL;
    (void)fd;
    if (!vf)
        return -EBADF;
    if (vfile_put_ref_only(vf)) {
        if (closed)
            *closed = vf;
    }
    return 0;
}

int vfs_dupfd(int fd, int minfd)
{
    if (minfd < 0) minfd = 0;
    if (fd < 0 || fd >= GFILE_MAX)
        return -EBADF;
    uint64_t alloc_flags = spin_lock_irqsave(&g_file_alloc_lock);
    int newfd = file_find_free_from(minfd);
    if (newfd >= 0)
        file_note_alloc(newfd);
    spin_unlock_irqrestore(&g_file_alloc_lock, alloc_flags);
    if (newfd < 0)
        return -EMFILE;

    /* Bucket(old) first to pin the vfile with a reference, then install into
     * the reserved newfd's bucket; no two bucket locks are ever held. */
    spinlock_t *src = file_bucket_lock(fd);
    uint64_t flags = spin_lock_irqsave(src);
    vfile_t *vf = g_files[fd];
    if (vf)
        vfile_get(vf);
    spin_unlock_irqrestore(src, flags);
    if (!vf) {
        uint64_t f = spin_lock_irqsave(&g_file_alloc_lock);
        file_note_free(newfd);
        spin_unlock_irqrestore(&g_file_alloc_lock, f);
        return -EBADF;
    }

    spinlock_t *dst = file_bucket_lock(newfd);
    flags = spin_lock_irqsave(dst);
    g_files[newfd] = vf;
    g_file_slot_refs[newfd] = 1;
    spin_unlock_irqrestore(dst, flags);
    return newfd;
}

int vfs_dup(int fd)
{
    return vfs_dupfd(fd, 3);
}

int vfs_dup3(int oldfd, int newfd, int flags)
{
    (void)flags;
    if (newfd >= GFILE_MAX || newfd < 0) return -EBADF;
    if (oldfd == newfd) return -EINVAL;
    if (oldfd < 0 || oldfd >= GFILE_MAX) return -EBADF;

    /* Pin the source vfile under its bucket, then take the destination
     * bucket; the vfile reference keeps the source alive even if oldfd is
     * closed concurrently, which POSIX leaves either way. */
    spinlock_t *src = file_bucket_lock(oldfd);
    uint64_t irqflags = spin_lock_irqsave(src);
    vfile_t *vf = g_files[oldfd];
    if (vf)
        vfile_get(vf);
    spin_unlock_irqrestore(src, irqflags);
    if (!vf)
        return -EBADF;

    spinlock_t *dst = file_bucket_lock(newfd);
    irqflags = spin_lock_irqsave(dst);
    if (g_files[newfd]) {
        spin_unlock_irqrestore(dst, irqflags);
        /* vfs_put_file, not a bare put: oldfd may have been closed
         * concurrently, making this the last reference, and the finalizer
         * must then run. */
        vfs_put_file(vf);
        return -EBUSY;
    }
    g_files[newfd] = vf;
    g_file_slot_refs[newfd] = 1;
    spin_unlock_irqrestore(dst, irqflags);
    uint64_t alloc_flags = spin_lock_irqsave(&g_file_alloc_lock);
    file_note_alloc(newfd);
    spin_unlock_irqrestore(&g_file_alloc_lock, alloc_flags);
    return newfd;
}
