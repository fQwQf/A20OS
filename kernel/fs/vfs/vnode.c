#include "fs/vfs.h"
#include "core/stdio.h"

void vnode_ref_init(vnode_t *vn, int refs)
{
    if (!vn)
        return;
    refcount_set(&vn->ref_count, refs);
}

void vnode_get(vnode_t *vn)
{
    if (!vn)
        return;
    refcount_inc(&vn->ref_count);
}

int vnode_ref_read(vnode_t *vn)
{
    if (!vn)
        return 0;
    return refcount_read(&vn->ref_count);
}

void vnode_put(vnode_t *vn)
{
    if (!vn)
        return;
    if (vnode_ref_read(vn) <= 0) {
        printf("[VFS BUG] vnode_put on freed vnode %p ino=%lu\n",
               (void *)vn, vn->ino);
        return;
    }
    if (refcount_dec_and_test(&vn->ref_count)) {
        if (vn->ops && vn->ops->release)
            vn->ops->release(vn);
    }
}
