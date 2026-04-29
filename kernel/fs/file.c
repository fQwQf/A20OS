#include "fs/file.h"

#include "core/consts.h"
#include "core/lock.h"
#include "core/string.h"
#include "mm/objcache.h"

#define GFILE_MAX VFS_MAX_OPEN
#define GFILE_WORDS ((GFILE_MAX + 63) / 64)

static vfile_t *g_files[GFILE_MAX];
static uint64_t g_file_mask[GFILE_WORDS];
static int g_file_next = 3;
static spinlock_t g_file_lock = SPINLOCK_INIT;
static obj_cache_t g_vfile_cache = OBJ_CACHE_INIT("vfile", vfile_t, 256);

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
    spin_init(&g_file_lock);
    obj_cache_init(&g_vfile_cache, "vfile", sizeof(vfile_t), 256);
    memset(g_files, 0, sizeof(g_files));
    memset(g_file_mask, 0, sizeof(g_file_mask));
    g_file_next = 3;
}

vfile_t *vfile_alloc(void)
{
    return (vfile_t *)obj_cache_alloc_zero(&g_vfile_cache);
}

void vfile_free(vfile_t *vf)
{
    obj_cache_free(&g_vfile_cache, vf);
}

int file_install_at(int fd, vfile_t *vf)
{
    if (fd < 0 || fd >= GFILE_MAX || !vf)
        return -EBADF;
    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    if (g_files[fd]) {
        spin_unlock_irqrestore(&g_file_lock, flags);
        return -EBUSY;
    }
    g_files[fd] = vf;
    file_note_alloc(fd);
    spin_unlock_irqrestore(&g_file_lock, flags);
    return fd;
}

int vfs_alloc_fd(vfile_t *vf)
{
    if (!vf) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    int gfd = file_find_free_from(g_file_next);
    if (gfd < 0)
        gfd = file_find_free_from(3);
    if (gfd >= 0) {
        g_files[gfd] = vf;
        file_note_alloc(gfd);
    }
    spin_unlock_irqrestore(&g_file_lock, flags);
    return gfd;
}

vfile_t *vfs_get_file(int fd)
{
    if (fd < 0 || fd >= GFILE_MAX) return NULL;
    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    vfile_t *vf = g_files[fd];
    spin_unlock_irqrestore(&g_file_lock, flags);
    return vf;
}

vfile_t *vfs_get_file_ref(int fd)
{
    if (fd < 0 || fd >= GFILE_MAX)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    vfile_t *vf = g_files[fd];
    if (vf)
        refcount_inc(&vf->ref_count);
    spin_unlock_irqrestore(&g_file_lock, flags);
    return vf;
}

void vfs_put_file_ref(int fd, vfile_t *vf)
{
    if (!vf)
        return;
    (void)vfs_close(fd);
}

int vfs_ref_fd(int fd)
{
    if (fd < 0 || fd >= GFILE_MAX)
        return -EBADF;
    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    vfile_t *vf = g_files[fd];
    if (!vf) {
        spin_unlock_irqrestore(&g_file_lock, flags);
        return -EBADF;
    }
    refcount_inc(&vf->ref_count);
    spin_unlock_irqrestore(&g_file_lock, flags);
    return 0;
}

int file_close_prepare(int fd, vfile_t **closed)
{
    if (closed) *closed = NULL;
    if (fd < 0 || fd >= GFILE_MAX) return -EBADF;

    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    vfile_t *vf = g_files[fd];
    if (!vf) {
        spin_unlock_irqrestore(&g_file_lock, flags);
        return -EBADF;
    }

    if (refcount_dec_and_test(&vf->ref_count)) {
        g_files[fd] = NULL;
        file_note_free(fd);
        if (closed) *closed = vf;
    }
    spin_unlock_irqrestore(&g_file_lock, flags);
    return 0;
}

int vfs_dupfd(int fd, int minfd)
{
    if (minfd < 0) minfd = 0;
    uint64_t flags = spin_lock_irqsave(&g_file_lock);
    if (fd < 0 || fd >= GFILE_MAX || !g_files[fd]) {
        spin_unlock_irqrestore(&g_file_lock, flags);
        return -EBADF;
    }
    vfile_t *vf = g_files[fd];
    refcount_inc(&vf->ref_count);
    int newfd = file_find_free_from(minfd);
    if (newfd >= 0) {
        g_files[newfd] = vf;
        file_note_alloc(newfd);
        spin_unlock_irqrestore(&g_file_lock, flags);
        return newfd;
    }
    refcount_dec_and_test(&vf->ref_count);
    spin_unlock_irqrestore(&g_file_lock, flags);
    return -EMFILE;
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

    uint64_t irqflags = spin_lock_irqsave(&g_file_lock);
    if (oldfd < 0 || oldfd >= GFILE_MAX || !g_files[oldfd]) {
        spin_unlock_irqrestore(&g_file_lock, irqflags);
        return -EBADF;
    }
    if (g_files[newfd]) {
        spin_unlock_irqrestore(&g_file_lock, irqflags);
        return -EBUSY;
    }
    g_files[newfd] = g_files[oldfd];
    refcount_inc(&g_files[newfd]->ref_count);
    file_note_alloc(newfd);
    spin_unlock_irqrestore(&g_file_lock, irqflags);
    return newfd;
}
