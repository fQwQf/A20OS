#include "fs/fscontext.h"

#include "core/errno.h"
#include "core/string.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "fs/vfs/mount.h"
#include "mm/slab.h"

/*
 * Filesystem context (new mount API).
 *
 * A context file accumulates {source, fstype, options}; fsmount() with a
 * target resolves the path and calls the existing vfs_mount().  Without a
 * target it builds a detached mount and returns a mount fd instead, which
 * move_mount(2) later attaches.  mount_setattr and move_mount operate on the
 * mount table directly.
 */

typedef struct fscontext_file {
    char fstype[32];
    char source[256];
    char options[1024];
    int  flags;
} fscontext_file_t;

/* fsmount(2) mount attributes (the ms_flags argument). */
#define MOUNT_ATTR_RDONLY 0x00000001

static int fscontext_read(vfile_t *vf, char *buf, size_t count)
{
    (void)vf;
    (void)buf;
    (void)count;
    /* A filesystem context is not readable.  Linux's fscontext_fops has no
     * read op, so read(2) reports EINVAL; returning EOF instead made libmount
     * retry the read in a loop after fsopen(2). */
    return -EINVAL;
}

static int fscontext_write(vfile_t *vf, const char *buf, size_t count)
{
    (void)vf;
    (void)buf;
    (void)count;
    return (int)count;
}

static int fscontext_close(vfile_t *vf)
{
    fscontext_file_t *fc = vf ? vf->priv : NULL;
    if (fc) {
        kfree(fc);
        vf->priv = NULL;
    }
    return 0;
}

static vfile_ops_t g_fscontext_ops = {
    .read = fscontext_read,
    .write = fscontext_write,
    .close = fscontext_close,
};

/*
 * Mount fd: the handle fsmount(2) returns for a detached mount.  It carries
 * no I/O of its own; move_mount(2) looks the mount up through it, and closing
 * it before that happens tears the detached mount down again.
 */
typedef struct mountfd_file {
    mount_t *mnt;
} mountfd_file_t;

static int mountfd_read(vfile_t *vf, char *buf, size_t count)
{
    (void)vf;
    (void)buf;
    (void)count;
    return -EINVAL; /* a mount fd is not readable either */
}

static int mountfd_write(vfile_t *vf, const char *buf, size_t count)
{
    (void)vf;
    (void)buf;
    (void)count;
    return (int)count;
}

static int mountfd_close(vfile_t *vf)
{
    mountfd_file_t *mf = vf ? vf->priv : NULL;
    if (mf) {
        /* publish_ready means move_mount(2) already gave the mount a place in
         * the tree; from then on it belongs to the namespace, not to this
         * descriptor. */
        if (mf->mnt && !mf->mnt->publish_ready) {
            vfs_mount_fs_teardown(mf->mnt);
            vfs_mount_remove(mf->mnt);
        }
        kfree(mf);
        vf->priv = NULL;
    }
    return 0;
}

static vfile_ops_t g_mountfd_ops = {
    .read = mountfd_read,
    .write = mountfd_write,
    .close = mountfd_close,
};

static int mountfd_install(mount_t *mnt)
{
    mountfd_file_t *mf = kcalloc(1, sizeof(*mf));
    vfile_t *vf = vfile_alloc();
    if (!mf || !vf) {
        if (mf) kfree(mf);
        if (vf) vfile_free(vf);
        return -ENOMEM;
    }
    mf->mnt = mnt;
    vf->flags = O_RDWR;
    vfile_ref_init(vf, 1);
    vf->ops = &g_mountfd_ops;
    vf->priv = mf;
    return anonfd_install_vfile(vf, O_CLOEXEC);
}

mount_t *fscontext_mountfd_get(int gfd)
{
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return NULL;
    mount_t *mnt = NULL;
    if (vf->ops == &g_mountfd_ops && vf->priv)
        mnt = ((mountfd_file_t *)vf->priv)->mnt;
    vfs_put_file_ref(gfd, vf);
    return mnt;
}

static int fscontext_fsmount_detached(const char *source, const char *fstype,
                                      int flags, const char *options);

int fscontext_create(const char *fstype)
{
    if (!fstype || !fstype[0])
        return -EINVAL;

    fscontext_file_t *fc = kcalloc(1, sizeof(*fc));
    vfile_t *vf = vfile_alloc();
    if (!fc || !vf) {
        if (fc) kfree(fc);
        if (vf) vfile_free(vf);
        return -ENOMEM;
    }
    strncpy(fc->fstype, fstype, sizeof(fc->fstype) - 1);
    fc->fstype[sizeof(fc->fstype) - 1] = '\0';
    vf->flags = O_RDWR;
    vfile_ref_init(vf, 1);
    vf->ops = &g_fscontext_ops;
    vf->priv = fc;
    return anonfd_install_vfile(vf, O_CLOEXEC);
}

