#include "fs/vfs/file.h"
#include "fs/vfs/mount.h"
#include "fs/vfs/stat_perm.h"
#include "fs/file.h"
#include "fs/pipe.h"
#include "fs/devfs.h"
#include "fs/block_cache.h"
#include "fs/page_cache.h"

int vfs_is_pipe_vfile(vfile_t *vf)
{
    return pipe_vfile_is(vf);
}

int vfs_is_char_device_vfile(vfile_t *vf)
{
    return devfs_is_char_vfile(vf);
}

int vfs_should_read(int flags)
{
    int acc = flags & O_ACCMODE;
    return acc == O_RDONLY || acc == O_RDWR;
}

int vfs_should_write(int flags)
{
    int acc = flags & O_ACCMODE;
    return acc == O_WRONLY || acc == O_RDWR;
}

int vfs_read_file(vfile_t *vf, char *buf, size_t count)
{
    if (!vf)
        return -EBADF;
    if ((vf->vnode || vfs_is_pipe_vfile(vf) || vfs_is_char_device_vfile(vf)) &&
        !vfs_should_read(vf->flags))
        return -EBADF;
    if (vf->vnode && ((vf->vnode->mode & S_IFMT) == S_IFREG) &&
        (vf->flags & O_DIRECT)) {
        page_cache_writeback_vnode(vf->vnode, NULL, NULL);
        long cur_off = (long)vf->offset;
        page_cache_invalidate_uptodate_range(vf->vnode, (uint64_t)cur_off,
                                              (uint64_t)(cur_off + count));
    }
    if (vf->vnode && ((vf->vnode->mode & S_IFMT) == S_IFREG) &&
        !(vf->flags & O_DIRECT) &&
        vf->ops && vf->ops->read && vf->ops->lseek) {
        /* Skip page cache for virtual filesystems (procfs, cgroupfs, devfs,
         * ramfs) — they have no block-backed storage and the page cache
         * will dereference NULL pointers on their vnodes. */
        mount_t *vmnt = vf->vnode->mnt;
        int virtual_fs = (vmnt &&
            (vmnt->type == FS_TYPE_PROCFS ||
             vmnt->type == FS_TYPE_CGROUP ||
             vmnt->type == FS_TYPE_DEVFS ||
             vmnt->type == FS_TYPE_SYSFS));
        if (!virtual_fs) {
            int r = page_cache_read_vfile(vf, buf, count);
            if (r != -ENOSYS)
                return r;
        }
    }
    if (vf->ops && vf->ops->read)
        return vf->ops->read(vf, buf, count);
    return -EBADF;
}

int vfs_write_file(vfile_t *vf, const char *buf, size_t count)
{
    if (!vf)
        return -EBADF;
    if ((vf->vnode || vfs_is_pipe_vfile(vf) || vfs_is_char_device_vfile(vf)) &&
        !vfs_should_write(vf->flags))
        return -EBADF;
    if (vf->seals & F_SEAL_WRITE)
        return -EPERM;
    if ((vf->seals & F_SEAL_GROW) && vf->vnode &&
        vf->offset + count > vf->vnode->size)
        return -EPERM;
    if ((vf->flags & O_DIRECT) && vf->vnode && ((vf->vnode->mode & S_IFMT) == S_IFREG)) {
        page_cache_writeback_vnode(vf->vnode, NULL, NULL);
        long cur_off = (long)vf->offset;
        page_cache_invalidate_uptodate_range(vf->vnode, (uint64_t)cur_off,
                                              (uint64_t)(cur_off + count));
    }
    if (vf->ops && vf->ops->write) {
        if ((vf->flags & O_APPEND) && vf->vnode)
            vf->offset = vf->vnode->size;
        size_t write_start = vf->offset;
        int r = vf->ops->write(vf, buf, count);
        if (r > 0 && vf->vnode) {
            if (vf->flags & O_DIRECT)
                page_cache_invalidate_uptodate_range(vf->vnode, write_start,
                                                      write_start + (size_t)r);
            else
                page_cache_invalidate(vf->vnode);
            vfs_touch_mtime(vf->vnode);
        }
        return r;
    }
    return -EBADF;
}

