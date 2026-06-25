#include "core/refcount.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/vfs.h"

vfile_t *a20_timerfd_vfile_alloc(void)
{
    return vfile_alloc();
}

void a20_timerfd_vfile_free(vfile_t *vf)
{
    if (vf)
        vfile_free(vf);
}

void *a20_timerfd_vfile_priv(vfile_t *vf)
{
    return vf ? vf->priv : NULL;
}

void a20_timerfd_vfile_init(vfile_t *vf, vfile_ops_t *ops, void *data, int flags)
{
    if (!vf)
        return;
    vf->ops = ops;
    vf->priv = data;
    vf->flags = flags;
    refcount_set(&vf->ref_count, 1);
}

int a20_timerfd_install_vfile(vfile_t *vf, int flags)
{
    return anonfd_install_vfile(vf, flags);
}

int a20_timerfd_vfile_ops_match(vfile_t *vf, vfile_ops_t *ops)
{
    return vf && vf->ops == ops;
}
