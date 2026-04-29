#include "fs/locks.h"

#include "core/consts.h"
#include "core/lock.h"
#include "core/string.h"
#include "proc/proc.h"

typedef struct fs_file_lock {
    int used;
    int owner_kind;
    uintptr_t key;
    int owner;
    short type;
    int64_t start;
    int64_t end;
} fs_file_lock_t;

typedef struct fs_bsd_flock {
    int used;
    uintptr_t key;
    vfile_t *owner;
    int type;
} fs_bsd_flock_t;

#define FS_FILE_LOCK_MAX 256

static fs_file_lock_t g_file_locks[FS_FILE_LOCK_MAX];
static fs_bsd_flock_t g_bsd_flocks[FS_FILE_LOCK_MAX];
static spinlock_t g_file_lock_table_lock = SPINLOCK_INIT;

static uintptr_t fs_lock_key(vfile_t *vf)
{
    if (vf && vf->vnode && vf->vnode->ino)
        return (((uintptr_t)vf->vnode->mnt) >> 3) ^
               ((uintptr_t)vf->vnode->ino << 17) ^
               (uintptr_t)vf->vnode->ino;
    return (uintptr_t)vf;
}

static int64_t fs_file_size(vfile_t *vf)
{
    if (!vf || !vf->vnode) return 0;
    kstat_t st;
    memset(&st, 0, sizeof(st));
    if (vf->vnode->ops && vf->vnode->ops->stat &&
        vf->vnode->ops->stat(vf->vnode, &st) == 0)
        return (int64_t)st.st_size;
    return (int64_t)vf->vnode->size;
}

static int fs_lock_range(vfile_t *vf, const fs_flock_t *lk,
                         int64_t *start, int64_t *end)
{
    int64_t base;
    if (!vf || !lk || !start || !end) return -EINVAL;
    if (lk->l_whence == SEEK_SET) base = 0;
    else if (lk->l_whence == SEEK_CUR) base = (int64_t)vf->offset;
    else if (lk->l_whence == SEEK_END) base = fs_file_size(vf);
    else return -EINVAL;

    int64_t s = base + lk->l_start;
    int64_t e;
    if (lk->l_len == 0) {
        e = 0x7fffffffffffffffLL;
    } else if (lk->l_len > 0) {
        e = s + lk->l_len - 1;
    } else {
        e = s - 1;
        s = s + lk->l_len;
    }
    if (s < 0) return -EINVAL;
    *start = s;
    *end = e;
    return 0;
}

static int fs_lock_overlaps(int64_t a0, int64_t a1, int64_t b0, int64_t b1)
{
    return a0 <= b1 && b0 <= a1;
}

static int fs_lock_conflicts(const fs_file_lock_t *held, uintptr_t key,
                             int owner_kind, int owner, short type,
                             int64_t start, int64_t end)
{
    if (!held->used || held->key != key)
        return 0;
    if (held->owner_kind == owner_kind && held->owner == owner)
        return 0;
    if (!fs_lock_overlaps(held->start, held->end, start, end))
        return 0;
    if (held->type == F_RDLCK && type == F_RDLCK)
        return 0;
    return 1;
}

int fs_locks_get(vfile_t *vf, fs_flock_t *lk, int owner_kind, int owner)
{
    if (!lk) return -EINVAL;
    if (lk->l_type != F_RDLCK && lk->l_type != F_WRLCK && lk->l_type != F_UNLCK)
        return -EINVAL;

    int64_t start, end;
    int r = fs_lock_range(vf, lk, &start, &end);
    if (r < 0) return r;
    uintptr_t key = fs_lock_key(vf);

    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (!fs_lock_conflicts(&g_file_locks[i], key, owner_kind, owner,
                               lk->l_type, start, end))
            continue;
        lk->l_type = g_file_locks[i].type;
        lk->l_whence = SEEK_SET;
        lk->l_start = g_file_locks[i].start;
        lk->l_len = (g_file_locks[i].end == 0x7fffffffffffffffLL) ?
                    0 : (g_file_locks[i].end - g_file_locks[i].start + 1);
        lk->l_pid = g_file_locks[i].owner;
        spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);

    lk->l_type = F_UNLCK;
    lk->l_pid = owner;
    return 0;
}