int fscontext_config(int gfd, const char *key, const char *value)
{
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return -EBADF;
    if (vf->ops != &g_fscontext_ops || !vf->priv) {
        vfs_put_file_ref(gfd, vf);
        return -EBADF;
    }
    fscontext_file_t *fc = vf->priv;
    int r = 0;
    if (key && strcmp(key, "source") == 0) {
        if (!value)
            r = -EINVAL;
        else {
            strncpy(fc->source, value, sizeof(fc->source) - 1);
            fc->source[sizeof(fc->source) - 1] = '\0';
        }
    } else if (key && strcmp(key, "type") == 0) {
        if (!value)
            r = -EINVAL;
        else {
            strncpy(fc->fstype, value, sizeof(fc->fstype) - 1);
            fc->fstype[sizeof(fc->fstype) - 1] = '\0';
        }
    } else if (key && strcmp(key, "sflags") == 0) {
        long v = 0;
        if (value) {
            int neg = 0;
            const char *p = value;
            if (*p == '-') { neg = 1; p++; }
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (*p - '0');
                p++;
            }
            if (neg) v = -v;
        }
        fc->flags = (int)v;
    } else if (key) {
        /* Other keys are mount options appended to the options string. */
        size_t used = strlen(fc->options);
        if (value && used + strlen(key) + strlen(value) + 3 < sizeof(fc->options)) {
            if (used) {
                fc->options[used++] = ',';
            }
            memcpy(fc->options + used, key, strlen(key));
            used += strlen(key);
            fc->options[used++] = '=';
            memcpy(fc->options + used, value, strlen(value));
            used += strlen(value);
            fc->options[used] = '\0';
        } else if (!value && used + strlen(key) + 2 < sizeof(fc->options)) {
            if (used)
                fc->options[used++] = ',';
            memcpy(fc->options + used, key, strlen(key));
            used += strlen(key);
            fc->options[used] = '\0';
        }
    }
    vfs_put_file_ref(gfd, vf);
    return r;
}

int fscontext_fsmount(int gfd, int flags, int mnt_flags, const char *target)
{
    /* @flags is FSMOUNT_CLOEXEC, which says nothing about the filesystem: it
     * must not reach mount_t.flags, where bit 0 means read-only.  The mount
     * attributes that do matter arrive in @mnt_flags. */
    (void)flags;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return -EBADF;
    if (vf->ops != &g_fscontext_ops || !vf->priv) {
        vfs_put_file_ref(gfd, vf);
        return -EBADF;
    }
    fscontext_file_t *fc = vf->priv;
    char fstype[32];
    char source[256];
    char options[1024];
    strncpy(fstype, fc->fstype, sizeof(fstype) - 1);
    fstype[sizeof(fstype) - 1] = '\0';
    strncpy(source, fc->source, sizeof(source) - 1);
    source[sizeof(source) - 1] = '\0';
    strncpy(options, fc->options, sizeof(options) - 1);
    options[sizeof(options) - 1] = '\0';
    int cflags = fc->flags;
    vfs_put_file_ref(gfd, vf);

    if (mnt_flags & MOUNT_ATTR_RDONLY)
        cflags |= VFS_MOUNT_RDONLY;

    if (!target)
        return fscontext_fsmount_detached(source, fstype, cflags, options);

    int r = vfs_mount(source[0] ? source : "none", target, fstype, cflags,
                      options[0] ? options : NULL);
    if (r < 0)
        return r;

    /* Return an O_PATH-style fd on the mount root. */
    struct vnode *vn = vfs_resolve_no_follow(target);
    if (!vn)
        return 0; /* mount succeeded; no fd available */
    int gfd2 = fscontext_open_tree_fd(vn);
    vnode_put(vn);
    return gfd2;
}

/*
 * fsmount(2) without a target: the filesystem is created but not placed in
 * the tree, and the returned fd is the only handle on it.  Closing that fd
 * before move_mount(2) attaches it destroys the mount again, which is what
 * keeps an abandoned fsmount from leaking a table slot.
 */
static int fscontext_fsmount_detached(const char *source, const char *fstype,
                                      int flags, const char *options)
{
    mount_t *mnt = NULL;
    int r = vfs_mount_detached(source[0] ? source : "none", fstype, flags,
                               options[0] ? options : NULL, &mnt);
    if (r < 0)
        return r;
    int mfd = mountfd_install(mnt);
    if (mfd < 0) {
        vfs_mount_fs_teardown(mnt);
        vfs_mount_remove(mnt);
    }
    return mfd;
}

int fscontext_open_tree_fd(struct vnode *vn)
{
    if (!vn)
        return -EINVAL;
    vfile_t *vf = vfile_alloc();
    if (!vf)
        return -ENOMEM;
    /* O_PATH-style fd: open the vnode with no I/O ops. */
    if (vn->ops && vn->ops->open) {
        vfile_t *opened = vn->ops->open(vn, O_RDONLY);
        if (opened) {
            vfile_free(vf);
            vf = opened;
        }
    }
    if (!vf->vnode)
        vf->vnode = vn; /* caller keeps its reference */
    vf->flags = O_PATH;
    vfile_ref_init(vf, 1);
    strncpy(vf->path, "tree", MAX_PATH_LEN - 1);
    vf->path[MAX_PATH_LEN - 1] = '\0';
    int gfd = fdtable_install_current_vfile(vf, 0);
    if (gfd < 0) {
        if (vf->ops && vf->ops->close)
            vf->ops->close(vf);
        vfile_free(vf);
        return gfd;
    }
    return gfd;
}
