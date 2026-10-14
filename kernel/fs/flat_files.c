#ifdef CONFIG_NATIVE_FLAT_FILES
#include "fs/flat_files.h"
#include "fs/file.h"
#include "core/errno.h"
#include "core/fcntl.h"
#include "core/string.h"
#include "mm/slab.h"

/* fat32lite has one metadata scratch sector. Every operation, including open
 * and seek, uses this sleepable mutex; a callback may block on actual storage.
 * The shared mutex also serializes offsets of a shared open-file description. */
static mutex_t files_lock = MUTEX_INIT;
static fat32lite_fs_t files_fs;
static int files_bound;

int a20_flat_files_bind(const fat32lite_io_t *io, uint32_t partition_lba) {
    if (!io || !io->read || io->write)
        return -EINVAL;
    mutex_lock(&files_lock);
    int status =
        files_bound ? -EBUSY : fat32lite_mount(&files_fs, io, partition_lba);
    if (!status)
        files_bound = 1;
    mutex_unlock(&files_lock);
    return status;
}
static int file_read(vfile_t *vf, char *buf, size_t count) {
    mutex_lock(&files_lock);
    fat32lite_file_t *file = vf->priv;
    int result = fat32lite_read(file, buf, count);
    vf->offset = file->pos;
    mutex_unlock(&files_lock);
    return result < 0 ? -EIO : result;
}
static long file_seek(vfile_t *vf, long offset, int whence) {
    mutex_lock(&files_lock);
    fat32lite_file_t *file = vf->priv;
    int64_t base = whence == 0 ? 0 : whence == 1 ? file->pos : file->size;
    int64_t pos = base + (int64_t)offset;
    long result = -EINVAL;
    if (whence >= 0 && whence <= 2 && pos >= 0 && pos <= 0x7fffffff &&
        !fat32lite_seek(file, (uint32_t)pos)) {
        vf->offset = file->pos;
        result = (long)pos;
    }
    mutex_unlock(&files_lock);
    return result;
}
static int file_close(vfile_t *vf) {
    mutex_lock(&files_lock);
    int status = fat32lite_close(vf->priv);
    mutex_unlock(&files_lock);
    kfree(vf->priv);
    return status;
}
static vfile_ops_t file_ops = {
    .read = file_read, .lseek = file_seek, .close = file_close};

int a20_flat_file_open(const char *path, int flags, vfile_t **out) {
    if (!path || !out || flags != O_RDONLY)
        return -EINVAL;
    vfile_t *vf = vfile_alloc();
    vnode_t *vn = kcalloc(1, sizeof(*vn));
    fat32lite_file_t *file = kcalloc(1, sizeof(*file));
    if (!vf || !vn || !file) {
        if (vf)
            vfile_free(vf);
        kfree(vn);
        kfree(file);
        return -ENOMEM;
    }
    mutex_lock(&files_lock);
    int status =
        files_bound ? fat32lite_open(&files_fs, path, file) : FAT32LITE_ENOENT;
    mutex_unlock(&files_lock);
    if (status) {
        vfile_free(vf);
        kfree(vn);
        kfree(file);
        return status == FAT32LITE_ENOENT ? -ENOENT : -EIO;
    }
    vn->ino = (uint64_t)file->dir_lba * 16 + file->dir_index + 1;
    vn->type = VFS_FT_REGULAR;
    vn->mode = S_IFREG | 0444;
    vn->size = file->size;
    vf->vnode = vn;
    vf->flags = flags;
    vf->priv = file;
    vf->ops = &file_ops;
    *out = vf;
    return 0;
}

int a20_flat_file_image(vfile_t *vf, size_t limit, void **data,
                        size_t *length) {
    if (!vf || vf->ops != &file_ops || !data || !length)
        return -EINVAL;
    fat32lite_file_t file;
    mutex_lock(&files_lock);
    file = *(fat32lite_file_t *)vf->priv;
    mutex_unlock(&files_lock);
    if (!file.size || file.size > limit)
        return -ENOEXEC;
    void *buffer = kmalloc(file.size);
    if (!buffer)
        return -ENOMEM;
    mutex_lock(&files_lock);
    int result = fat32lite_seek(&file, 0);
    if (!result)
        result = fat32lite_read(&file, buffer, file.size);
    mutex_unlock(&files_lock);
    if (result != (int)file.size) {
        kfree(buffer);
        return -EIO;
    }
    *data = buffer;
    *length = file.size;
    return 0;
}

/* Small profile VFS dispatch. File identity/refcount/allocation and Native
 * handle ownership are the same production modules as the full VFS profile. */
int vfs_read_file(vfile_t *vf, char *buf, size_t count) {
    return vf && vf->ops == &file_ops ? file_read(vf, buf, count) : -EBADF;
}
int vfs_write_file(vfile_t *vf, const char *buf, size_t count) {
    (void)vf;
    (void)buf;
    (void)count;
    return -EROFS;
}
long vfs_lseek_vfile(vfile_t *vf, long offset, int whence) {
    return vf && vf->ops == &file_ops ? file_seek(vf, offset, whence) : -EBADF;
}
int vfs_vfile_stat(vfile_t *vf, kstat_t *out) {
    if (!vf || vf->ops != &file_ops || !out)
        return -EBADF;
    memset(out, 0, sizeof(*out));
    out->st_ino = vf->vnode->ino;
    out->st_mode = vf->vnode->mode;
    out->st_nlink = 1;
    out->st_size = vf->vnode->size;
    out->st_blksize = 512;
    out->st_blocks = (out->st_size + 511) / 512;
    return 0;
}
int vfs_finalize_closed_vfile(vfile_t *vf) {
    if (!vf)
        return 0;
    int close_error = 0;
    if (vf->ops && vf->ops->close)
        close_error = vf->ops->close(vf);
    kfree(vf->vnode);
    vfile_free(vf);
    return close_error;
}
#endif
