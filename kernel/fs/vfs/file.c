#include "fs/vfs/file.h"
#include "fs/vfs/file_state.h"
#include "fs/quota.h"
#include "fs/vfs/mount.h"
#include "fs/vfs/mntns.h"
#include "fs/vfs/shutdown.h"
#include "fs/ext4.h"
#include "fs/vfs/stat_perm.h"
#include "fs/file.h"
#include "fs/pipe.h"
#include "fs/devfs.h"
#include "fs/block_cache.h"
#include "fs/page_cache.h"
#include "fs/inotify.h"
#include "ipc/ipc.h"
#include "mm/vm.h"
#include "proc/proc.h"
#include "core/timer.h"

_Static_assert(FS_TYPE_RAMFS == 1 && FS_TYPE_EXT4 == 3 && FS_TYPE_PROCFS == 4 &&
               FS_TYPE_DEVFS == 5 && FS_TYPE_CGROUP == 6 && FS_TYPE_SYSFS == 7 &&
               FS_TYPE_UXFS == 10 && FS_TYPE_LITTLEFS == 11,
               "file_state mirror: fs type values");
_Static_assert(VFS_MOUNT_RDONLY == 0x1, "file_state mirror: mount flags");

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
    return a20_vfs_should_read(flags);
}

int vfs_should_write(int flags)
{
    return a20_vfs_should_write(flags);
}

static int vfs_file_uses_page_cache(vnode_t *vn)
{
    if (!vn)
        return 0;
    mount_t *mnt = vn->mnt;
    return a20_vfs_file_uses_page_cache((vn->mode & S_IFMT) == S_IFREG,
                                        mnt ? (int)mnt->type : -1);
}

static int vfs_file_uses_buffered_write(vnode_t *vn)
{
    if (!vn)
        return 0;
    mount_t *mnt = vn->mnt;
    return a20_vfs_file_uses_buffered_write(
        (vn->mode & S_IFMT) == S_IFREG, mnt ? (int)mnt->type : -1,
        mnt ? mnt->flags : 0,
        (vn->ops && vn->ops->readpage) ? 1 : 0,
        (vn->ops && vn->ops->writepage) ? 1 : 0);
}

static int vfs_read_file_impl(vfile_t *vf, char *buf, size_t count)
{
    if (!vf)
        return -EBADF;
    if (vf->backend_close_done)
        return -ESHUTDOWN;
    if ((vf->vnode || vfs_is_pipe_vfile(vf) || vfs_is_char_device_vfile(vf)) &&
        !vfs_should_read(vf->flags))
        return -EBADF;
    if (vfs_file_uses_page_cache(vf->vnode) && (vf->flags & O_DIRECT)) {
        page_cache_writeback_vnode(vf->vnode, NULL, NULL);
        long cur_off = (long)vf->offset;
        page_cache_invalidate_uptodate_range(vf->vnode, (uint64_t)cur_off,
                                              (uint64_t)(cur_off + count));
    }
    int has_mmap_cache = vf->vnode && vf->vnode->ops &&
                         vf->vnode->ops->readpage;
    if ((vfs_file_uses_page_cache(vf->vnode) || has_mmap_cache) &&
        !(vf->flags & O_DIRECT) &&
        vf->ops && vf->ops->read && vf->ops->lseek) {
        /*
         * MAP_SHARED writers update the canonical cache frame directly.
         * Harvest hardware dirty bits before deciding whether the cached page
         * is authoritative, otherwise a stale !uptodate flag can refill the
         * page from disk and overwrite newer mmap data.
         */
        mm_sync_shared_dirty_for_vnode(vf->vnode);
        int r = page_cache_read_vfile(vf, buf, count);
        if (r != -ENOSYS) {
            if (r > 0)
                proc_io_account((uint64_t)r, 0, 0, 0);
            return r;
        }
    }
    if (vf->ops && vf->ops->read) {
        int r = vf->ops->read(vf, buf, count);
        if (r > 0)
            proc_io_account((uint64_t)r, 0, 0, 0);
        return r;
    }
    return -EBADF;
}

