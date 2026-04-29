#include "fs/anonfd.h"

#include "core/consts.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "mm/slab.h"

int anonfd_free_priv_close(vfile_t *vf)
{
    if (vf && vf->priv) {
        kfree(vf->priv);
        vf->priv = NULL;
    }
    return 0;
}

int anonfd_install_vfile(vfile_t *vf, int flags)
{
    int gfd = vfs_alloc_fd(vf);
    if (gfd < 0) {
        if (vf->ops && vf->ops->close)
            vf->ops->close(vf);
        vfile_free(vf);
        return -EMFILE;
    }
    return fdtable_install_current(gfd, flags);
}
