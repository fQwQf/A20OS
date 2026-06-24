#include <core/lock.h>
#include <core/string.h>
#include <fs/vfs.h>
#include <proc/proc.h>

void a20_vnode_key(void *vn, void **mnt_out, uint64_t *ino_out)
{
    vnode_t *v = (vnode_t *)vn;
    if (!v || !mnt_out || !ino_out)
        return;
    *mnt_out = v->mnt;
    *ino_out = v->ino;
}

int a20_vnode_stat_op(void *vn, kstat_t *st)
{
    vnode_t *v = (vnode_t *)vn;
    if (!v || !st)
        return -EINVAL;
    if (v->ops && v->ops->stat)
        return v->ops->stat(v, st);
    memset(st, 0, sizeof(*st));
    st->st_ino = v->ino;
    st->st_mode = v->mode;
    st->st_uid = v->uid;
    st->st_gid = v->gid;
    st->st_size = v->size;
    st->st_nlink = 1;
    return 0;
}

typedef struct {
    int uid;
    int gid;
    int egid;
    int fsgid;
    int fsuid;
    int ngroups;
    int groups[MAX_GROUPS];
} a20_vfs_cred_t;

void a20_proc_get_cred(void *t, a20_vfs_cred_t *out)
{
    task_t *task = (task_t *)t;
    if (!task || !out)
        return;
    out->uid = task->cred.uid;
    out->gid = task->cred.gid;
    out->egid = task->cred.egid;
    out->fsgid = task->cred.fsgid;
    out->fsuid = task->cred.fsuid;
    out->ngroups = task->cred.ngroups;
    int n = out->ngroups;
    if (n > MAX_GROUPS)
        n = MAX_GROUPS;
    for (int i = 0; i < n; i++)
        out->groups[i] = task->cred.groups[i];
}
