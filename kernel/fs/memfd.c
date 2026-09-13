#include "fs/memfd.h"

#include "core/consts.h"
#include "core/string.h"
#include "fs/anonfd.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/page_cache.h"
#include "fs/vfs.h"
#include "fs/vfs/stat_perm.h"
#include "mm/frame.h"
#include "mm/slab.h"

typedef struct {
    pfn_t *pages;
    size_t npages;
    size_t cap;
    size_t size;
    mutex_t data_lock;
    int secret;
    int owner_euid;
} memfd_file_t;

static uint8_t *memfd_page_ptr(memfd_file_t *mf, size_t index)
{
    if (index >= mf->npages || mf->pages[index] == PFN_NONE)
        return NULL;
    return (uint8_t *)pfn_to_virt(mf->pages[index]);
}

static int memfd_file_grow(memfd_file_t *mf, size_t need)
{
    size_t need_pages = (need + PAGE_SIZE - 1) / PAGE_SIZE;
    if (need_pages <= mf->npages)
        return 0;
    if (need_pages > mf->cap) {
        size_t cap = mf->cap ? mf->cap : 16;
        while (cap < need_pages)
            cap *= 2;
        pfn_t *pages = kmalloc(cap * sizeof(pfn_t));
        if (!pages)
            return -ENOMEM;
        for (size_t i = 0; i < mf->npages; i++)
            pages[i] = mf->pages[i];
        for (size_t i = mf->npages; i < cap; i++)
            pages[i] = PFN_NONE;
        if (mf->pages)
            kfree(mf->pages);
        mf->pages = pages;
        mf->cap = cap;
    }
    for (size_t i = mf->npages; i < need_pages; i++) {
        pfn_t p = pfa_alloc_page();
        if (p == PFN_NONE)
            return -ENOMEM;
        memset(pfn_to_virt(p), 0, PAGE_SIZE);
        mf->pages[i] = p;
        mf->npages = i + 1;
    }
    return 0;
}

static int memfd_file_read(vfile_t *vf, char *buf, size_t count)
{
    memfd_file_t *mf = vf ? vf->priv : NULL;
    if (!mf) return -EBADF;
    mutex_lock(&mf->data_lock);
    if (vf->offset >= mf->size) {
        mutex_unlock(&mf->data_lock);
        return 0;
    }
    size_t n = mf->size - vf->offset;
    if (n > count) n = count;
    size_t done = 0;
    while (done < n) {
        size_t off = vf->offset + done;
        size_t poff = off % PAGE_SIZE;
        size_t chunk = PAGE_SIZE - poff;
        if (chunk > n - done) chunk = n - done;
        uint8_t *p = memfd_page_ptr(mf, off / PAGE_SIZE);
        if (p)
            memcpy(buf + done, p + poff, chunk);
        else
            memset(buf + done, 0, chunk);
        done += chunk;
    }
    vf->offset += n;
    mutex_unlock(&mf->data_lock);
    return (int)n;
}

static int memfd_file_write(vfile_t *vf, const char *buf, size_t count)
{
    memfd_file_t *mf = vf ? vf->priv : NULL;
    if (!mf) return -EBADF;
    mutex_lock(&mf->data_lock);
    int r = memfd_file_grow(mf, vf->offset + count);
    if (r < 0) {
        mutex_unlock(&mf->data_lock);
        return r;
    }
    size_t done = 0;
    while (done < count) {
        size_t off = vf->offset + done;
        size_t poff = off % PAGE_SIZE;
        size_t chunk = PAGE_SIZE - poff;
        if (chunk > count - done) chunk = count - done;
        uint8_t *p = memfd_page_ptr(mf, off / PAGE_SIZE);
        if (p)
            memcpy(p + poff, buf + done, chunk);
        done += chunk;
    }
    vf->offset += count;
    if (vf->offset > mf->size) mf->size = vf->offset;
    if (vf->vnode) vf->vnode->size = mf->size;
    mutex_unlock(&mf->data_lock);
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
        for (size_t i = 0; i < mf->npages; i++)
            if (mf->pages[i] != PFN_NONE)
                pfa_free(mf->pages[i], 0);
        if (mf->pages)
            kfree(mf->pages);
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
    mutex_lock(&mf->data_lock);
    int r = memfd_file_grow(mf, size);
    if (r < 0) {
        mutex_unlock(&mf->data_lock);
        return r;
    }
    size_t done = mf->size;
    while (done < size) {
        size_t poff = done % PAGE_SIZE;
        size_t chunk = PAGE_SIZE - poff;
        if (chunk > size - done) chunk = size - done;
        uint8_t *p = memfd_page_ptr(mf, done / PAGE_SIZE);
        if (p)
            memset(p + poff, 0, chunk);
        done += chunk;
    }
    mf->size = size;
    vn->size = size;
    mutex_unlock(&mf->data_lock);
    return 0;
}

