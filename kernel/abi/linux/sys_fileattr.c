#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "sys/usercopy.h"

/* file_getattr(2) / file_setattr(2): LoongArch-only Linux syscalls that
 * expose the FS_IOC_GETFLAGS/SETFLAGS attribute interface as syscalls.  The
 * ABI layer only translates the user struct fileattr wire format and defers
 * to the VFS core (kernel/fs/vfs/fileattr.c), which owns the attribute
 * model. */

int64_t sys_file_getattr(int fd, void *attr)
{
    if (!attr)
        return -EFAULT;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf)
        return -EBADF;
    /* VFS core reports the attribute word; A20OS supports no flags yet, so
     * the caller sees valid=1, flags=0, masks=0. */
    a20_fileattr_t fa;
    vfs_fileattr_get(&fa);
    vfs_put_file_ref((int)gfd, vf);
    return copy_to_user(attr, &fa, sizeof(fa)) < 0 ? -EFAULT : 0;
}

int64_t sys_file_setattr(int fd, void *attr)
{
    if (!attr)
        return -EFAULT;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf)
        return -EBADF;
    a20_fileattr_t fa;
    if (copy_from_user(&fa, attr, sizeof(fa)) < 0) {
        vfs_put_file_ref((int)gfd, vf);
        return -EFAULT;
    }
    int r = vfs_fileattr_set(&fa);
    vfs_put_file_ref((int)gfd, vf);
    return r;
}
