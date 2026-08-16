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
    long pr0 = user_path_strncpy(kpath, path, sizeof(kpath));
    if (pr0 < 0)
        return pr0;
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
    long pr0 = user_path_strncpy(kpath, path, sizeof(kpath));
    if (pr0 < 0)
        return pr0;
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
    long pr0;
    if ((pr0 = user_path_strncpy(fbuf, from_path, sizeof(fbuf))) < 0 ||
        (pr0 = user_path_strncpy(tbuf, to_path, sizeof(tbuf))) < 0)
        return pr0;
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
    long pr0 = user_path_strncpy(kpath, path, sizeof(kpath));
    if (pr0 < 0)
        return pr0;
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

/* statmount(2): report mount attributes into a statmnt buffer.  A20OS
 * supports a minimal subset: mnt_id, mnt_parent_id, mnt_mountpoint and
 * mnt_fs_type. */
#define STATMOUNT_MNT_ID            0x0001
#define STATMOUNT_MNT_PARENT        0x0002
#define STATMOUNT_MNT_ROOT          0x0004
#define STATMOUNT_MNT_POINT         0x0008
#define STATMOUNT_FS_TYPE           0x0010

int64_t sys_statmount(uint64_t mnt_id, uint64_t flags, void *buf,
                      size_t bufsize, unsigned int mask)
{
    (void)flags;
    if (!buf)
        return -EFAULT;
    /* A minimal fixed layout: the first two u64s are size and mask, then
     * fields follow.  We only fill what the caller requested. */
    char *p = buf;
    /* 64-bit struct statmnt header: u64 size, u64 mask, u32 sb_flags,
     * u32 mnt_flags, u64 mnt_id, u64 mnt_parent_id, u64 mnt_group_id,
     * u64 mnt_root, u64 mnt_point, u64 mnt_ns_id, then strings. */
    if (bufsize < 64)
        return -EOVERFLOW;
    memset(p, 0, bufsize < 128 ? bufsize : 128);

    mount_t *mnt = NULL;
    for (int i = 0; i < vfs_mount_count(); i++) {
        mount_t *m = vfs_mount_at(i);
        if (m && (uint64_t)i == mnt_id) {
            mnt = m;
            break;
        }
    }
    if (!mnt)
        return -ENOENT;

    uint64_t *u = (uint64_t *)p;
    u[0] = 64; /* size */
    u[1] = mask;
    u[4] = mnt_id;            /* mnt_id */
    u[5] = 0;                 /* mnt_parent_id */
    /* mnt_root string at offset 32, mnt_point at 48, fs_type at 64 */
    size_t used = 64;
    if (mask & STATMOUNT_MNT_ROOT) {
        const char *root = "/";
        size_t n = strlen(root) + 1;
        if (used + n <= bufsize) {
            memcpy(p + used, root, n);
            u[6] = (uint64_t)used; /* mnt_root offset */
            used += n;
        }
    }
    if (mask & STATMOUNT_MNT_POINT) {
        const char *pt = mnt->path;
        size_t n = strlen(pt) + 1;
        if (used + n <= bufsize) {
            memcpy(p + used, pt, n);
            u[7] = (uint64_t)used;
            used += n;
        }
    }
    if (mask & STATMOUNT_FS_TYPE) {
        const char *ft = mnt->fstype[0] ? mnt->fstype : "ramfs";
        size_t n = strlen(ft) + 1;
        if (used + n <= bufsize) {
            memcpy(p + used, ft, n);
            ((uint64_t *)(p + 40))[0] = (uint64_t)used; /* fs_type at offset 40 */
            used += n;
        }
    }
    if (copy_to_user(buf, p, used) < 0)
        return -EFAULT;
    return 0;
}

int64_t sys_listmount(uint64_t mnt_id, uint64_t last_mnt_id,
                      uint64_t *list, size_t nr, unsigned int flags)
{
    (void)last_mnt_id;
    (void)flags;
    if (!list)
        return -EFAULT;
    if (nr == 0)
        return 0;
    uint64_t *ids = proc_scratch_buffer(nr * sizeof(uint64_t));
    if (!ids)
        return -ENOMEM;
    uint64_t start = (uint64_t)(mnt_id == 0 ? 0 : mnt_id + 1);
    uint64_t n = 0;
    for (int i = (int)start; i < vfs_mount_count() && n < nr; i++)
        ids[n++] = (uint64_t)i;
    if (n > 0 && copy_to_user(list, ids, n * sizeof(uint64_t)) < 0)
        return -EFAULT;
    return (int64_t)n;
}

int64_t sys_listns(unsigned int nstype, uint64_t *nsids, size_t nr)
{
    (void)nstype;
    if (nsids && nr > 0) {
        uint64_t zero = 0;
        if (copy_to_user(nsids, &zero, sizeof(zero)) < 0)
            return -EFAULT;
        return 1;
    }
    return 0;
}

int64_t sys_open_tree_attr(int dfd, const char *path, unsigned int flags,
                           unsigned int attr_mask, void *attr, size_t size)
{
    (void)attr_mask;
    (void)attr;
    (void)size;
    /* open_tree_attr returns an fd for a mount tree with requested
     * attributes; equivalent to open_tree plus a statmount-style query. */
    return sys_open_tree(dfd, path, flags);
}
