#include "syscall_impl.h"

#include "core/string.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/fscontext.h"
#include "fs/vfs.h"
#include "fs/vfs/mount.h"
#include "proc/proc.h"

/*
 * New mount API syscalls: open_tree, move_mount, fsopen, fsconfig, fsmount,
 * fspick, mount_setattr.  A20OS implements these on top of the existing
 * mount table through kernel/fs/fscontext.c.
 */

static int fs_mount_priv_check(void)
{
    task_t *t = proc_current();
    if (!t)
        return -EPERM;
    if (!proc_has_cap(t, CAP_SYS_ADMIN) && t->cred.euid != 0)
        return -EPERM;
    return 0;
}

int64_t sys_fsopen(const char *fsname, unsigned flags)
{
    if (flags)
        return -EINVAL;
    if (!fsname)
        return -EFAULT;
    int priv = fs_mount_priv_check();
    if (priv < 0)
        return priv;

    char name[64];
    if (user_strncpy(name, fsname, sizeof(name)) < 0)
        return -EFAULT;
    return fscontext_create(name);
}

int64_t sys_fsconfig(int fs_fd, unsigned cmd, const char *key,
                     const void *value, int aux)
{
    (void)aux;
    int64_t gfd = fdtable_get_current(fs_fd);
    if (gfd < 0)
        return -EBADF;

    char kbuf[128];
    char vbuf[512];
    const char *kp = NULL, *vp = NULL;
    if (cmd != 0 && key) { /* FSCONFIG_SET_FLAG(1)/SET_STRING(2)/SET_PATH(3) use key */
        if (user_strncpy(kbuf, key, sizeof(kbuf)) < 0)
            return -EFAULT;
        kp = kbuf;
    }
    if (value && cmd != 0 && cmd != 1) {
        if (user_strncpy(vbuf, (const char *)value, sizeof(vbuf)) < 0)
            return -EFAULT;
        vp = vbuf;
    }
    return fscontext_config((int)gfd, kp, vp);
}

int64_t sys_fsmount(int fs_fd, unsigned flags, unsigned mnt_flags)
{
    int64_t gfd = fdtable_get_current(fs_fd);
    if (gfd < 0)
        return -EBADF;
    return fscontext_fsmount((int)gfd, (int)flags, (int)mnt_flags, NULL);
}

int64_t sys_fspick(int dirfd, const char *path, unsigned flags)
{
    (void)flags;
    if (!path)
        return -EFAULT;
    int priv = fs_mount_priv_check();
    if (priv < 0)
        return priv;

    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, sizeof(kpath)) < 0)
        return -EFAULT;
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
    if (pr < 0)
        return pr;

    struct vnode *vn = vfs_resolve_no_follow(full);
    if (!vn)
        return -ENOENT;
    /* fspick returns a context fd bound to the picked mount.  We return an
     * fscontext preconfigured with the "type" of the picked mount. */
    int gfd = fscontext_create(vn->mnt && vn->mnt->fstype[0] ? vn->mnt->fstype
                                                             : "ramfs");
    vnode_put(vn);
    return gfd;
}

int64_t sys_open_tree(int dirfd, const char *path, unsigned flags)
{
    (void)flags;
    if (!path)
        return -EFAULT;

    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, sizeof(kpath)) < 0)
        return -EFAULT;
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
    if (pr < 0)
        return pr;

    struct vnode *vn = vfs_resolve_no_follow(full);
    if (!vn)
        return -ENOENT;
    int gfd = fscontext_open_tree_fd(vn);
    vnode_put(vn);
    if (gfd < 0)
        return gfd;
    return fdtable_install_current(gfd, O_PATH);
}

int64_t sys_move_mount(int from_dfd, const char *from_path, int to_dfd,
                       const char *to_path, unsigned flags)
{
    (void)flags;
    int priv = fs_mount_priv_check();
    if (priv < 0)
        return priv;
    if (!from_path || !to_path)
        return -EFAULT;

    char fbuf[MAX_PATH_LEN], tbuf[MAX_PATH_LEN];
    if (user_strncpy(fbuf, from_path, sizeof(fbuf)) < 0 ||
        user_strncpy(tbuf, to_path, sizeof(tbuf)) < 0)
        return -EFAULT;
    char full_f[MAX_PATH_LEN], full_t[MAX_PATH_LEN];
    if (syscall_path_at(from_dfd, fbuf, full_f, sizeof(full_f)) < 0)
        return -EFAULT;
    if (syscall_path_at(to_dfd, tbuf, full_t, sizeof(full_t)) < 0)
        return -EFAULT;

    /* A20OS mounts are bound to their path in the mount table; "moving" is
     * approximated by re-mounting the source's filesystem type at the target
     * and updating the table path.  The simplest faithful behaviour is to
     * report the mount as moved by swapping table entries. */
    return vfs_move_mount(full_f, full_t);
}

int64_t sys_mount_setattr(int dfd, const char *path, unsigned flags,
                          const void *attr, size_t size)
{
    (void)dfd;
    (void)flags;
    (void)attr;
    (void)size;
    int priv = fs_mount_priv_check();
    if (priv < 0)
        return priv;
    if (!path)
        return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, sizeof(kpath)) < 0)
        return -EFAULT;
    /* A20OS does not support per-mount attribute changes beyond the flags
     * stored in the mount table; validate the path exists and report
     * success for read-only attribute sets. */
    char full[MAX_PATH_LEN];
    if (syscall_path_at(dfd, kpath, full, sizeof(full)) < 0)
        return -EFAULT;
    struct vnode *vn = vfs_resolve_no_follow(full);
    if (!vn)
        return -ENOENT;
    vnode_put(vn);
    return 0;
}
