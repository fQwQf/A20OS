#include "net/socket_internal.h"
#include "net/lwip_stack.h"
#include "fs/file.h"

void a20_socket_file_lwip_poll(void)
{
    a20_lwip_poll();
}

task_t *a20_socket_file_proc_current(void)
{
    return proc_current();
}

void a20_socket_file_sched(void)
{
    sched();
}

void a20_socket_file_proc_make_ready(task_t *task)
{
    if (task)
        proc_make_ready(task);
}

int a20_socket_file_task_state(task_t *task)
{
    return task ? (int)task->state : 0;
}

void *a20_socket_file_vfile_priv(vfile_t *vf)
{
    return vf ? vf->priv : NULL;
}

void a20_socket_file_vfile_set_priv(vfile_t *vf, void *priv_ptr)
{
    if (vf)
        vf->priv = priv_ptr;
}

int a20_socket_file_vfile_ops_match(vfile_t *vf, vfile_ops_t *ops)
{
    return vf && vf->ops == ops;
}

void a20_socket_file_vfile_init(vfile_t *vf, vfile_ops_t *ops, void *priv_ptr, int flags)
{
    if (!vf)
        return;
    vf->flags = flags;
    vfile_ref_init(vf, 1);
    vf->ops = ops;
    vf->priv = priv_ptr;
}
