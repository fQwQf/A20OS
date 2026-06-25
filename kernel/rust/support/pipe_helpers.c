#include "core/refcount.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "proc/proc.h"

void *a20_pipe_vfile_priv(vfile_t *vf)
{
    return vf ? vf->priv : NULL;
}

int a20_pipe_vfile_flags(vfile_t *vf)
{
    return vf ? vf->flags : 0;
}

void a20_pipe_vfile_init(vfile_t *vf, vfile_ops_t *ops, void *priv_data, int flags)
{
    if (!vf)
        return;
    vf->ops = ops;
    vf->priv = priv_data;
    vf->flags = flags;
    refcount_set(&vf->ref_count, 1);
}

int a20_pipe_vfile_ops_eq(vfile_t *vf, vfile_ops_t *ops)
{
    return vf && vf->ops == ops;
}

int a20_pipe_task_pid(task_t *task)
{
    return task ? task->pid : -1;
}

void a20_pipe_task_set_blocked(task_t *task)
{
    if (task)
        task->state = PROC_BLOCKED;
}