int vfs_read_file(vfile_t *vf, char *buf, size_t count)
{
    int ret = mntns_fileop_enter();
    if (ret < 0)
        return ret;
    ret = vfs_read_file_impl(vf, buf, count);
    mntns_fileop_leave();
    return ret;
}

static int vfs_write_file_impl(vfile_t *vf, const char *buf, size_t count)
{
    if (!vf)
        return -EBADF;
    if (vf->backend_close_done)
        return -ESHUTDOWN;
    if ((vf->vnode || vfs_is_pipe_vfile(vf) || vfs_is_char_device_vfile(vf)) &&
        !vfs_should_write(vf->flags))
        return -EBADF;
    if (vf->seals & F_SEAL_WRITE)
        return -EPERM;
    if ((vf->seals & F_SEAL_GROW) && vf->vnode &&
        vf->offset + count > vf->vnode->size)
        return -EPERM;
    int use_page_cache = vfs_file_uses_page_cache(vf->vnode);
    int has_mmap_cache = vf->vnode && vf->vnode->ops &&
                         vf->vnode->ops->readpage;
    int buffered_write = !(vf->flags & O_DIRECT) &&
                         vfs_file_uses_buffered_write(vf->vnode);
    if ((vf->flags & O_DIRECT) && use_page_cache) {
        page_cache_writeback_vnode(vf->vnode, NULL, NULL);
        long cur_off = (long)vf->offset;
        page_cache_invalidate_uptodate_range(vf->vnode, (uint64_t)cur_off,
                                              (uint64_t)(cur_off + count));
    }
    if (vf->ops && vf->ops->write) {
        mutex_t *write_lock =
            use_page_cache && !(vf->flags & O_DIRECT) && vf->vnode
                ? &vf->vnode->write_lock : NULL;
        if (write_lock)
            mutex_lock(write_lock);
        if ((vf->flags & O_APPEND) && vf->vnode)
            vf->offset = vf->vnode->size;
        size_t write_start = vf->offset;
        size_t old_size = vf->vnode ? vf->vnode->size : 0;
        if (vf->vnode && write_start + count > old_size &&
            quota_check_space(vf->vnode,
                              (uint64_t)(write_start + count) - old_size) <
                0) {
            if (write_lock)
                mutex_unlock(write_lock);
            return -EDQUOT;
        }
        int r = buffered_write
            ? page_cache_write_vfile(vf, buf, count)
            : vf->ops->write(vf, buf, count);
        if (r > 0 && vf->vnode) {
            size_t new_end = write_start + (size_t)r;
            if (new_end > old_size)
                quota_account_space(vf->vnode, new_end - old_size);
            if ((vf->flags & O_DIRECT) && use_page_cache)
                page_cache_invalidate_uptodate_range(vf->vnode, write_start,
                                                      write_start + (size_t)r);
            else if (use_page_cache && !buffered_write)
                page_cache_update_after_write(vf->vnode, write_start,
                                              old_size, buf, (size_t)r);
            else if (!use_page_cache && has_mmap_cache)
                page_cache_invalidate_uptodate_range(vf->vnode, write_start,
                                                      write_start + (size_t)r);
            vfs_touch_mtime(vf->vnode);
            inotify_vnode_event(vf->vnode, NULL, IN_MODIFY);
            a20_fs_notify(vf->vnode, A20_EVENT_FS_MODIFY, NULL, 0, 0);
        }
        if (write_lock)
            mutex_unlock(write_lock);
        if (r > 0)
            proc_io_account(0, (uint64_t)r, 0, 0);
        return r;
    }
    return -EBADF;
}

int vfs_write_file(vfile_t *vf, const char *buf, size_t count)
{
    int ret = mntns_fileop_enter();
    if (ret < 0)
        return ret;
    ret = vfs_write_file_impl(vf, buf, count);
    mntns_fileop_leave();
    return ret;
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

    mutex_lock(&vf->offset_lock);
    long saved = vf->ops->lseek(vf, 0, SEEK_CUR);
    if (saved < 0) {
        mutex_unlock(&vf->offset_lock);
        vfs_put_file_ref(fd, vf);
        return (int)saved;
    }
    long seek_r = vf->ops->lseek(vf, (long)offset, SEEK_SET);
    if (seek_r < 0) {
        mutex_unlock(&vf->offset_lock);
        vfs_put_file_ref(fd, vf);
        return (int)seek_r;
    }

    int r = vfs_read_file(vf, buf, count);
    long restore_r = vf->ops->lseek(vf, saved, SEEK_SET);
    mutex_unlock(&vf->offset_lock);
    vfs_put_file_ref(fd, vf);
    if (restore_r < 0 && r >= 0)
        return (int)restore_r;
    return r;
}

