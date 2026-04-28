#include "syscall_internal.h"

#define XATTR_NAME_MAX_LOCAL 64
#define XATTR_VALUE_MAX_LOCAL 512
#define XATTR_TABLE_MAX 128
#define XATTR_CREATE 1
#define XATTR_REPLACE 2

typedef struct {
    int used;
    void *mnt;
    uint64_t ino;
    char name[XATTR_NAME_MAX_LOCAL];
    size_t size;
    uint8_t value[XATTR_VALUE_MAX_LOCAL];
} xattr_store_t;

static xattr_store_t g_xattrs[XATTR_TABLE_MAX];

static int xattr_vnode_key(vnode_t *vn, void **mnt, uint64_t *ino)
{
    if (!vn || !mnt || !ino) return -ENOENT;
    *mnt = vn->mnt;
    *ino = vn->ino;
    return 0;
}

static int xattr_store_find(void *mnt, uint64_t ino, const char *name)
{
    for (int i = 0; i < XATTR_TABLE_MAX; i++) {
        if (g_xattrs[i].used && g_xattrs[i].mnt == mnt &&
            g_xattrs[i].ino == ino && strcmp(g_xattrs[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int xattr_store_slot(void)
{
    for (int i = 0; i < XATTR_TABLE_MAX; i++)
        if (!g_xattrs[i].used) return i;
    return -ENOSPC;
}

static int xattr_copy_name(const char *uname, char *kname)
{
    if (!uname) return -EFAULT;
    if (user_strncpy(kname, uname, XATTR_NAME_MAX_LOCAL) < 0) return -EFAULT;
    if (!kname[0]) return -EINVAL;
    return 0;
}

static int64_t xattr_set_vnode(vnode_t *vn, const char *uname,
                                     const void *uvalue, size_t size, int flags)
{
    if (!vn) return -ENOENT;
    if (flags & ~(XATTR_CREATE | XATTR_REPLACE)) return -EINVAL;
    if (size > XATTR_VALUE_MAX_LOCAL) return -ENOSPC;

    char name[XATTR_NAME_MAX_LOCAL];
    int r = xattr_copy_name(uname, name);
    if (r < 0) return r;

    void *mnt;
    uint64_t ino;
    xattr_vnode_key(vn, &mnt, &ino);
    int idx = xattr_store_find(mnt, ino, name);
    if ((flags & XATTR_CREATE) && idx >= 0) return -EEXIST;
    if ((flags & XATTR_REPLACE) && idx < 0) return -ENODATA;
    if (idx < 0) {
        idx = xattr_store_slot();
        if (idx < 0) return idx;
        memset(&g_xattrs[idx], 0, sizeof(g_xattrs[idx]));
        g_xattrs[idx].used = 1;
        g_xattrs[idx].mnt = mnt;
        g_xattrs[idx].ino = ino;
        strncpy(g_xattrs[idx].name, name, XATTR_NAME_MAX_LOCAL - 1);
    }
    if (size && copy_from_user(g_xattrs[idx].value, uvalue, size) < 0)
        return -EFAULT;
    g_xattrs[idx].size = size;
    return 0;
}

static int64_t xattr_get_vnode(vnode_t *vn, const char *uname,
                                     void *uvalue, size_t size)
{
    if (!vn) return -ENOENT;
    char name[XATTR_NAME_MAX_LOCAL];
    int r = xattr_copy_name(uname, name);
    if (r < 0) return r;
    void *mnt;
    uint64_t ino;
    xattr_vnode_key(vn, &mnt, &ino);
    int idx = xattr_store_find(mnt, ino, name);
    if (idx < 0) return -ENODATA;
    if (!uvalue || size == 0) return (int64_t)g_xattrs[idx].size;
    if (size < g_xattrs[idx].size) return -ERANGE;
    if (copy_to_user(uvalue, g_xattrs[idx].value, g_xattrs[idx].size) < 0)
        return -EFAULT;
    return (int64_t)g_xattrs[idx].size;
}

static int64_t xattr_list_vnode(vnode_t *vn, char *ulist, size_t size)
{
    if (!vn) return -ENOENT;
    void *mnt;
    uint64_t ino;
    xattr_vnode_key(vn, &mnt, &ino);
    size_t total = 0;
    for (int i = 0; i < XATTR_TABLE_MAX; i++) {
        if (g_xattrs[i].used && g_xattrs[i].mnt == mnt && g_xattrs[i].ino == ino)
            total += strlen(g_xattrs[i].name) + 1;
    }
    if (!ulist || size == 0) return (int64_t)total;
    if (size < total) return -ERANGE;
    char *buf = kmalloc(total ? total : 1);
    if (!buf) return -ENOMEM;
    size_t off = 0;
    for (int i = 0; i < XATTR_TABLE_MAX; i++) {
        if (g_xattrs[i].used && g_xattrs[i].mnt == mnt && g_xattrs[i].ino == ino) {
            size_t len = strlen(g_xattrs[i].name) + 1;
            memcpy(buf + off, g_xattrs[i].name, len);
            off += len;
        }
    }
    int cr = copy_to_user(ulist, buf, total);
    kfree(buf);
    return cr < 0 ? -EFAULT : (int64_t)total;
}

static int64_t xattr_remove_vnode(vnode_t *vn, const char *uname)
{
    if (!vn) return -ENOENT;
    char name[XATTR_NAME_MAX_LOCAL];
    int r = xattr_copy_name(uname, name);
    if (r < 0) return r;
    void *mnt;
    uint64_t ino;
    xattr_vnode_key(vn, &mnt, &ino);
    int idx = xattr_store_find(mnt, ino, name);
    if (idx < 0) return -ENODATA;
    memset(&g_xattrs[idx], 0, sizeof(g_xattrs[idx]));
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

int64_t sys_setxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
    vnode_t *vn = xattr_resolve_path(path, 0);
    if (!vn) return -ENOENT;
    int64_t r = xattr_set_vnode(vn, name, value, size, flags);
    vnode_put(vn);
    return r;
}

int64_t sys_lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
    return sys_setxattr(path, name, value, size, flags);
}

int64_t sys_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags)
{
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file((int)gfd);
    if (!vf || !vf->vnode) return -EBADF;
    return xattr_set_vnode(vf->vnode, name, value, size, flags);
}

int64_t sys_getxattr(const char *path, const char *name, void *value, size_t size)
{
    vnode_t *vn = xattr_resolve_path(path, 0);
    if (!vn) return -ENOENT;
    int64_t r = xattr_get_vnode(vn, name, value, size);
    vnode_put(vn);
    return r;
}

int64_t sys_lgetxattr(const char *path, const char *name, void *value, size_t size)
{
    return sys_getxattr(path, name, value, size);
}

int64_t sys_fgetxattr(int fd, const char *name, void *value, size_t size)
{
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file((int)gfd);
    if (!vf) return -EBADF;
    if (!vf->vnode) return -ENODATA;
    return xattr_get_vnode(vf->vnode, name, value, size);
}

int64_t sys_listxattr(const char *path, char *list, size_t size)
{
    vnode_t *vn = xattr_resolve_path(path, 0);
    if (!vn) return -ENOENT;
    int64_t r = xattr_list_vnode(vn, list, size);
    vnode_put(vn);
    return r;
}

int64_t sys_llistxattr(const char *path, char *list, size_t size)
{
    return sys_listxattr(path, list, size);
}

int64_t sys_flistxattr(int fd, char *list, size_t size)
{
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file((int)gfd);
    if (!vf || !vf->vnode) return -EBADF;
    return xattr_list_vnode(vf->vnode, list, size);
}

int64_t sys_removexattr(const char *path, const char *name)
{
    vnode_t *vn = xattr_resolve_path(path, 0);
    if (!vn) return -ENOENT;
    int64_t r = xattr_remove_vnode(vn, name);
    vnode_put(vn);
    return r;
}

int64_t sys_lremovexattr(const char *path, const char *name)
{
    return sys_removexattr(path, name);
}

int64_t sys_fremovexattr(int fd, const char *name)
{
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file((int)gfd);
    if (!vf || !vf->vnode) return -EBADF;
    return xattr_remove_vnode(vf->vnode, name);
}
