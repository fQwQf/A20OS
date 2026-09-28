/*
 * uxfs.h -- kernel-side interface to the user-space filesystem proxy (uxfs).
 *
 * Design and boundaries: docs/hybrid-kernel/06-user-fs.md.  The ABI-layer
 * fs_serve / fs_block_io syscalls (kernel/abi/native/sys_native_fs.c) use this
 * interface to perform mount registration and controlled block IO.
 */
#ifndef FS_UXFS_H
#define FS_UXFS_H

#include "core/types.h"

struct a20_channel_ep;
struct task_t;
struct vnode;

/*
 * uxfs_serve_mount -- mount the user-space file service at @ep onto @path.
 *
 * @path          mount point (must already exist and be a directory)
 * @ep            server-side channel endpoint; on success ownership of the
 *                reference transfers to uxfs
 * @server        service task that requested the registration (used to
 *                validate block-IO ownership)
 * @block_index   class index of the block device the service may access;
 *                <0 means there is no block backing
 * @serve_flags   bit0: the server declares a read-only backing (e.g. iso9660);
 *                when set the mount is marked VFS_MOUNT_RDONLY and page-cache
 *                buffered writes are disabled accordingly
 *
 * Returns 0 or a negative errno.  A UFS_OP_INIT handshake is performed before
 * the mount; if the service is unavailable the error is
 * -EIO/-ETIMEDOUT。
 */
int uxfs_serve_mount(const char *path, struct a20_channel_ep *ep,
                     struct task_t *server, int block_index,
                     uint32_t serve_flags);

/* umount teardown: release the server endpoint reference (called from the
 * FS_TYPE branch of vfs_umount). */
void uxfs_unmount(struct vnode *root);

/*
 * uxfs_block_io -- controlled block IO: permitted only when @task is the
 * uxfs service task currently registered for the mount, so that an arbitrary
 * process cannot bypass the filesystem and read or write the service's disk
 * directly.  @write is 0 for read and 1 for write.  @buf points at a kernel
 * buffer; @lba and @count are in sectors.
 */
int uxfs_block_io(struct task_t *task, int block_index, int write, uint64_t lba,
                  void *buf, uint32_t count);

/* Capacity query (in sectors): subject to the same ownership check.  The
 * service process uses it to initialise its block device descriptor. */
int uxfs_block_capacity(struct task_t *task, int block_index,
                        uint64_t *out_sectors);

#endif /* FS_UXFS_H */