static long vfs_lseek_vfile_impl(vfile_t *vf, long offset, int whence)
{
    if (!vf)
        return -EBADF;
    if (vf->backend_close_done)
        return -ESHUTDOWN;
    int lock_offset = vf->vnode && vf->ops && vf->ops->lseek;
    if (lock_offset)
        mutex_lock(&vf->offset_lock);
    long r;
    if (devfs_is_tty_vfile(vf) || vfs_is_pipe_vfile(vf)) {
        r = -ESPIPE;
    } else if (vf->vnode && (((vf->vnode->mode) & S_IFMT) == S_IFIFO)) {
        r = -ESPIPE;
    } else if (vf->vnode && (((vf->vnode->mode) & S_IFMT) == 0140000)) { /* S_IFSOCK is 0140000 */
        r = -ESPIPE;
    } else if (vf->ops && vf->ops->lseek) {
        r = vf->ops->lseek(vf, offset, whence);
    } else {
        r = -EBADF;
    }
    if (lock_offset)
        mutex_unlock(&vf->offset_lock);
    return r;
}

long vfs_lseek_vfile(vfile_t *vf, long offset, int whence)
{
    int ret = mntns_fileop_enter();
    if (ret < 0)
        return ret;
    long result = vfs_lseek_vfile_impl(vf, offset, whence);
    mntns_fileop_leave();
    return result;
}

long vfs_lseek(int fd, long offset, int whence)
{
    vfile_t *vf = vfs_get_file_ref(fd);
    long r = vfs_lseek_vfile(vf, offset, whence);
    vfs_put_file(vf);
    return r;
}

static int vfs_getdents64_vfile_impl(vfile_t *vf, void *dirp, size_t count)
{
    if (!vf)
        return -EBADF;
    if (vf->backend_close_done)
        return -ESHUTDOWN;
    if (vf->ops && vf->ops->readdir)
        return vf->ops->readdir(vf, dirp, count);
    return -EBADF;
}

int vfs_getdents64_vfile(vfile_t *vf, void *dirp, size_t count)
{
    int ret = mntns_fileop_enter();
    if (ret < 0)
        return ret;
    ret = vfs_getdents64_vfile_impl(vf, dirp, count);
    mntns_fileop_leave();
    return ret;
}

int vfs_getdents64(int fd, void *dirp, size_t count)
{
    vfile_t *vf = vfs_get_file_ref(fd);
    int r = vfs_getdents64_vfile(vf, dirp, count);
    vfs_put_file(vf);
    return r;
}

static int vfs_ioctl_vfile_impl(vfile_t *vf, unsigned long req, void *arg)
{
    if (!vf)
        return -EBADF;
    if (vf->backend_close_done)
        return -ESHUTDOWN;
    if (vf->ops && vf->ops->ioctl)
        return vf->ops->ioctl(vf, req, arg);
    return -ENOTTY;
}

int vfs_ioctl_vfile(vfile_t *vf, unsigned long req, void *arg)
{
    int ret = mntns_fileop_enter();
    if (ret < 0)
        return ret;
    ret = vfs_ioctl_vfile_impl(vf, req, arg);
    mntns_fileop_leave();
    return ret;
}

int vfs_ioctl(int fd, unsigned long req, void *arg)
{
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf)
        return -EBADF;
    int r = vfs_ioctl_vfile(vf, req, arg);
    vfs_put_file(vf);
    return r;
}

/* mount_t::fs_data is only a bcache_t for the filesystem types below.  cgroup
 * stores its cg_sb_t and uxfs its uxfs_sb_t there, both far smaller than a
 * bcache_t, so handing them to bcache_sync() reads past the object and syncs
 * whatever memory follows it. */
