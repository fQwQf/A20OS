#include "fs/anonfd.h"

#include "core/consts.h"
#include "core/string.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "mm/slab.h"

int anonfd_free_priv_close(vfile_t *vf)
{
    if (vf && vf->priv) {
        kfree(vf->priv);
        vf->priv = NULL;
    }
    return 0;
}
int anonfd_install_vfile(vfile_t *vf, int flags)
{
    int gfd = vfs_alloc_fd(vf);
    if (gfd < 0) {
        if (vf->ops && vf->ops->close)
            vf->ops->close(vf);
        vfile_free(vf);
        return -EMFILE;
    }
    return fdtable_install_current(gfd, flags);
}

/* ---- generic anonymous in-memory file (staging buffer) ---- */

typedef struct anon_buf {
    uint8_t *data;
    size_t size;
    size_t cap;
} anon_buf_t;

static int anon_buf_grow(anon_buf_t *ab, size_t need)
{
    if (need <= ab->cap)
        return 0;
    size_t cap = ab->cap ? ab->cap : 4096;
    while (cap < need)
        cap *= 2;
    uint8_t *data = kmalloc(cap);
    if (!data)
        return -ENOMEM;
    if (ab->data) {
        memcpy(data, ab->data, ab->size);
        kfree(ab->data);
    }
    ab->data = data;
    ab->cap = cap;
    return 0;
}

static int anon_buf_read(vfile_t *vf, char *buf, size_t count)
{
    anon_buf_t *ab = vf ? vf->priv : NULL;
    if (!ab)
        return -EBADF;
    if (vf->offset >= ab->size)
        return 0;
    size_t n = ab->size - vf->offset;
    if (n > count)
        n = count;
    memcpy(buf, ab->data + vf->offset, n);
    vf->offset += n;
    return (int)n;
}

static int anon_buf_write(vfile_t *vf, const char *buf, size_t count)
{
    anon_buf_t *ab = vf ? vf->priv : NULL;
    if (!ab)
        return -EBADF;
    int r = anon_buf_grow(ab, vf->offset + count);
    if (r < 0)
        return r;
    memcpy(ab->data + vf->offset, buf, count);
    vf->offset += count;
    if (vf->offset > ab->size)
        ab->size = vf->offset;
    return (int)count;
}

static long anon_buf_lseek(vfile_t *vf, long offset, int whence)
{
    anon_buf_t *ab = vf ? vf->priv : NULL;
    if (!ab)
        return -EBADF;
    long base = 0;
    if (whence == SEEK_CUR)
        base = (long)vf->offset;
    else if (whence == SEEK_END)
        base = (long)ab->size;
    else if (whence != SEEK_SET)
        return -EINVAL;
    long pos = base + offset;
    if (pos < 0)
        return -EINVAL;
    vf->offset = (size_t)pos;
    return pos;
}

static int anon_buf_close(vfile_t *vf)
{
    anon_buf_t *ab = vf ? vf->priv : NULL;
    if (ab) {
        if (ab->data)
            kfree(ab->data);
        kfree(ab);
        vf->priv = NULL;
    }
    return 0;
}

static vfile_ops_t g_anon_buf_ops = {
    .read = anon_buf_read,
    .write = anon_buf_write,
    .lseek = anon_buf_lseek,
    .close = anon_buf_close,
};

int anonfd_create(int flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK))
        return -EINVAL;
    anon_buf_t *ab = kcalloc(1, sizeof(*ab));
    vfile_t *vf = vfile_alloc();
    if (!ab || !vf) {
        if (ab) kfree(ab);
        if (vf) vfile_free(vf);
        return -ENOMEM;
    }
    vf->flags = O_RDWR;
    vfile_ref_init(vf, 1);
    vf->ops = &g_anon_buf_ops;
    vf->priv = ab;
    return anonfd_install_vfile(vf, flags);
}
