#include "core/string.h"
#include "fs/locks.h"
#include "proc/proc.h"
#include "fs/vfs.h"

uintptr_t a20_locks_file_key(vfile_t *vf)
{
    if (vf && vf->vnode && vf->vnode->ino)
        return (((uintptr_t)vf->vnode->mnt) >> 3) ^
               ((uintptr_t)vf->vnode->ino << 17) ^
               (uintptr_t)vf->vnode->ino;
    return (uintptr_t)vf;
}

int64_t a20_locks_file_size(vfile_t *vf)
{
    if (!vf || !vf->vnode)
        return 0;
    kstat_t st;
    memset(&st, 0, sizeof(st));
    if (vf->vnode->ops && vf->vnode->ops->stat &&
        vf->vnode->ops->stat(vf->vnode, &st) == 0)
        return (int64_t)st.st_size;
    return (int64_t)vf->vnode->size;
}

int64_t a20_locks_file_offset(vfile_t *vf)
{
    return vf ? (int64_t)vf->offset : 0;
}

int a20_locks_current_pid(void)
{
    task_t *cur = proc_current();
    return cur ? cur->pid : -1;
}
