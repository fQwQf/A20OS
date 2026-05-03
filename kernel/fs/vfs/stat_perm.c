#include "fs/vfs/stat_perm.h"
#include "core/string.h"
#include "core/timekeeping.h"

#define VFS_TIME_META_MAX 8192

typedef struct {
    int used;
    void *mnt;
    uint64_t ino;
    uint64_t atime;
    uint64_t atime_nsec;
    uint64_t mtime;
    uint64_t mtime_nsec;
    uint64_t ctime;
    uint64_t ctime_nsec;
} vfs_time_meta_t;

static vfs_time_meta_t g_time_meta[VFS_TIME_META_MAX];

void fill_char_kstat(kstat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_nlink = 1;
    st->st_blksize = 4096;
}

void fill_pipe_kstat(kstat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFIFO | 0600;
    task_t *cur = proc_current();
    st->st_uid = cur ? (uint32_t)cur->cred.uid : 0;
    st->st_gid = cur ? (uint32_t)cur->cred.gid : 0;
    st->st_nlink = 1;
    st->st_blksize = 4096;
}

int vfs_task_in_group(task_t *t, uint32_t gid)
{
    if (!t)
        return gid == 0;
    if ((uint32_t)t->cred.fsgid == gid ||
        (uint32_t)t->cred.egid == gid ||
        (uint32_t)t->cred.gid == gid)
        return 1;
    for (int i = 0; i < t->cred.ngroups && i < MAX_GROUPS; i++) {
        if ((uint32_t)t->cred.groups[i] == gid)
            return 1;
    }
    return 0;
}

static int vfs_ids_in_group(task_t *t, uint32_t primary_gid, uint32_t gid)
{
    if (primary_gid == gid)
        return 1;
    if (!t)
        return gid == 0;
    for (int i = 0; i < t->cred.ngroups && i < MAX_GROUPS; i++) {
        if ((uint32_t)t->cred.groups[i] == gid)
            return 1;
    }
    return 0;
}

