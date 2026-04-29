#include "fs/inotify.h"

#include "core/consts.h"
#include "core/string.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/slab.h"

static int inotify_empty_read(vfile_t *vf, char *buf, size_t count)
{
    (void)vf; (void)buf; (void)count;
    return -EAGAIN;
}

static vfile_ops_t g_inotify_ops = {
    .read = inotify_empty_read,
};

int inotify_create_file(int flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    vfile_t *vf = vfile_alloc();
    if (!vf) return -ENOMEM;
    vf->flags = O_RDONLY | (flags & O_NONBLOCK);
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_inotify_ops;
    return anonfd_install_vfile(vf, flags);
}
