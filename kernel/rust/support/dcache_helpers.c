#include "fs/vfs.h"

int a20_dcache_dir_key(vnode_t *dir, mount_t **mnt_out, uint64_t *ino_out)
{
    if (!dir || !dir->mnt)
        return 0;
    if (mnt_out)
        *mnt_out = dir->mnt;
    if (ino_out)
        *ino_out = dir->ino;
    return 1;
}

int a20_dcache_mount_type(mount_t *mnt)
{
    if (!mnt)
        return 0;
    return mnt->type;
}

void a20_dcache_vnode_get(vnode_t *vn)
{
    vnode_get(vn);
}

void a20_dcache_vnode_put(vnode_t *vn)
{
    vnode_put(vn);
}

int a20_dcache_vnode_ref_read(vnode_t *vn)
{
    return vnode_ref_read(vn);
}