static int memfd_file_readpage(vnode_t *vn, uint64_t index,
                               void *data, size_t len)
{
    if (!vn || !vn->fs_data || !data)
        return -EINVAL;
    memfd_file_t *mf = vn->fs_data;
    mutex_lock(&mf->data_lock);
    memset(data, 0, len);
    uint64_t off = index * PAGE_SIZE;
    size_t n = 0;
    if (off < mf->size) {
        n = mf->size - (size_t)off;
        if (n > len)
            n = len;
        uint8_t *p = memfd_page_ptr(mf, (size_t)index);
        if (p)
            memcpy(data, p, n);
    }
    mutex_unlock(&mf->data_lock);
    return (int)n;
}

static int memfd_file_writepage(vnode_t *vn, uint64_t index,
                                const void *data, size_t len)
{
    if (!vn || !vn->fs_data || !data)
        return -EINVAL;
    memfd_file_t *mf = vn->fs_data;
    mutex_lock(&mf->data_lock);
    uint64_t off = index * PAGE_SIZE;
    if (off < mf->size) {
        size_t n = mf->size - (size_t)off;
        if (n > len)
            n = len;
        uint8_t *p = memfd_page_ptr(mf, (size_t)index);
        if (p)
            memcpy(p, data, n);
    }
    mutex_unlock(&mf->data_lock);
    return 0;
}

static void memfd_file_release(vnode_t *vn)
{
    if (vn) {
        /* mmap faults cached the file's pages against this vnode; nothing
         * else drops them, so a shm pool would leak its whole mapping. */
        page_cache_truncate(vn, 0);
        vfs_drop_time_meta(vn);
        kfree(vn);
    }
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
    .readpage = memfd_file_readpage,
    .writepage = memfd_file_writepage,
    .release = memfd_file_release,
};

static int memfd_create_impl(int flags, unsigned allowed_flags, int secret)
{
    if (flags & ~(uint32_t)allowed_flags)
        return -EINVAL;
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
    mutex_init(&mf->data_lock);
    mf->secret = secret;
    task_t *cur = proc_current();
    mf->owner_euid = cur ? cur->cred.euid : 0;
    memset(vn, 0, sizeof(*vn));
    vn->ino = (uint64_t)(uintptr_t)vn;
    vn->type = VFS_FT_REGULAR;
    vn->mode = S_IFREG | 0777;
    vnode_ref_init(vn, 1);
    vn->ops = &g_memfile_vops;
    vn->fs_data = mf;
    vf->vnode = vn;
    vf->flags = O_RDWR;
    vfile_ref_init(vf, 1);
    vf->ops = &g_memfile_fops;
    vf->priv = mf;
    if (!(flags & 0x2U))
        vf->seals = F_SEAL_SEAL;
    return anonfd_install_vfile(vf, flags);
}

int memfd_create_file(int flags)
{
    return memfd_create_impl(flags, O_CLOEXEC | 0x2U | 0x4U, 0);
}

int memfd_secret_file(int flags)
{
    return memfd_create_impl(flags, O_CLOEXEC | 0x2U, 1);
}

int vfile_is_memfd_secret(vfile_t *vf)
{
    if (!vf || vf->ops != &g_memfile_fops || !vf->priv)
        return 0;
    return ((memfd_file_t *)vf->priv)->secret;
}

/* Secret memfds may only be mapped (and stolen through pidfd_getfd) by a
 * caller whose euid matches the creator; everyone else sees -EACCES. */
int memfd_secret_may_access(vfile_t *vf, task_t *caller)
{
    if (!vf || vf->ops != &g_memfile_fops || !vf->priv)
        return 1;
    memfd_file_t *mf = vf->priv;
    if (!mf->secret)
        return 1;
    return caller && caller->cred.euid == mf->owner_euid;
}

/* Replace the memfd contents with the given bytes (used by the DRM PRIME
 * export path to snapshot a dumb buffer).  Returns 0 on success. */
int memfd_set_contents(int fd, const void *data, size_t len)
{
    if (fd < 0 || !data)
        return -EINVAL;
    int gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return gfd;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return -EBADF;
    memfd_file_t *mf = vf ? vf->priv : NULL;
    if (!mf) {
        vfs_put_file_ref(gfd, vf);
        return -EBADF;
    }
    mutex_lock(&mf->data_lock);
    int r = memfd_file_grow(mf, len);
    if (r == 0) {
        size_t done = 0;
        while (done < len) {
            size_t poff = done % PAGE_SIZE;
            size_t chunk = PAGE_SIZE - poff;
            if (chunk > len - done) chunk = len - done;
            uint8_t *p = memfd_page_ptr(mf, done / PAGE_SIZE);
            if (p)
                memcpy(p + poff, (const uint8_t *)data + done, chunk);
            done += chunk;
        }
        mf->size = len;
        if (vf->vnode) vf->vnode->size = len;
    }
    mutex_unlock(&mf->data_lock);
    vfs_put_file_ref(gfd, vf);
    return r;
}
