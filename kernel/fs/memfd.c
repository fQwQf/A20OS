#include "fs/memfd.h"

#include "core/consts.h"
#include "core/string.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/slab.h"

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} memfd_file_t;

static int memfd_file_grow(memfd_file_t *mf, size_t need)
{
    if (need <= mf->cap) return 0;
    size_t cap = mf->cap ? mf->cap : 4096;
    while (cap < need) cap *= 2;
    uint8_t *data = kmalloc(cap);
    if (!data) return -ENOMEM;
    memset(data, 0, cap);
    if (mf->data) {
        memcpy(data, mf->data, mf->size);
        kfree(mf->data);
    }
    mf->data = data;
    mf->cap = cap;
    return 0;
}

static int memfd_file_read(vfile_t *vf, char *buf, size_t count)
{
    memfd_file_t *mf = vf ? vf->priv : NULL;
    if (!mf) return -EBADF;
    if (vf->offset >= mf->size) return 0;
    size_t n = mf->size - vf->offset;
    if (n > count) n = count;
    memcpy(buf, mf->data + vf->offset, n);
    vf->offset += n;
    return (int)n;
}

static int memfd_file_write(vfile_t *vf, const char *buf, size_t count)
{
    memfd_file_t *mf = vf ? vf->priv : NULL;
    if (!mf) return -EBADF;
    int r = memfd_file_grow(mf, vf->offset + count);
    if (r < 0) return r;
    memcpy(mf->data + vf->offset, buf, count);
    vf->offset += count;
    if (vf->offset > mf->size) mf->size = vf->offset;
    if (vf->vnode) vf->vnode->size = mf->size;
    return (int)count;
}

static long memfd_file_lseek(vfile_t *vf, long offset, int whence)
{
    memfd_file_t *mf = vf ? vf->priv : NULL;
    if (!mf) return -EBADF;
    long base = 0;
    if (whence == SEEK_CUR) base = (long)vf->offset;
    else if (whence == SEEK_END) base = (long)mf->size;
    else if (whence != SEEK_SET) return -EINVAL;
    long pos = base + offset;
    if (pos < 0) return -EINVAL;
    vf->offset = (size_t)pos;
    return pos;
}

static int memfd_file_close(vfile_t *vf)
{
    memfd_file_t *mf = vf ? vf->priv : NULL;
    if (mf) {
        if (mf->data) kfree(mf->data);
        kfree(mf);
        vf->priv = NULL;
    }
    return 0;
}

static int memfd_file_stat(vnode_t *vn, kstat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = vn ? vn->ino : 0;
    st->st_mode = S_IFREG | 0777;
    st->st_nlink = 1;
    st->st_size = vn ? vn->size : 0;
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    return 0;
}

static int memfd_file_truncate(vnode_t *vn, size_t size)
{
    if (!vn) return -EINVAL;
    memfd_file_t *mf = vn->fs_data;
    if (!mf) return -EINVAL;
    int r = memfd_file_grow(mf, size);
    if (r < 0) return r;
    if (size > mf->size) memset(mf->data + mf->size, 0, size - mf->size);
    mf->size = size;
    vn->size = size;
    return 0;
}

static void memfd_file_release(vnode_t *vn)
{
    if (vn) kfree(vn);
}

static vfile_ops_t g_memfile_fops = {
    .read = memfd_file_read,
    .write = memfd_file_write,
    .lseek = memfd_file_lseek,
    .close = memfd_file_close,
};

static vnode_ops_t g_memfile_vops = {
    .stat = memfd_file_stat,
    .truncate = memfd_file_truncate,
    .release = memfd_file_release,
};

int memfd_create_file(int flags)
{
    if (flags & ~(O_CLOEXEC | 0x3U | 0x4U)) return -EINVAL;
    memfd_file_t *mf = kmalloc(sizeof(*mf));
    vnode_t *vn = kmalloc(sizeof(*vn));
    vfile_t *vf = vfile_alloc();
    if (!mf || !vn || !vf) {
        if (mf) kfree(mf);
        if (vn) kfree(vn);
        if (vf) vfile_free(vf);
        return -ENOMEM;
    }
    memset(mf, 0, sizeof(*mf));
    memset(vn, 0, sizeof(*vn));
    vn->ino = (uint64_t)(uintptr_t)vn;
    vn->type = VFS_FT_REGULAR;
    vn->mode = S_IFREG | 0777;
    vnode_ref_init(vn, 1);
    vn->ops = &g_memfile_vops;
    vn->fs_data = mf;
    vf->vnode = vn;
    vf->flags = O_RDWR;
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_memfile_fops;
    vf->priv = mf;
    if (!(flags & 0x2U))
        vf->seals = F_SEAL_SEAL;
    return anonfd_install_vfile(vf, flags);
}
