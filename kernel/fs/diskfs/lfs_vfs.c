/*
 * A20OS — littlefs VFS adapter.
 *
 * Mounts a littlefs v2 filesystem (kernel/external/littlefs, vendored
 * upstream) on any block_dev_t-backed bcache.  littlefs is a power-loss
 * resilient filesystem designed for MCUs: every commit is metadata+data
 * journalled into pair blocks, so a crash at any point leaves the previous
 * consistent state intact.  That is the property the embedded segment needs
 * and none of FAT/ext4(no journal)/NTFS provide.
 *
 * Block geometry: block_size == PCACHE_PAGE_SIZE so the bcache byte API and
 * littlefs blocks stay page-aligned; prog == write (block storage needs no
 * erase), erase is a no-op, sync forwards to the device flush (fsync
 * durability reaches stable media — see block_dev.h flush).
 *
 * Inodes: littlefs has no stable inode numbers, so ino is the FNV-1a hash of
 * the path within the mount (root == 1).  Stable per name, never reused for
 * a different path while mounted — good enough for dcache keys and /proc
 * inode display, and honestly documented here.
 *
 * Paths: vnode_t carries no path text, so each littlefs vnode owns a
 * lfs_node_t (fs_data) with its mount-relative path; release frees it.
 *
 * No symlinks and no extended attributes (the upstream public API does not
 * expose them); the VFS table reports that fail-closed like FAT32.
 */
#include "fs/vfs.h"
#include "fs/lfs_vfs.h"
#include "fs/block_cache.h"
#include "fs/file.h"
#include "fs/xattr.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/fcntl.h"
#include "mm/slab.h"
/* Declare the allocator bridge before lfs.h pulls lfs_util.h in: the
 * generic kernel include order resolves <stdlib.h> to the lwIP port's
 * shim, which knows nothing about littlefs. */
#include "compat/stdlib.h"
#include "lfs.h"

#define LFS_VFS_BLOCK_SIZE PCACHE_PAGE_SIZE

typedef struct lfs_mount {
    lfs_t      lfs;
    /* littlefs keeps lfs->cfg as a POINTER, so the config must outlive the
     * mount — it lives here, never on a caller's stack. */
    struct lfs_config cfg;
    bcache_t  *bc;
    uint64_t   blocks;
} lfs_mount_t;

/* Per-vnode state: the mount plus this node's mount-relative path. */
typedef struct lfs_node {
    lfs_mount_t *m;
    char         rel[MAX_PATH_LEN];
    vnode_t     *vn;      /* back-pointer so children can ref their parent */
} lfs_node_t;

typedef struct lfs_file_priv {
    lfs_file_t   file;
    lfs_node_t  *node;
} lfs_file_priv_t;

static vnode_ops_t g_lfs_vnode_ops;
static vfile_ops_t g_lfs_file_ops;

/* ---- ino helpers ---- */

static uint64_t lfs_ino_hash(const char *rel)
{
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = rel; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ULL;
    }
    /* never 0 (0 reads as "no inode"); 1 is reserved for the root dir */
    h |= 1ULL << 32;
    return h;
}