static bcache_t *vfs_mount_bcache(mount_t *mnt)
{
    if (!mnt)
        return NULL;
    switch (mnt->type) {
    case FS_TYPE_FAT32:
    case FS_TYPE_NTFS:
    case FS_TYPE_EXT4:
    case FS_TYPE_LITTLEFS:
    case FS_TYPE_ISOFS:
        return (bcache_t *)mnt->fs_data;
    default:
        return NULL;
    }
}

int vfs_sync(void)
{
    int pc_r = page_cache_writeback_all(NULL, NULL);
    if (pc_r < 0)
        return pc_r;
    for (int i = 0; i < vfs_mount_count(); i++) {
        mount_t *mnt = vfs_mount_at(i);
        if (!mnt)
            continue;
        bcache_t *bc = vfs_mount_bcache(mnt);
        if (bc) {
            int ret = bcache_sync_checked(bc);
            if (ret < 0)
                return ret;
        } else if (mnt->type == FS_TYPE_UXFS && mnt->root &&
                   mnt->root->ops && mnt->root->ops->sync_vnode) {
            /* UXFS owns its device cache in the service.  Its mount-root
             * SYNC request is backend-wide and must be part of system sync,
             * even when no individual file is currently open. */
            int ret = mnt->root->ops->sync_vnode(mnt->root);
            if (ret < 0)
                return ret;
        }
    }
    return 0;
}

static uint64_t g_shutdown_mm_generation;
static int g_shutdown_mm_prepared;
static int g_shutdown_vfile_snapshot;

extern int mm_shared_write_shutdown_begin(uint64_t *generation);
extern int mm_shared_write_shutdown_drained(uint64_t generation);
extern int mm_shared_write_shutdown_revoke(uint64_t generation);
extern int mm_shared_write_shutdown_abort(uint64_t generation);
extern int mm_shared_write_shutdown_commit(uint64_t generation);

static int vfs_shutdown_begin_cb(void *context)
{
    (void)context;
    int ret = mntns_shutdown_begin();
    if (ret < 0)
        return ret;
    ret = vfile_shutdown_snapshot_begin();
    if (ret < 0) {
        mntns_shutdown_abort();
        return ret;
    }
    g_shutdown_vfile_snapshot = 1;
    g_shutdown_mm_prepared = 0;
    g_shutdown_mm_generation = 0;
    ret = mm_shared_write_shutdown_begin(&g_shutdown_mm_generation);
    if (ret < 0) {
        vfile_shutdown_snapshot_end();
        g_shutdown_vfile_snapshot = 0;
        mntns_shutdown_abort();
        return ret;
    }
    g_shutdown_mm_prepared = 1;
    return 0;
}

static int vfs_shutdown_abort_cb(void *context)
{
    (void)context;
    int ret = 0;
    if (g_shutdown_mm_prepared) {
        ret = mm_shared_write_shutdown_abort(g_shutdown_mm_generation);
        if (ret < 0)
            return ret;
        g_shutdown_mm_prepared = 0;
    }
    if (g_shutdown_vfile_snapshot) {
        vfile_shutdown_snapshot_end();
        g_shutdown_vfile_snapshot = 0;
    }
    mntns_shutdown_abort();
    return 0;
}

static bcache_t *shutdown_mount_cache(mount_t *mnt)
{
    if (!mnt)
        return NULL;
    switch (mnt->type) {
    case FS_TYPE_FAT32:
    case FS_TYPE_NTFS:
    case FS_TYPE_EXT4:
    case FS_TYPE_LITTLEFS:
    case FS_TYPE_ISOFS:
        return (bcache_t *)mnt->fs_data;
    default:
        return NULL;
    }
}

