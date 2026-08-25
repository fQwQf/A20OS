#include "fs/quota.h"

#include "core/lock.h"
#include "core/string.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "fs/vfs/mount.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

#ifndef EDQUOT
#define EDQUOT 122
#endif

#define QMOUNT_MAX      8
#define DQENT_MAX       256

typedef struct {
    int used;
    uint32_t id;
    uint64_t bhard;
    uint64_t bsoft;
    uint64_t curspace;
    uint64_t ihard;
    uint64_t isoft;
    uint64_t curinodes;
} dqent_t;

typedef struct {
    mount_t *mnt;
    spinlock_t lock;
    int enabled[MAXQUOTAS];
    dqent_t ents[MAXQUOTAS][DQENT_MAX];
} qmount_t;

static qmount_t g_qmounts[QMOUNT_MAX];
static spinlock_t g_quota_global_lock;
static int g_quota_ready;

static const uint64_t DEFAULT_GRACE = 7 * 24 * 3600;

int quota_init(void)
{
    memset(g_qmounts, 0, sizeof(g_qmounts));
    g_quota_ready = 1;
    return 0;
}

static qmount_t *qmount_lookup(mount_t *mnt, int create)
{
    if (!g_quota_ready || !mnt)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&g_quota_global_lock);
    qmount_t *free_slot = NULL;
    for (int i = 0; i < QMOUNT_MAX; i++) {
        if (g_qmounts[i].mnt == mnt) {
            spin_unlock_irqrestore(&g_quota_global_lock, flags);
            return &g_qmounts[i];
        }
        if (!g_qmounts[i].mnt && !free_slot)
            free_slot = &g_qmounts[i];
    }
    if (!create || !free_slot) {
        spin_unlock_irqrestore(&g_quota_global_lock, flags);
        return NULL;
    }
    free_slot->mnt = mnt;
    spin_unlock_irqrestore(&g_quota_global_lock, flags);
    return free_slot;
}

static mount_t *quota_mount_of_vnode(vnode_t *vn)
{
    return vn ? vn->mnt : NULL;
}

static dqent_t *dqent_find_locked(qmount_t *qm, int type, uint32_t id,
                                  int create)
{
    dqent_t *free_ent = NULL;
    for (int i = 0; i < DQENT_MAX; i++) {
        dqent_t *e = &qm->ents[type][i];
        if (e->used && e->id == id)
            return e;
        if (!e->used && !free_ent)
            free_ent = e;
    }
    if (!create)
        return NULL;
    if (!free_ent)
        return NULL;
    memset(free_ent, 0, sizeof(*free_ent));
    free_ent->used = 1;
    free_ent->id = id;
    return free_ent;
}

static int quota_owner_ids(vnode_t *vn, uint32_t *uid, uint32_t *gid)
{
    task_t *cur = proc_current();
    *uid = vn && vn->uid ? vn->uid : (cur ? (uint32_t)cur->cred.euid : 0);
    *gid = vn && vn->gid ? vn->gid : (cur ? (uint32_t)cur->cred.egid : 0);
    return 0;
}

int quota_check_space(vnode_t *vn, uint64_t extra_bytes)
{
    mount_t *mnt = quota_mount_of_vnode(vn);
    qmount_t *qm = mnt ? qmount_lookup(mnt, 0) : NULL;
    if (!qm || extra_bytes == 0)
        return 0;

    uint32_t uid = 0, gid = 0;
    quota_owner_ids(vn, &uid, &gid);

    int denied = 0;
    uint64_t flags = spin_lock_irqsave(&qm->lock);
    for (int type = 0; type < MAXQUOTAS && !denied; type++) {
        if (!qm->enabled[type])
            continue;
        dqent_t *e = dqent_find_locked(qm, type, type == USRQUOTA ? uid : gid,
                                       0);
        if (!e)
            continue;
        uint64_t hard = e->bhard;
        if (!hard)
            continue;
        if (e->curspace >= hard ||
            extra_bytes > hard - e->curspace)
            denied = EDQUOT;
    }
    spin_unlock_irqrestore(&qm->lock, flags);
    return denied;
}

int quota_check_inode_mnt(mount_t *mnt, uint32_t uid, uint32_t gid)
{
    qmount_t *qm = qmount_lookup(mnt, 0);
    if (!qm)
        return 0;

    int denied = 0;
    uint64_t flags = spin_lock_irqsave(&qm->lock);
    for (int type = 0; type < MAXQUOTAS && !denied; type++) {
        if (!qm->enabled[type])
            continue;
        dqent_t *e = dqent_find_locked(qm, type, type == USRQUOTA ? uid : gid,
                                       0);
        if (!e)
            continue;
        uint64_t hard = e->ihard;
        if (!hard)
            continue;
        if (e->curinodes >= hard)
            denied = EDQUOT;
    }
    spin_unlock_irqrestore(&qm->lock, flags);
    return denied;
}