int fs_locks_set(vfile_t *vf, const fs_flock_t *lk, int owner_kind, int owner, int wait)
{
    if (!lk) return -EINVAL;
    if (lk->l_type != F_RDLCK && lk->l_type != F_WRLCK && lk->l_type != F_UNLCK)
        return -EINVAL;

    int64_t start, end;
    int r = fs_lock_range(vf, lk, &start, &end);
    if (r < 0) return r;
    uintptr_t key = fs_lock_key(vf);

retry:
    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (!fs_lock_conflicts(&g_file_locks[i], key, owner_kind, owner,
                               lk->l_type, start, end))
            continue;
        spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
        if (!wait) return -EAGAIN;
        proc_yield();
        goto retry;
    }

    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (!g_file_locks[i].used || g_file_locks[i].key != key ||
            g_file_locks[i].owner_kind != owner_kind ||
            g_file_locks[i].owner != owner)
            continue;
        if (fs_lock_overlaps(g_file_locks[i].start, g_file_locks[i].end, start, end))
            g_file_locks[i].used = 0;
    }

    if (lk->l_type == F_UNLCK) {
        spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
        return 0;
    }

    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (!g_file_locks[i].used) {
            g_file_locks[i].used = 1;
            g_file_locks[i].owner_kind = owner_kind;
            g_file_locks[i].key = key;
            g_file_locks[i].owner = owner;
            g_file_locks[i].type = lk->l_type;
            g_file_locks[i].start = start;
            g_file_locks[i].end = end;
            spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
    return -ENOLCK;
}

void fs_locks_release_process(int pid)
{
    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (g_file_locks[i].used &&
            g_file_locks[i].owner_kind == FS_LOCK_OWNER_PID &&
            g_file_locks[i].owner == pid)
            g_file_locks[i].used = 0;
    }
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
}

void fs_locks_release_file(vfile_t *vf, int gfd)
{
    uintptr_t key = fs_lock_key(vf);
    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (g_file_locks[i].used &&
            g_file_locks[i].owner_kind == FS_LOCK_OWNER_OFD &&
            g_file_locks[i].owner == gfd)
            g_file_locks[i].used = 0;
        if (g_bsd_flocks[i].used &&
            g_bsd_flocks[i].key == key &&
            g_bsd_flocks[i].owner == vf)
            g_bsd_flocks[i].used = 0;
    }
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
}

int fs_flocks_apply(vfile_t *vf, int operation)
{
    if (!vf) return -EBADF;
    int op = operation & (LOCK_SH | LOCK_EX | LOCK_UN);
    if ((operation & ~(LOCK_SH | LOCK_EX | LOCK_NB | LOCK_UN)) || op == 0)
        return -EINVAL;
    if ((op & (op - 1)) != 0)
        return -EINVAL;

    uintptr_t key = fs_lock_key(vf);
    if (op == LOCK_UN) {
        uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
        for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
            if (g_bsd_flocks[i].used &&
                g_bsd_flocks[i].key == key &&
                g_bsd_flocks[i].owner == vf)
                g_bsd_flocks[i].used = 0;
        }
        spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
        return 0;
    }

    int type = (op == LOCK_EX) ? F_WRLCK : F_RDLCK;

retry:
    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (!g_bsd_flocks[i].used || g_bsd_flocks[i].key != key ||
            g_bsd_flocks[i].owner == vf)
            continue;
        if (g_bsd_flocks[i].type == F_RDLCK && type == F_RDLCK)
            continue;
        spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
        if (operation & LOCK_NB) return -EAGAIN;
        proc_yield();
        goto retry;
    }

    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (g_bsd_flocks[i].used &&
            g_bsd_flocks[i].key == key &&
            g_bsd_flocks[i].owner == vf) {
            g_bsd_flocks[i].type = type;
            spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
            return 0;
        }
    }

    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (!g_bsd_flocks[i].used) {
            g_bsd_flocks[i].used = 1;
            g_bsd_flocks[i].key = key;
            g_bsd_flocks[i].owner = vf;
            g_bsd_flocks[i].type = type;
            spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
    return -ENOLCK;
}