int vfs_read(int fd, char *buf, size_t count)
{
    vfile_t *vf = vfs_get_file_ref(fd);
    int r = vfs_read_file(vf, buf, count);
    vfs_put_file_ref(fd, vf);
    return r;
}

int vfs_write(int fd, const char *buf, size_t count)
{
    vfile_t *vf = vfs_get_file_ref(fd);
    int r = vfs_write_file(vf, buf, count);
    vfs_put_file_ref(fd, vf);
    return r;
}

int vfs_pread(int fd, char *buf, size_t count, uint64_t offset)
{
    if (!buf)
        return -EINVAL;
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf)
        return -EBADF;
    if (!vf->ops || !vf->ops->lseek) {
        vfs_put_file_ref(fd, vf);
        return -ESPIPE;
    }

    long saved = vf->ops->lseek(vf, 0, SEEK_CUR);
    if (saved < 0) {
        vfs_put_file_ref(fd, vf);
        return (int)saved;
    }
    long seek_r = vf->ops->lseek(vf, (long)offset, SEEK_SET);
    if (seek_r < 0) {
        vfs_put_file_ref(fd, vf);
        return (int)seek_r;
    }

    int r = vfs_read_file(vf, buf, count);
    long restore_r = vf->ops->lseek(vf, saved, SEEK_SET);
    vfs_put_file_ref(fd, vf);
    if (restore_r < 0 && r >= 0)
        return (int)restore_r;
    return r;
}

long vfs_lseek(int fd, long offset, int whence)
{
    vfile_t *vf = vfs_get_file_ref(fd);
    long r = -EBADF;
    if (vf) {
        if (devfs_is_tty_vfile(vf) || vfs_is_pipe_vfile(vf)) {
            r = -ESPIPE;
        } else if (vf->vnode && (((vf->vnode->mode) & S_IFMT) == S_IFIFO)) {
            r = -ESPIPE;
        } else if (vf->vnode && (((vf->vnode->mode) & S_IFMT) == 0140000)) { /* S_IFSOCK is 0140000 */
            r = -ESPIPE;
        } else if (vf->ops && vf->ops->lseek) {
            r = vf->ops->lseek(vf, offset, whence);
        }
    }
    vfs_put_file_ref(fd, vf);
    return r;
}

int vfs_getdents64(int fd, void *dirp, size_t count)
{
    vfile_t *vf = vfs_get_file_ref(fd);
    int r = -EBADF;
    if (vf) {
        if (vf->ops && vf->ops->readdir)
            r = vf->ops->readdir(vf, dirp, count);
    }
    vfs_put_file_ref(fd, vf);
    return r;
}

int vfs_ioctl(int fd, unsigned long req, void *arg)
{
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf)
        return -EBADF;
    int r = -ENOTTY;
    if (vf->ops && vf->ops->ioctl)
        r = vf->ops->ioctl(vf, req, arg);
    vfs_put_file_ref(fd, vf);
    return r;
}

int vfs_sync(void)
{
    int pc_r = page_cache_writeback_all(NULL, NULL);
    if (pc_r < 0)
        return pc_r;
    for (int i = 0; i < vfs_mount_count(); i++) {
        mount_t *mnt = vfs_mount_at(i);
        if (mnt && mnt->fs_data)
            bcache_sync((bcache_t *)mnt->fs_data);
    }
    return 0;
}

int vfs_fsync(int fd)
{
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf)
        return -EBADF;
    if (vf->vnode) {
        int pc_r = page_cache_writeback_vnode(vf->vnode, NULL, NULL);
        if (pc_r < 0) {
            vfs_put_file_ref(fd, vf);
            return pc_r;
        }
    }
    if (vf->vnode && vf->vnode->mnt && vf->vnode->mnt->fs_data)
        bcache_sync((bcache_t *)vf->vnode->mnt->fs_data);
    vfs_put_file_ref(fd, vf);
    return 0;
}