void quota_account_space(vnode_t *vn, int64_t bytes_delta)
{
    mount_t *mnt = quota_mount_of_vnode(vn);
    qmount_t *qm = mnt ? qmount_lookup(mnt, 0) : NULL;
    if (!qm || bytes_delta == 0)
        return;

    uint32_t uid = 0, gid = 0;
    quota_owner_ids(vn, &uid, &gid);

    uint64_t flags = spin_lock_irqsave(&qm->lock);
    for (int type = 0; type < MAXQUOTAS; type++) {
        if (!qm->enabled[type])
            continue;
        dqent_t *e = dqent_find_locked(qm, type, type == USRQUOTA ? uid : gid,
                                       1);
        if (!e)
            continue;
        if (bytes_delta < 0 && (uint64_t)(-bytes_delta) > e->curspace)
            e->curspace = 0;
        else
            e->curspace += (uint64_t)bytes_delta;
    }
    spin_unlock_irqrestore(&qm->lock, flags);
}

void quota_account_inode_create(vnode_t *vn)
{
    mount_t *mnt = quota_mount_of_vnode(vn);
    qmount_t *qm = mnt ? qmount_lookup(mnt, 0) : NULL;
    if (!qm)
        return;

    uint32_t uid = 0, gid = 0;
    quota_owner_ids(vn, &uid, &gid);

    uint64_t flags = spin_lock_irqsave(&qm->lock);
    for (int type = 0; type < MAXQUOTAS; type++) {
        if (!qm->enabled[type])
            continue;
        dqent_t *e = dqent_find_locked(qm, type, type == USRQUOTA ? uid : gid,
                                       1);
        if (e)
            e->curinodes++;
    }
    spin_unlock_irqrestore(&qm->lock, flags);
}

void quota_account_inode_ids(mount_t *mnt, uint32_t uid, uint32_t gid)
{
    qmount_t *qm = qmount_lookup(mnt, 0);
    if (!qm)
        return;

    uint64_t flags = spin_lock_irqsave(&qm->lock);
    for (int type = 0; type < MAXQUOTAS; type++) {
        if (!qm->enabled[type])
            continue;
        dqent_t *e = dqent_find_locked(qm, type, type == USRQUOTA ? uid : gid,
                                       1);
        if (e)
            e->curinodes++;
    }
    spin_unlock_irqrestore(&qm->lock, flags);
}

void quota_account_inode_remove(vnode_t *vn, uint64_t freed_bytes)
{
    mount_t *mnt = quota_mount_of_vnode(vn);
    qmount_t *qm = mnt ? qmount_lookup(mnt, 0) : NULL;
    if (!qm)
        return;

    uint32_t uid = vn->uid, gid = vn->gid;
    uint64_t flags = spin_lock_irqsave(&qm->lock);
    for (int type = 0; type < MAXQUOTAS; type++) {
        if (!qm->enabled[type])
            continue;
        dqent_t *e = dqent_find_locked(qm, type, type == USRQUOTA ? uid : gid,
                                       0);
        if (!e)
            continue;
        if (e->curinodes > 0)
            e->curinodes--;
        if (freed_bytes) {
            if (freed_bytes > e->curspace)
                e->curspace = 0;
            else
                e->curspace -= freed_bytes;
        }
    }
    spin_unlock_irqrestore(&qm->lock, flags);
}

static mount_t *quota_find_special(const char *special)
{
    if (!special)
        return NULL;
    for (int i = 0; i < vfs_mount_count(); i++) {
        mount_t *m = vfs_mount_at(i);
        if (!m)
            continue;
        if ((m->dev[0] && strcmp(m->dev, special) == 0) ||
            (m->path[0] && strcmp(m->path, special) == 0))
            return m;
    }
    return NULL;
}

static void dqent_to_dqblk(const dqent_t *e, struct if_dqblk *out)
{
    memset(out, 0, sizeof(*out));
    out->dqb_bhardlimit = e->bhard;
    out->dqb_bsoftlimit = e->bsoft;
    out->dqb_curspace = e->curspace;
    out->dqb_ihardlimit = e->ihard;
    out->dqb_isoftlimit = e->isoft;
    out->dqb_curinodes = e->curinodes;
    out->dqb_valid = QIF_BLIMITS | QIF_SPACE | QIF_ILIMITS | QIF_INODES;
}

static void dqblk_to_dqent(dqent_t *e, const struct if_dqblk *in)
{
    if (in->dqb_valid & QIF_BLIMITS) {
        e->bhard = in->dqb_bhardlimit;
        e->bsoft = in->dqb_bsoftlimit;
    }
    if (in->dqb_valid & QIF_ILIMITS) {
        e->ihard = in->dqb_ihardlimit;
        e->isoft = in->dqb_isoftlimit;
    }
}