static int vfs_shutdown_sync_cb(void *context)
{
    (void)context;
    uint64_t generation = g_shutdown_mm_generation;
    int ret;
    /* This waits only for admitted MM write-grant transactions, not for user
     * tasks as a class; the UFSD service and ordinary kernel I/O keep running. */
    uint64_t wait_start = timer_get_ticks();
    uint64_t wait_budget = 5ULL * TICKS_PER_SEC;
    uint32_t wait_iterations = 0;
    do {
        ret = mm_shared_write_shutdown_drained(generation);
        if (ret == 0) {
            if ((uint64_t)(timer_get_ticks() - wait_start) >= wait_budget ||
                ++wait_iterations >= 1000000U)
                return -ETIMEDOUT;
            proc_yield();
        }
    } while (ret == 0);
    if (ret < 0)
        return ret;
    ret = mm_shared_write_shutdown_revoke(generation);
    if (ret < 0)
        return ret;

    ret = page_cache_writeback_all(NULL, NULL);
    if (ret < 0)
        return ret;
    int ns_count = mntns_shutdown_namespace_count();
    for (int n = 0; n < ns_count; n++) {
        mnt_namespace_t *ns = mntns_shutdown_namespace_at(n);
        if (!ns)
            return -EIO;
        for (int i = 0; i < ns->nmounts; i++) {
            mount_t *mnt = ns->mounts[i];
            if (!mnt || !mnt->publish_ready || mnt->shared_source ||
                mnt->ns_users == 0)
                continue;
            bcache_t *bc = shutdown_mount_cache(mnt);
            if (bc) {
                ret = bcache_sync_checked(bc);
                if (ret < 0)
                    return ret;
            } else if (mnt->type == FS_TYPE_UXFS && mnt->root &&
                       mnt->root->ops && mnt->root->ops->sync_vnode) {
                ret = mnt->root->ops->sync_vnode(mnt->root);
                if (ret < 0)
                    return ret;
            }
        }
    }
    return 0;
}

static int vfs_shutdown_commit_cb(void *context)
{
    (void)context;
    if (!g_shutdown_mm_prepared || !g_shutdown_vfile_snapshot)
        return -EINVAL;
    if (g_shutdown_mm_prepared == 1) {
        int ret = mm_shared_write_shutdown_commit(g_shutdown_mm_generation);
        if (ret < 0)
            return ret;
        /* From this point the VFS/MM write gates are permanent; a later
         * failure is retried in place and never resumes ordinary operations. */
        g_shutdown_mm_prepared = 2;
    }
    return vfile_shutdown_close_mounted();
}

static int vfs_shutdown_teardown_cb(void *context)
{
    (void)context;
    if (g_shutdown_mm_prepared != 2)
        return -EINVAL;
    /* Backend CLOSE can dirty inode/orphan metadata. Re-run checked writeback
     * after every close ACK before asking a filesystem to persist CLEAN. */
    int ret = page_cache_writeback_all(NULL, NULL);
    if (ret < 0)
        return ret;
    int ns_count = mntns_shutdown_namespace_count();
    for (int n = 0; n < ns_count; n++) {
        mnt_namespace_t *ns = mntns_shutdown_namespace_at(n);
        if (!ns)
            return -EIO;
        for (int i = 0; i < ns->nmounts; i++) {
            mount_t *mnt = ns->mounts[i];
            if (!mnt || !mnt->publish_ready || mnt->shared_source ||
                mnt->ns_users == 0)
                continue;
            bcache_t *bc = shutdown_mount_cache(mnt);
            if (bc && (ret = bcache_sync_checked(bc)) < 0)
                return ret;
            if (mnt->type == FS_TYPE_UXFS && mnt->root && mnt->root->ops &&
                mnt->root->ops->sync_vnode &&
                (ret = mnt->root->ops->sync_vnode(mnt->root)) < 0)
                return ret;
        }
    }
    /* Namespace registry is newest-first, so private clones are visited
     * before their shared source. This callback commits in-memory instances
     * without freeing mount/vnode objects that surviving task references may
     * still name while firmware poweroff is pending.
     *
     * Per-mount faults are isolated: one mount's failure must not starve
     * remaining mounts of their clean-shutdown attempt (a no-journal backend
     * left dirty by a skipped peer fail-closes its next mount).  The first
     * error is remembered and returned so the shutdown state machine keeps
     * its refuse-and-retry contract. */
    int first_ret = 0;
    for (int n = 0; n < ns_count; n++) {
        mnt_namespace_t *ns = mntns_shutdown_namespace_at(n);
        if (!ns)
            return -EIO;
        /* Snapshot the table before iterating: a concurrent unmount (e.g.
         * the peer service's exit cleanup running vfs_mount_remove on its
         * own CPU after acknowledging SHUTDOWN) compacts ns->mounts and
         * drops ns->nmounts mid-loop, which would silently skip every mount
         * past the removed slot — a no-journal backend left dirty by a
         * skipped entry then fail-closes its next mount.  Root references
         * keep the snapshot entries coherent after removal. */
        mount_t *snapshot[MNTNS_MAX_MOUNTS];
        int snap_count = 0;
        uint64_t snap_flags = mntns_mount_table_lock();
        for (int i = 0; i < ns->nmounts && snap_count < MNTNS_MAX_MOUNTS;
             i++) {
            if (!ns->mounts[i])
                continue;
            snapshot[snap_count++] = ns->mounts[i];
            vfs_mount_root_get(ns->mounts[i]);
        }
        mntns_mount_table_unlock(snap_flags);
        for (int i = 0; i < snap_count; i++) {
            mount_t *mnt = snapshot[i];
            if (!mnt->publish_ready || mnt->shared_source ||
                mnt->ns_users == 0)
                continue;
            ret = 0;
            if (mnt->type == FS_TYPE_EXT4) {
                ret = ext4_shutdown_clean(mnt->root);
            } else if (mnt->type == FS_TYPE_UXFS) {
                extern int uxfs_unmount(struct vnode *root);
                ret = uxfs_unmount(mnt->root);
            }
            if (ret < 0 && first_ret == 0)
                first_ret = ret;
            vfs_mount_root_put(mnt);
        }
    }
    if (first_ret < 0)
        return first_ret;
    vfile_shutdown_snapshot_end();
    g_shutdown_vfile_snapshot = 0;
    return 0;
}