/* rel-of(parent) + "/" + name into out */
static int lfs_child_rel(const lfs_node_t *dir, const char *name,
                         char *out, size_t outsz)
{
    int n;
    if (strcmp(dir->rel, "/") == 0)
        n = snprintf(out, outsz, "/%s", name);
    else
        n = snprintf(out, outsz, "%s/%s", dir->rel, name);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

/* ---- lfs block device callbacks (bcache-backed) ---- */

static int lfs_bd_read(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, void *buffer, lfs_size_t size)
{
    lfs_mount_t *m = (lfs_mount_t *)c->context;
    int r = bcache_read_bytes(m->bc,
                              (uint64_t)block * LFS_VFS_BLOCK_SIZE + off,
                              buffer, size);
    return r < 0 ? LFS_ERR_IO : 0;
}

static int lfs_bd_prog(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, const void *buffer, lfs_size_t size)
{
    lfs_mount_t *m = (lfs_mount_t *)c->context;
    int r = bcache_write_bytes(m->bc,
                               (uint64_t)block * LFS_VFS_BLOCK_SIZE + off,
                               buffer, size);
    return r < 0 ? LFS_ERR_IO : 0;
}

static int lfs_bd_erase(const struct lfs_config *c, lfs_block_t block)
{
    (void)c;
    (void)block;
    /* Block storage keeps prog-overwrite semantics; erase is a no-op. */
    return 0;
}

static int lfs_bd_sync(const struct lfs_config *c)
{
    lfs_mount_t *m = (lfs_mount_t *)c->context;
    bcache_sync(m->bc);
    return 0;
}

/* ---- vnode helpers ---- */

static lfs_node_t *lfs_node_of(vnode_t *vn)
{
    return vn && vn->fs_data ? (lfs_node_t *)vn->fs_data : NULL;
}

static void lfs_fill_vnode(vnode_t *vn, const struct lfs_info *info,
                           uint64_t ino)
{
    vn->ino = ino;
    switch (info->type) {
    case LFS_TYPE_DIR:
        vn->type = VFS_FT_DIR;
        vn->mode = S_IFDIR | 0755;
        break;
    default:
        vn->type = VFS_FT_REGULAR;
        vn->mode = S_IFREG | 0644;
        break;
    }
    vn->size = info->size;
}

static vnode_t *lfs_make_vnode(lfs_node_t *dir, const char *name,
                               const struct lfs_info *info, uint32_t mode)
{
    vnode_t *vn = kcalloc(1, sizeof(vnode_t));
    if (!vn)
        return NULL;
    lfs_node_t *nn = kcalloc(1, sizeof(*nn));
    if (!nn) {
        kfree(vn);
        return NULL;
    }
    nn->m = dir->m;
    if (lfs_child_rel(dir, name, nn->rel, sizeof(nn->rel)) < 0) {
        kfree(nn);
        kfree(vn);
        return NULL;
    }

    vnode_ref_init(vn, 1);
    vnode_get(dir->vn); /* child holds a reference on its parent */
    vn->parent = dir->vn;
    nn->vn = vn;
    vn->ops = &g_lfs_vnode_ops;
    vn->fs_data = nn;
    lfs_fill_vnode(vn, info, lfs_ino_hash(nn->rel));
    if (mode & S_IFMT)
        vn->mode = (vn->mode & S_IFMT) | (mode & 07777u);
    return vn;
}

static int lfs_vfs_lookup(vnode_t *dir, const char *name, vnode_t **out)
{
    lfs_node_t *dn = lfs_node_of(dir);
    if (!dn || !name || !*name || strcmp(name, "..") == 0)
        return -ENOENT;

    char rel[MAX_PATH_LEN];
    if (lfs_child_rel(dn, name, rel, sizeof(rel)) < 0)
        return -ENAMETOOLONG;
    struct lfs_info info;
    if (lfs_stat(&dn->m->lfs, rel, &info) < 0)
        return -ENOENT;

    vnode_t *vn = lfs_make_vnode(dn, name, &info, 0);
    if (!vn)
        return -ENOMEM;
    *out = vn;
    return 0;
}

static int lfs_vfs_create(vnode_t *dir, const char *name, int mode,
                          vnode_t **out)
{
    lfs_node_t *dn = lfs_node_of(dir);
    if (!dn)
        return -ENXIO;
    if ((mode & S_IFMT) && (mode & S_IFMT) != S_IFREG)
        return -EPERM; /* only regular files are creatable here */

    char rel[MAX_PATH_LEN];
    if (lfs_child_rel(dn, name, rel, sizeof(rel)) < 0)
        return -ENAMETOOLONG;
    lfs_file_t f;
    int r = lfs_file_open(&dn->m->lfs, &f, rel,
                          LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL);
    if (r < 0)
        return r == LFS_ERR_EXIST ? -EEXIST : -EIO;
    struct lfs_info info;
    memset(&info, 0, sizeof(info));
    info.type = LFS_TYPE_REG;
    info.size = 0;
    vnode_t *vn = lfs_make_vnode(dn, name, &info, (uint32_t)mode);
    lfs_file_close(&dn->m->lfs, &f);
    if (!vn)
        return -ENOMEM;
    *out = vn;
    return 0;
}

static int lfs_vfs_mkdir(vnode_t *dir, const char *name, int mode)
{
    (void)mode;
    lfs_node_t *dn = lfs_node_of(dir);
    if (!dn)
        return -ENXIO;
    char rel[MAX_PATH_LEN];
    if (lfs_child_rel(dn, name, rel, sizeof(rel)) < 0)
        return -ENAMETOOLONG;
    int r = lfs_mkdir(&dn->m->lfs, rel);
    if (r < 0)
        return r == LFS_ERR_EXIST ? -EEXIST : -EIO;
    return 0;
}

/* unlink/rmdir share littlefs' remove(3) semantics; empty-directory
 * enforcement for rmdir is done here explicitly. */
static int lfs_remove_rel(lfs_node_t *dn, const char *name, int is_rmdir)
{
    char rel[MAX_PATH_LEN];
    if (lfs_child_rel(dn, name, rel, sizeof(rel)) < 0)
        return -ENAMETOOLONG;
    struct lfs_info info;
    if (lfs_stat(&dn->m->lfs, rel, &info) < 0)
        return -ENOENT;
    if (is_rmdir && info.type == LFS_TYPE_DIR) {
        lfs_dir_t d;
        if (lfs_dir_open(&dn->m->lfs, &d, rel) < 0)
            return -EIO;
        int r = lfs_dir_read(&dn->m->lfs, &d, &info);
        lfs_dir_close(&dn->m->lfs, &d);
        if (r > 0)
            return -ENOTEMPTY;
    }
    return lfs_remove(&dn->m->lfs, rel) < 0 ? -EIO : 0;
}

static int lfs_vfs_unlink(vnode_t *dir, const char *name)
{
    lfs_node_t *dn = lfs_node_of(dir);
    if (!dn)
        return -ENXIO;
    return lfs_remove_rel(dn, name, 0);
}

static int lfs_vfs_rmdir(vnode_t *dir, const char *name)
{
    lfs_node_t *dn = lfs_node_of(dir);
    if (!dn)
        return -ENXIO;
    return lfs_remove_rel(dn, name, 1);
}

static int lfs_vfs_readdir(vfile_t *vf, void *dirp, size_t count)
{
    vnode_t *vn = vf->vnode;
    lfs_node_t *dn = lfs_node_of(vn);
    if (!dn)
        return -ENXIO;
    if (vn->type != VFS_FT_DIR)
        return -ENOTDIR;

    lfs_dir_t d;
    if (lfs_dir_open(&dn->m->lfs, &d, dn->rel) < 0)
        return -EIO;

    /* VFS readdir contract: variable-length vfs_dirent64_t records
     * (d_reclen = offsetof(d_name) + strlen + 1, 8-aligned), packed until
     * @count is full — same wire format ext4 delivers. */
    char *out = (char *)dirp;
    size_t total = 0;
    struct lfs_info info;
    while (lfs_dir_read(&dn->m->lfs, &d, &info) > 0) {
        size_t nl = strlen(info.name);
        size_t reclen = offsetof(vfs_dirent64_t, d_name) + nl + 1;
        reclen = (reclen + 7) & ~7UL;
        if (total + reclen > count)
            break;
        vfs_dirent64_t *ent = (vfs_dirent64_t *)(out + total);
        memset(ent, 0, reclen);
        memcpy(ent->d_name, info.name, nl + 1);
        ent->d_type = info.type == LFS_TYPE_DIR ? VFS_FT_DIR : VFS_FT_REGULAR;
        char rel2[MAX_PATH_LEN];
        if (lfs_child_rel(dn, info.name, rel2, sizeof(rel2)) == 0)
            ent->d_ino = lfs_ino_hash(rel2);
        ent->d_reclen = (uint16_t)reclen;
        total += reclen;
    }
    lfs_dir_close(&dn->m->lfs, &d);
    return (int)total;
}

static int lfs_vfs_truncate(vnode_t *vn, size_t size)
{
    lfs_node_t *dn = lfs_node_of(vn);
    if (!dn)
        return -ENXIO;
    lfs_file_t f;
    if (lfs_file_open(&dn->m->lfs, &f, dn->rel, LFS_O_WRONLY) < 0)
        return -EIO;
    int r = lfs_file_truncate(&dn->m->lfs, &f, size);
    lfs_file_close(&dn->m->lfs, &f);
    if (r < 0)
        return -EIO;
    vn->size = size;
    return 0;
}

static int lfs_vfs_stat(vnode_t *vn, kstat_t *st)
{
    if (!st)
        return -EINVAL;
    memset(st, 0, sizeof(*st));
    st->st_ino = vn->ino;
    st->st_mode = vn->mode;
    st->st_nlink = 1;
    st->st_size = (long long)vn->size;
    st->st_blksize = LFS_VFS_BLOCK_SIZE;
    st->st_blocks = (vn->size + LFS_VFS_BLOCK_SIZE - 1) / LFS_VFS_BLOCK_SIZE;
    st->st_uid = vn->uid;
    st->st_gid = vn->gid;
    return 0;
}

static int lfs_vfs_sync_vnode(vnode_t *vn)
{
    /* Files commit through their open vfile (ops->sync); the mount-level
     * device flush happens in the generic fsync path. */
    (void)vn;
    return 0;
}

static void lfs_vfs_release(vnode_t *vn)
{
    lfs_node_t *nn = lfs_node_of(vn);
    if (nn) {
        kfree(nn);
        vn->fs_data = NULL;
    }
}

static vfile_t *lfs_vfs_open(vnode_t *vn, int flags);

static vnode_ops_t g_lfs_vnode_ops = {
    .lookup     = lfs_vfs_lookup,
    .create     = lfs_vfs_create,
    .mkdir      = lfs_vfs_mkdir,
    .unlink     = lfs_vfs_unlink,
    .rmdir      = lfs_vfs_rmdir,
    .truncate   = lfs_vfs_truncate,
    .stat       = lfs_vfs_stat,
    .sync_vnode = lfs_vfs_sync_vnode,
    .open       = lfs_vfs_open,
    .release    = lfs_vfs_release,
};

/* ---- vfile (open file) ops ---- */

static vfile_t *lfs_vfs_open(vnode_t *vn, int flags)
{
    lfs_node_t *dn = lfs_node_of(vn);
    if (!dn)
        return NULL;

    lfs_file_priv_t *fp = kcalloc(1, sizeof(*fp));
    if (!fp)
        return NULL;
    fp->node = dn;

    int lf = 0;
    switch (flags & O_ACCMODE) {
    case O_RDONLY: lf = LFS_O_RDONLY; break;
    case O_WRONLY: lf = LFS_O_WRONLY; break;
    default:       lf = LFS_O_RDWR;   break;
    }
    if (flags & O_CREAT)    lf |= LFS_O_CREAT;
    if (flags & O_EXCL)     lf |= LFS_O_EXCL;
    if (flags & O_TRUNC)    lf |= LFS_O_TRUNC;
    if (flags & O_APPEND)   lf |= LFS_O_APPEND;

    if (lfs_file_open(&dn->m->lfs, &fp->file, dn->rel, lf) < 0) {
        kfree(fp);
        return NULL;
    }

    vfile_t *vf = vfile_alloc();
    if (!vf) {
        lfs_file_close(&dn->m->lfs, &fp->file);
        kfree(fp);
        return NULL;
    }
    /* The fd slot adopts this reference (fdtable_install_vfile transfers),
     * so the vfile starts life at exactly one. */
    vfile_ref_init(vf, 1);
    vf->ops = &g_lfs_file_ops;
    vf->priv = fp;
    vf->vnode = vn;
    vnode_get(vn);
    vf->flags = flags & (O_ACCMODE | O_APPEND | O_NONBLOCK | O_DIRECT);
    vf->offset = (flags & O_APPEND) ? (long)vn->size : 0;
    return vf;
}

static int lfs_vfs_read(vfile_t *vf, char *buf, size_t count)
{
    lfs_file_priv_t *fp = vf->priv;
    if (!fp)
        return -EBADF;
    mutex_lock(&vf->offset_lock);
    lfs_ssize_t n = lfs_file_read(&fp->node->m->lfs, &fp->file, buf, count);
    mutex_unlock(&vf->offset_lock);
    if (n < 0)
        return -EIO;
    vf->offset = (long)lfs_file_tell(&fp->node->m->lfs, &fp->file);
    return (int)n;
}

static int lfs_vfs_write(vfile_t *vf, const char *buf, size_t count)
{
    lfs_file_priv_t *fp = vf->priv;
    if (!fp)
        return -EBADF;
    mutex_lock(&vf->offset_lock);
    lfs_ssize_t n = lfs_file_write(&fp->node->m->lfs, &fp->file, buf, count);
    mutex_unlock(&vf->offset_lock);
    if (n < 0)
        return -EIO;
    vf->offset = (long)lfs_file_tell(&fp->node->m->lfs, &fp->file);
    vnode_t *vn = vf->vnode;
    lfs_soff_t sz = lfs_file_size(&fp->node->m->lfs, &fp->file);
    if (vn && sz >= 0 && (size_t)sz > vn->size)
        vn->size = (size_t)sz;
    return (int)n;
}

static long lfs_vfs_lseek(vfile_t *vf, long offset, int whence)
{
    lfs_file_priv_t *fp = vf->priv;
    if (!fp)
        return -EBADF;
    mutex_lock(&vf->offset_lock);
    lfs_soff_t n = lfs_file_seek(&fp->node->m->lfs, &fp->file, offset,
                                 whence);
    mutex_unlock(&vf->offset_lock);
    if (n < 0)
        return -EINVAL;
    vf->offset = (long)n;
    return (long)n;
}

static int lfs_vfs_sync(vfile_t *vf)
{
    lfs_file_priv_t *fp = vf->priv;
    if (!fp)
        return -EBADF;
    /* lfs_file_sync commits the file's metadata+data pairs; the config sync
     * hook then flushes the device. */
    return lfs_file_sync(&fp->node->m->lfs, &fp->file) < 0 ? -EIO : 0;
}

static int lfs_vfs_close(vfile_t *vf)
{
    lfs_file_priv_t *fp = vf->priv;
    if (!fp)
        return 0;
    lfs_file_close(&fp->node->m->lfs, &fp->file);
    kfree(fp);
    vf->priv = NULL;
    return 0;
}

static vfile_ops_t g_lfs_file_ops = {
    .read    = lfs_vfs_read,
    .write   = lfs_vfs_write,
    .lseek   = lfs_vfs_lseek,
    .readdir = lfs_vfs_readdir,
    .sync    = lfs_vfs_sync,
    .close   = lfs_vfs_close,
};

/* Kernel-allocator bridge for the vendored littlefs tree (see
 * compat/stdlib.h).  Defined here so the vendored sources never include
 * kernel headers. */
void *lfs_kmalloc(unsigned long size) { return kmalloc((size_t)size); }
void  lfs_kfree(void *ptr) { kfree(ptr); }
void *lfs_krealloc(void *ptr, unsigned long size) {
    /* littlefs never reallocs in v2.9; present for the shim's sake. */
    (void)ptr;
    return kmalloc((size_t)size);
}

/* GCC libgcc helpers littlefs' builtins expand to.  The kernel links
 * -nostdlib and deliberately avoids libgcc (see fdtable.c's software ctz),
 * so these live here as plain software implementations. */
unsigned int __bswapsi2(unsigned int v)
{
    return ((v & 0x000000ffU) << 24) | ((v & 0x0000ff00U) << 8) |
           ((v & 0x00ff0000U) >> 8)  | ((v & 0xff000000U) >> 24);
}

unsigned long __bswapdi2(unsigned long v)
{
    return ((v & 0x00000000000000ffUL) << 56) |
           ((v & 0x000000000000ff00UL) << 40) |
           ((v & 0x0000000000ff0000UL) << 24) |
           ((v & 0x00000000ff000000UL) << 8)  |
           ((v & 0x000000ff00000000UL) >> 8)  |
           ((v & 0x0000ff0000000000UL) >> 24) |
           ((v & 0x00ff000000000000UL) >> 40) |
           ((v & 0xff00000000000000UL) >> 56);
}

int __ctzdi2(unsigned long v)
{
    if (!v)
        return 64;
    int n = 0;
    if ((v & 0xFFFFFFFF) == 0) { n += 32; v >>= 32; }
    if ((v & 0xFFFF) == 0)     { n += 16; v >>= 16; }
    if ((v & 0xFF) == 0)       { n += 8;  v >>= 8;  }
    if ((v & 0xF) == 0)        { n += 4;  v >>= 4;  }
    if ((v & 0x3) == 0)        { n += 2;  v >>= 2;  }
    if ((v & 0x1) == 0)        { n += 1; }
    return n;
}

int __clzdi2(unsigned long v)
{
    if (!v)
        return 64;
    int n = 0;
    if ((v & 0xFFFFFFFF00000000UL) == 0) { n += 32; v <<= 32; }
    if ((v & 0xFFFF000000000000UL) == 0) { n += 16; v <<= 16; }
    if ((v & 0xFF00000000000000UL) == 0) { n += 8;  v <<= 8;  }
    if ((v & 0xF000000000000000UL) == 0) { n += 4;  v <<= 4;  }
    if ((v & 0xC000000000000000UL) == 0) { n += 2;  v <<= 2;  }
    if ((v & 0x8000000000000000UL) == 0) { n += 1; }
    return n;
}

/* String functions littlefs needs that core/string.c does not provide. */
unsigned long strcspn(const char *s, const char *reject)
{
    unsigned long n = 0;
    for (; s[n]; n++) {
        for (const char *r = reject; *r; r++)
            if (s[n] == *r)
                return n;
    }
    return n;
}

unsigned long strspn(const char *s, const char *accept)
{
    unsigned long n = 0;
    for (; s[n]; n++) {
        int found = 0;
        for (const char *a = accept; *a; a++)
            if (s[n] == *a) {
                found = 1;
                break;
            }
        if (!found)
            return n;
    }
    return n;
}

/* ---- mount / unmount ---- */

vnode_t *littlefs_mount(struct bcache *bc)
{
    if (!bc || !bc->dev)
        return NULL;

    uint64_t cap_bytes = (uint64_t)bc->dev->capacity * bc->dev->sector_size;
    if (cap_bytes < 4 * LFS_VFS_BLOCK_SIZE) {
        kerr("[LFS] device too small: %lu bytes\n",
             (unsigned long)cap_bytes);
        return NULL;
    }

    lfs_mount_t *m = kcalloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->bc = bc;
    m->blocks = cap_bytes / LFS_VFS_BLOCK_SIZE;

    struct lfs_config *cfg = &m->cfg;
    memset(cfg, 0, sizeof(*cfg));
    cfg->context = m;
    cfg->read  = lfs_bd_read;
    cfg->prog  = lfs_bd_prog;
    cfg->erase = lfs_bd_erase;
    cfg->sync  = lfs_bd_sync;
    cfg->read_size  = LFS_VFS_BLOCK_SIZE;
    cfg->prog_size  = LFS_VFS_BLOCK_SIZE;
    cfg->block_size = LFS_VFS_BLOCK_SIZE;
    cfg->block_count = (uint32_t)m->blocks;
    cfg->block_cycles = 500;   /* irrelevant with erase no-op; keeps compaction honest */
    cfg->cache_size  = LFS_VFS_BLOCK_SIZE;
    cfg->lookahead_size = 8 * LFS_VFS_BLOCK_SIZE;
    cfg->name_max = MAX_NAME_LEN - 1;

    int r = lfs_mount(&m->lfs, cfg);
    if (r < 0) {
        kerr("[LFS] mount failed: %d (unformatted image?)\n", r);
        kfree(m);
        return NULL;
    }

    vnode_t *root = kcalloc(1, sizeof(vnode_t));
    lfs_node_t *nn = root ? kcalloc(1, sizeof(*nn)) : NULL;
    if (!root || !nn) {
        lfs_unmount(&m->lfs);
        kfree(m);
        if (root) kfree(root);
        if (nn) kfree(nn);
        return NULL;
    }
    nn->m = m;
    nn->vn = root;
    strcpy(nn->rel, "/");
    vnode_ref_init(root, 1);
    root->type = VFS_FT_DIR;
    root->mode = S_IFDIR | 0755;
    root->ino = 1;
    root->ops = &g_lfs_vnode_ops;
    root->fs_data = nn;
    return root;
}

void littlefs_unmount(vnode_t *root)
{
    if (!root)
        return;
    lfs_node_t *nn = lfs_node_of(root);
    if (nn) {
        lfs_unmount(&nn->m->lfs);
        bcache_sync(nn->m->bc);
        kfree(nn->m);
        kfree(nn);
        root->fs_data = NULL;
    }
}