int vfs_mode_has_perm_ids(uint32_t st_mode, uint32_t file_uid,
                          uint32_t file_gid, uint32_t uid,
                          uint32_t gid, int mask)
{
    task_t *cur = proc_current();
    if (mask == F_OK)
        return 0;

    if (proc_has_cap(cur, CAP_DAC_OVERRIDE)) {
        if ((mask & X_OK) && ((st_mode & S_IFMT) == S_IFREG) &&
            !(st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
            return -EACCES;
        return 0;
    }

    if (!(mask & W_OK) && proc_has_cap(cur, CAP_DAC_READ_SEARCH)) {
        if ((mask & X_OK) && ((st_mode & S_IFMT) == S_IFREG) &&
            !(st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
            return -EACCES;
        return 0;
    }

    int shift = 0;
    if (uid == file_uid)
        shift = 6;
    else if (vfs_ids_in_group(cur, gid, file_gid))
        shift = 3;

    uint32_t need = 0;
    if (mask & R_OK)
        need |= 4;
    if (mask & W_OK)
        need |= 2;
    if (mask & X_OK)
        need |= 1;
    return (((st_mode >> shift) & need) == need) ? 0 : -EACCES;
}

static int vfs_mode_has_perm(uint32_t st_mode, uint32_t uid,
                             uint32_t gid, int mask)
{
    task_t *cur = proc_current();
    return vfs_mode_has_perm_ids(st_mode, uid, gid,
                                 cur ? (uint32_t)cur->cred.fsuid : 0,
                                 cur ? (uint32_t)cur->cred.fsgid : 0,
                                 mask);
}

int vfs_vnode_stat(vnode_t *vn, kstat_t *st)
{
    if (!vn || !st)
        return -EINVAL;
    int r = 0;
    if (vn->ops && vn->ops->stat) {
        r = vn->ops->stat(vn, st);
        if (r < 0)
            return r;
    } else {
        memset(st, 0, sizeof(*st));
        st->st_ino = vn->ino;
        st->st_mode = vn->mode;
        st->st_uid = vn->uid;
        st->st_gid = vn->gid;
        st->st_size = vn->size;
        st->st_nlink = 1;
    }
    for (int i = 0; i < VFS_TIME_META_MAX; i++) {
        if (g_time_meta[i].used && g_time_meta[i].mnt == vn->mnt &&
            g_time_meta[i].ino == vn->ino) {
            st->st_atime = g_time_meta[i].atime;
            st->st_atime_nsec = g_time_meta[i].atime_nsec;
            st->st_mtime = g_time_meta[i].mtime;
            st->st_mtime_nsec = g_time_meta[i].mtime_nsec;
            st->st_ctime = g_time_meta[i].ctime;
            st->st_ctime_nsec = g_time_meta[i].ctime_nsec;
            break;
        }
    }
    if (st->st_atime == 0 && st->st_mtime == 0 && st->st_ctime == 0) {
        uint64_t now[2];
        timekeeping_get_realtime(now);
        st->st_atime = now[0];
        st->st_atime_nsec = now[1];
        st->st_mtime = now[0];
        st->st_mtime_nsec = now[1];
        st->st_ctime = now[0];
        st->st_ctime_nsec = now[1];
    }
    return 0;
}

int vfs_vnode_permission(vnode_t *vn, int mask)
{
    kstat_t st;
    int r = vfs_vnode_stat(vn, &st);
    if (r < 0)
        return r;
    return vfs_mode_has_perm(st.st_mode, st.st_uid, st.st_gid, mask);
}

int vfs_current_owns(vnode_t *vn)
{
    task_t *cur = proc_current();
    if (proc_has_cap(cur, CAP_FOWNER))
        return 1;
    kstat_t st;
    if (vfs_vnode_stat(vn, &st) < 0)
        return 0;
    return cur && (uint32_t)cur->cred.fsuid == st.st_uid;
}

int vfs_sticky_may_remove(vnode_t *dir, vnode_t *victim)
{
    task_t *cur = proc_current();
    if (!dir || !victim || proc_has_cap(cur, CAP_FOWNER))
        return 0;

    kstat_t dst;
    kstat_t vst;
    if (vfs_vnode_stat(dir, &dst) < 0 || !(dst.st_mode & S_ISVTX))
        return 0;
    if (vfs_vnode_stat(victim, &vst) < 0)
        return 0;

    uint32_t fsuid = cur ? (uint32_t)cur->cred.fsuid : 0;
    if (fsuid == dst.st_uid || fsuid == vst.st_uid)
        return 0;
    return -EPERM;
}

static vfs_time_meta_t *vfs_time_meta_for(vnode_t *vn)
{
    if (!vn)
        return NULL;
    for (int i = 0; i < VFS_TIME_META_MAX; i++) {
        if (g_time_meta[i].used && g_time_meta[i].mnt == vn->mnt &&
            g_time_meta[i].ino == vn->ino)
            return &g_time_meta[i];
    }
    for (int i = 0; i < VFS_TIME_META_MAX; i++) {
        if (!g_time_meta[i].used) {
            memset(&g_time_meta[i], 0, sizeof(g_time_meta[i]));
            g_time_meta[i].used = 1;
            g_time_meta[i].mnt = vn->mnt;
            g_time_meta[i].ino = vn->ino;
            return &g_time_meta[i];
        }
    }
    return NULL;
}

void vfs_touch_mtime(vnode_t *vn)
{
    vfs_time_meta_t *tm = vfs_time_meta_for(vn);
    if (!tm)
        return;
    uint64_t now[2];
    timekeeping_get_realtime(now);
    if (tm->atime == 0 && tm->atime_nsec == 0) {
        tm->atime = now[0];
        tm->atime_nsec = now[1];
    }
    tm->mtime = now[0];
    tm->mtime_nsec = now[1];
    tm->ctime = now[0];
    tm->ctime_nsec = now[1];
}

int vfs_set_times(vnode_t *vn, const uint64_t times[4])
{
    vfs_time_meta_t *tm = vfs_time_meta_for(vn);
    if (!tm)
        return -ENOSPC;

    uint64_t now[2];
    timekeeping_get_realtime(now);
    uint64_t atime = now[0], atime_nsec = now[1];
    uint64_t mtime = now[0], mtime_nsec = now[1];
    if (times) {
        atime = times[0];
        atime_nsec = times[1];
        mtime = times[2];
        mtime_nsec = times[3];
    }
    if (atime_nsec >= 1000000000ULL || mtime_nsec >= 1000000000ULL)
        return -EINVAL;

    tm->atime = atime;
    tm->atime_nsec = atime_nsec;
    tm->mtime = mtime;
    tm->mtime_nsec = mtime_nsec;
    tm->ctime = now[0];
    tm->ctime_nsec = now[1];
    return 0;
}