int vfs_system_shutdown(void)
{
    static const vfs_shutdown_ops_t ops = {
        .context = NULL,
        .begin = vfs_shutdown_begin_cb,
        .sync = vfs_shutdown_sync_cb,
        .teardown = vfs_shutdown_teardown_cb,
        .abort = vfs_shutdown_abort_cb,
        .commit = vfs_shutdown_commit_cb,
    };
    return a20_vfs_shutdown_run(&ops);
}

static int vfs_fsync_vfile_impl(vfile_t *vf)
{
    if (!vf)
        return -EBADF;
    if (vf->backend_close_done)
        return -ESHUTDOWN;
    int r = 0;
    /* Filesystems that commit per open file (littlefs) sync here, before
     * the generic device flush below. */
    if (vf->ops && vf->ops->sync) {
        r = vf->ops->sync(vf);
        if (r < 0)
            return r;
    }
    if (vf->vnode) {
        mm_sync_shared_dirty_for_vnode(vf->vnode);
        int pc_r = page_cache_writeback_vnode(vf->vnode, NULL, NULL);
        if (pc_r < 0)
            return pc_r;
        /* A filesystem-provided sync scopes the block-cache flush to this
         * vnode's own data and allocation metadata instead of flushing the
         * whole mount (which makes one fsync pay for every concurrent
         * writer's dirty pages).  Filesystems without the op fall back to
         * the full mount sync. */
        if (vf->vnode->ops && vf->vnode->ops->sync_vnode) {
            r = vf->vnode->ops->sync_vnode(vf->vnode);
        } else {
            bcache_t *bc = vfs_mount_bcache(vf->vnode->mnt);
            if (bc)
                r = bcache_sync_checked(bc);
        }
    }
    return r;
}

int vfs_fsync_vfile(vfile_t *vf)
{
    int ret = mntns_fileop_enter();
    if (ret < 0)
        return ret;
    ret = vfs_fsync_vfile_impl(vf);
    mntns_fileop_leave();
    return ret;
}

int vfs_fsync(int fd)
{
    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf)
        return -EBADF;
    int r = vfs_fsync_vfile(vf);
    vfs_put_file_ref(fd, vf);
    return r;
}

int vfs_fdatasync(int fd)
{
    /* A20OS has no separate data-metadata distinction in writeback: the
     * data is written through the page cache before the block cache sync, so
     * fdatasync is equivalent to fsync for supported filesystems. */
    return vfs_fsync(fd);
}
