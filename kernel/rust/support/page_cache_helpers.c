#include "abi/linux/errno.h"
#include "fs/vfs.h"
#include "mm/frame.h"

void *a20_pfn_to_virt(pfn_t pfn)
{
    return pfn_to_virt(pfn);
}

int a20_pfn_valid(pfn_t pfn)
{
    return pfn_valid(pfn);
}

uint32_t a20_frame_refcount(pfn_t pfn)
{
    if (!pfn_valid(pfn))
        return 0;
    return pfa.meta[pfn].refcount;
}

int a20_vnode_writepage(vnode_t *vn, uint64_t index, const void *data, size_t len)
{
    if (!vn || !vn->ops || !vn->ops->writepage)
        return -ENOSYS;
    return vn->ops->writepage(vn, index, data, len);
}