static int quota_cmd_on_mnt(mount_t *mnt, uint32_t base_cmd, int type,
                            uint32_t id, void *addr)
{
    qmount_t *qm = qmount_lookup(mnt, base_cmd != Q_GETFMT &&
                                      base_cmd != Q_SYNC &&
                                      base_cmd != Q_GETINFO &&
                                      base_cmd != Q_QUOTAOFF);
    if (!qm)
        return -ESRCH;
    if (type < 0 || type >= MAXQUOTAS)
        return -EINVAL;

    switch (base_cmd) {
    case Q_SYNC:
        /* Accounting is synchronous; nothing to flush to disk yet. */
        return 0;
    case Q_QUOTAON: {
        uint64_t flags = spin_lock_irqsave(&qm->lock);
        qm->enabled[type] = 1;
        spin_unlock_irqrestore(&qm->lock, flags);
        return 0;
    }
    case Q_QUOTAOFF: {
        uint64_t flags = spin_lock_irqsave(&qm->lock);
        qm->enabled[type] = 0;
        spin_unlock_irqrestore(&qm->lock, flags);
        return 0;
    }
    case Q_GETFMT: {
        if (!addr)
            return -EFAULT;
        uint32_t fmt = QFMT_VFS_V0;
        return copy_to_user(addr, &fmt, sizeof(fmt)) < 0 ? -EFAULT : 0;
    }
    case Q_GETINFO: {
        if (!addr)
            return -EFAULT;
        struct if_dqinfo info;
        memset(&info, 0, sizeof(info));
        info.dqi_bgrace = DEFAULT_GRACE;
        info.dqi_igrace = DEFAULT_GRACE;
        info.dqi_valid = QIF_BTIME | QIF_ITIME;
        return copy_to_user(addr, &info, sizeof(info)) < 0 ? -EFAULT : 0;
    }
    case Q_SETINFO:
        /* Grace times and DQ_* flags are stored per-file in Linux; the
         * kernel-side model uses fixed defaults and ignores updates. */
        return 0;
    case Q_GETQUOTA: {
        if (!addr)
            return -EFAULT;
        struct if_dqblk blk;
        uint64_t flags = spin_lock_irqsave(&qm->lock);
        dqent_t *e = dqent_find_locked(qm, type, id, 0);
        if (e)
            dqent_to_dqblk(e, &blk);
        else
            memset(&blk, 0, sizeof(blk));
        spin_unlock_irqrestore(&qm->lock, flags);
        return copy_to_user(addr, &blk, sizeof(blk)) < 0 ? -EFAULT : 0;
    }
    case Q_SETQUOTA: {
        if (!addr)
            return -EFAULT;
        task_t *cur = proc_current();
        if (!cur || !proc_has_cap(cur, 16 /*CAP_SYS_ADMIN*/))
            return -EPERM;
        struct if_dqblk blk;
        if (copy_from_user(&blk, addr, sizeof(blk)) < 0)
            return -EFAULT;
        uint64_t flags = spin_lock_irqsave(&qm->lock);
        dqent_t *e = dqent_find_locked(qm, type, id, 1);
        if (!e) {
            spin_unlock_irqrestore(&qm->lock, flags);
            return -ENOMEM;
        }
        dqblk_to_dqent(e, &blk);
        e->curspace = blk.dqb_curspace;
        e->curinodes = blk.dqb_curinodes;
        spin_unlock_irqrestore(&qm->lock, flags);
        return 0;
    }
    case Q_GETNEXTQUOTA: {
        if (!addr)
            return -EFAULT;
        struct {
            struct if_dqblk blk;
            uint32_t dqb_id;
        } next;
        memset(&next, 0, sizeof(next));
        int found = 0;
        uint64_t flags = spin_lock_irqsave(&qm->lock);
        for (int i = 0; i < DQENT_MAX && !found; i++) {
            dqent_t *e = &qm->ents[type][i];
            if (e->used && e->id >= id) {
                dqent_to_dqblk(e, &next.blk);
                next.dqb_id = e->id;
                found = 1;
            }
        }
        spin_unlock_irqrestore(&qm->lock, flags);
        if (!found)
            return -ENOENT;
        return copy_to_user(addr, &next, sizeof(next)) < 0 ? -EFAULT : 0;
    }
    default:
        return -EINVAL;
    }
}

int quota_cmd(uint32_t cmd, const char *special, uint32_t id, void *addr)
{
    int type = (int)(cmd & SUBCMDMASK);
    uint32_t base = cmd >> SUBCMDSHIFT;

    mount_t *mnt = quota_find_special(special);
    if (!mnt)
        return base == Q_SYNC ? 0 : -ENODEV;
    if (base < 0x800001U || base > 0x800009U)
        return -EINVAL;
    return quota_cmd_on_mnt(mnt, base, type, id, addr);
}

int quota_cmd_fd(int fd, uint32_t cmd, uint32_t id, void *addr)
{
    int type = (int)(cmd & SUBCMDMASK);
    uint32_t base = cmd >> SUBCMDSHIFT;
    if (base < 0x800001U || base > 0x800009U)
        return -EINVAL;

    int gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return gfd;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return -EBADF;
    mount_t *mnt = vf->vnode ? vf->vnode->mnt : NULL;
    vfs_put_file_ref(gfd, vf);
    if (!mnt)
        return -ENODEV;
    return quota_cmd_on_mnt(mnt, base, type, id, addr);
}
