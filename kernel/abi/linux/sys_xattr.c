#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#include "fs/xattr.h"

static int xattr_copy_name(const char *uname, char *kname)
{
    if (!uname) return -EFAULT;
    if (user_strncpy(kname, uname, XATTR_NAME_MAX_LOCAL) < 0) return -EFAULT;
    if (!kname[0]) return -EINVAL;
    return 0;
}

static vnode_t *xattr_resolve_path(const char *upath, int nofollow)
{
    (void)nofollow;
    if (!upath) return NULL;
    char path[MAX_PATH_LEN];
    if (user_strncpy(path, upath, MAX_PATH_LEN) < 0) return NULL;
    return vfs_resolve(path);
}

static int64_t sys_xattr_set_vnode(vnode_t *vn, const char *uname,
                                   const void *uvalue, size_t size, int flags)
{
    char name[XATTR_NAME_MAX_LOCAL];
    int r = xattr_copy_name(uname, name);
    if (r < 0) return r;
    if (size > XATTR_VALUE_MAX_LOCAL) return -ENOSPC;
    uint8_t value[XATTR_VALUE_MAX_LOCAL];
    if (size && copy_from_user(value, uvalue, size) < 0)
        return -EFAULT;
    return xattr_set_vnode(vn, name, size ? value : NULL, size, flags);
}

static int64_t sys_xattr_get_vnode(vnode_t *vn, const char *uname,
                                   void *uvalue, size_t size)
{
    char name[XATTR_NAME_MAX_LOCAL];
    int r = xattr_copy_name(uname, name);
    if (r < 0) return r;
    if (!uvalue || size == 0)
        return xattr_get_vnode(vn, name, NULL, 0);
    if (size > XATTR_VALUE_MAX_LOCAL)
        size = XATTR_VALUE_MAX_LOCAL;
    uint8_t value[XATTR_VALUE_MAX_LOCAL];
    int64_t n = xattr_get_vnode(vn, name, value, size);
    if (n < 0) return n;
    if (copy_to_user(uvalue, value, (size_t)n) < 0)
        return -EFAULT;
    return n;
}

static int64_t sys_xattr_list_vnode(vnode_t *vn, char *ulist, size_t size)
{
    int64_t total = xattr_list_vnode(vn, NULL, 0);
    if (total < 0) return total;
    if (!ulist || size == 0)
        return total;
    if (size < (size_t)total)
        return -ERANGE;
    char *list = kmalloc(total ? (size_t)total : 1);
    if (!list) return -ENOMEM;
    int64_t n = xattr_list_vnode(vn, list, (size_t)total);
    if (n >= 0 && copy_to_user(ulist, list, (size_t)n) < 0)
        n = -EFAULT;
    kfree(list);
    return n;
}

static int64_t sys_xattr_remove_vnode(vnode_t *vn, const char *uname)
{
    char name[XATTR_NAME_MAX_LOCAL];
    int r = xattr_copy_name(uname, name);
    if (r < 0) return r;
    return xattr_remove_vnode(vn, name);
}

int64_t sys_setxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
    vnode_t *vn = xattr_resolve_path(path, 0);
    if (!vn) return -ENOENT;
    int64_t r = sys_xattr_set_vnode(vn, name, value, size, flags);
    vnode_put(vn);
    return r;
}

int64_t sys_lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
    return sys_setxattr(path, name, value, size, flags);
}

int64_t sys_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags)
{
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return -EBADF;
    if (!vf->vnode) {
        vfs_put_file_ref((int)gfd, vf);
        return -EBADF;
    }
    int64_t r = sys_xattr_set_vnode(vf->vnode, name, value, size, flags);
    vfs_put_file_ref((int)gfd, vf);
    return r;
}

int64_t sys_getxattr(const char *path, const char *name, void *value, size_t size)
{
    vnode_t *vn = xattr_resolve_path(path, 0);
    if (!vn) return -ENOENT;
    int64_t r = sys_xattr_get_vnode(vn, name, value, size);
    vnode_put(vn);
    return r;
}

int64_t sys_lgetxattr(const char *path, const char *name, void *value, size_t size)
{
    return sys_getxattr(path, name, value, size);
}

int64_t sys_fgetxattr(int fd, const char *name, void *value, size_t size)
{
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return -EBADF;
    if (!vf->vnode) {
        vfs_put_file_ref((int)gfd, vf);
        return -ENODATA;
    }
    int64_t r = sys_xattr_get_vnode(vf->vnode, name, value, size);
    vfs_put_file_ref((int)gfd, vf);
    return r;
}

int64_t sys_listxattr(const char *path, char *list, size_t size)
{
    vnode_t *vn = xattr_resolve_path(path, 0);
    if (!vn) return -ENOENT;
    int64_t r = sys_xattr_list_vnode(vn, list, size);
    vnode_put(vn);
    return r;
}

int64_t sys_llistxattr(const char *path, char *list, size_t size)
{
    return sys_listxattr(path, list, size);
}

int64_t sys_flistxattr(int fd, char *list, size_t size)
{
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return -EBADF;
    if (!vf->vnode) {
        vfs_put_file_ref((int)gfd, vf);
        return -EBADF;
    }
    int64_t r = sys_xattr_list_vnode(vf->vnode, list, size);
    vfs_put_file_ref((int)gfd, vf);
    return r;
}

int64_t sys_removexattr(const char *path, const char *name)
{
    vnode_t *vn = xattr_resolve_path(path, 0);
    if (!vn) return -ENOENT;
    int64_t r = sys_xattr_remove_vnode(vn, name);
    vnode_put(vn);
    return r;
}

int64_t sys_lremovexattr(const char *path, const char *name)
{
    return sys_removexattr(path, name);
}

int64_t sys_fremovexattr(int fd, const char *name)
{
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return -EBADF;
    if (!vf->vnode) {
        vfs_put_file_ref((int)gfd, vf);
        return -EBADF;
    }
    int64_t r = sys_xattr_remove_vnode(vf->vnode, name);
    vfs_put_file_ref((int)gfd, vf);
    return r;
}
