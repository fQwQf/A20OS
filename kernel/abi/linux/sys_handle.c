#include "syscall_impl.h"

#include "core/stdio.h"
#include "core/string.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/file_handle.h"
#include "fs/vfs.h"
#include "proc/proc.h"

/*
 * name_to_handle_at(2) / open_by_handle_at(2).
 *
 * Handles are kernel-side opaque 64-bit values backed by kernel/fs/file_handle.c.
 * The mount id is the filesystem type id of the vnode's mount, which is stable
 * for the lifetime of the mount.
 */

struct linux_file_handle {
    uint32_t handle_bytes;
    int      handle_type;
    unsigned char f_handle[];
};

#define HANDLE_BYTES 8

int64_t sys_name_to_handle_at(int dirfd, const char *pathname,
                              struct linux_file_handle *handle, int *mnt_id,
                              int flags)
{
    (void)flags;
    if (!pathname || !handle || !mnt_id)
        return -EFAULT;

    char kpath[MAX_PATH_LEN];
    long pr0 = user_path_strncpy(kpath, pathname, sizeof(kpath));
    if (pr0 < 0)
        return pr0;
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
    if (pr < 0)
        return pr;

    struct vnode *vn = vfs_resolve_no_follow(full);
    if (!vn)
        return -ENOENT;
    int mid = vn->mnt ? vn->mnt->type : FS_TYPE_RAMFS;
    uint64_t h = file_handle_mint(vn);
    vnode_put(vn);
    if (h == 0)
        return -ENOMEM;

    uint32_t ub = 0;
    if (copy_from_user(&ub, &handle->handle_bytes, sizeof(ub)) < 0)
        return -EFAULT;
    if (ub < HANDLE_BYTES) {
        uint32_t need = HANDLE_BYTES;
        if (copy_to_user(&handle->handle_bytes, &need, sizeof(need)) < 0)
            return -EFAULT;
        return -EOVERFLOW;
    }

    uint32_t hb = HANDLE_BYTES;
    int ht = 1;
    if (copy_to_user(&handle->handle_bytes, &hb, sizeof(hb)) < 0 ||
        copy_to_user(&handle->handle_type, &ht, sizeof(ht)) < 0 ||
        copy_to_user(&handle->f_handle, &h, sizeof(h)) < 0 ||
        copy_to_user(mnt_id, &mid, sizeof(mid)) < 0)
        return -EFAULT;
    return 0;
}

int64_t sys_open_by_handle_at(int mount_fd, struct linux_file_handle *handle,
                              int flags)
{
    (void)mount_fd;
    if (!handle)
        return -EFAULT;

    uint32_t ub = 0;
    uint64_t h = 0;
    if (copy_from_user(&ub, &handle->handle_bytes, sizeof(ub)) < 0)
        return -EFAULT;
    if (ub < HANDLE_BYTES)
        return -EOVERFLOW;
    if (copy_from_user(&h, &handle->f_handle, sizeof(h)) < 0)
        return -EFAULT;

    struct vnode *vn = file_handle_get(h);
    if (!vn)
        return -ESTALE;

    /* Reopen the vnode as a file and install it.  The vnode's filesystem
     * open op creates a vfile from the existing inode. */
    int gfd = vfs_open_vnode(vn, flags & O_ACCMODE);
    if (gfd < 0)
        return gfd;
    return fdtable_install_current(gfd, flags);
}
