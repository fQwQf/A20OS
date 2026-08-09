#include "fs/fscontext.h"

#include "core/errno.h"
#include "core/string.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/slab.h"

/*
 * Filesystem context (new mount API).
 *
 * A context file accumulates {source, fstype, options}; fsmount() resolves
 * the target path and calls the existing vfs_mount().  mount_setattr and
 * move_mount operate on the mount table directly.
 */

typedef struct fscontext_file {
    char fstype[32];
    char source[256];
    char options[1024];
    int  flags;
} fscontext_file_t;

static int fscontext_read(vfile_t *vf, char *buf, size_t count)
{
    (void)vf;
    (void)buf;
    (void)count;
    return 0; /* context files have no read content */
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
    (void)mnt_flags;
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

    if (!target)
        return -EINVAL;
    int r = vfs_mount(source[0] ? source : "none", target, fstype, cflags | flags,
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
    int gfd = vfs_alloc_fd(vf);
    if (gfd < 0) {
        if (vf->ops && vf->ops->close)
            vf->ops->close(vf);
        vfile_free(vf);
        return -EMFILE;
    }
    return gfd;
}
