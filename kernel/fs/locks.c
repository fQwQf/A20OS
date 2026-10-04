#include "fs/locks.h"

#include "core/consts.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/string.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/signal.h"

typedef struct fs_file_lock {
    int used;
    int owner_kind;
    uintptr_t key;
    uintptr_t owner;
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
/* Subtracting the requested range can split every range this owner holds on
 * the file in two, and the requested range itself is appended, so the algebra
 * needs room for twice the table plus one. */
#define FS_LOCK_SCRATCH_MAX (FS_FILE_LOCK_MAX * 2 + 1)

typedef struct {
    int64_t start;
    int64_t end;
    int16_t type;
} fs_lock_range_t;

/*
 * The range algebra used to run over ~23 KiB of per-call stack arrays inside
 * the g_file_lock_table_lock region — a third of a thread's 64 KiB kernel
 * stack in one frame.  The scratch is heap-allocated before the lock is taken
 * instead: a reclaiming kmalloc must never run under a spinlock, and the
 * scan-and-split below only reads the table, so it can be prepared first.
 */
static fs_lock_range_t *fs_lock_scratch_alloc(void)
{
    return (fs_lock_range_t *)kmalloc(sizeof(fs_lock_range_t) *
                                      FS_LOCK_SCRATCH_MAX);
}

static fs_file_lock_t g_file_locks[FS_FILE_LOCK_MAX];
static fs_bsd_flock_t g_bsd_flocks[FS_FILE_LOCK_MAX];
static spinlock_t g_file_lock_table_lock = SPINLOCK_INIT;
static wait_queue_t g_file_lock_waiters = WAIT_QUEUE_INIT;
static uint64_t g_file_lock_generation;

static int fs_lock_wait(uint64_t table_flags)
{
    task_t *cur = proc_current();
    if (!cur) {
        spin_unlock_irqrestore(&g_file_lock_table_lock, table_flags);
        proc_yield();
        return 0;
    }
    if (signal_task_has_unblocked(cur)) {
        spin_unlock_irqrestore(&g_file_lock_table_lock, table_flags);
        return -ERESTARTSYS;
    }

    uint64_t generation = g_file_lock_generation;
    spin_unlock_irqrestore(&g_file_lock_table_lock, table_flags);
    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
    if (!token.task)
        return -EAGAIN;

    wait_queue_entry_t entry = {0};
    table_flags = spin_lock_irqsave(&g_file_lock_table_lock);
    if (generation != g_file_lock_generation) {
        spin_unlock_irqrestore(&g_file_lock_table_lock, table_flags);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        return 0;
    }
    bool linked =
        wait_queue_link(&g_file_lock_waiters, &entry, token, 0);
    spin_unlock_irqrestore(&g_file_lock_table_lock, table_flags);

    proc_wake_reason_t reason;
    if (linked)
        reason = proc_park_commit(token);
    else {
        (void)proc_park_cancel(token);
        reason = PROC_WAKE_CANCEL;
    }
    wait_queue_unlink(&g_file_lock_waiters, &entry);
    proc_park_finish(token);
    if (proc_wake_reason_is_task_interrupt(reason) ||
        signal_task_has_unblocked(cur))
        return -ERESTARTSYS;
    /* Neither a park failure nor a failed enqueue may be reported as "keep
     * retrying": the caller re-enters the table scan under the spinlock, so a
     * 0 here burns the whole retry budget without ever sleeping. */
    if (!linked)
        return -EAGAIN;
    return 0;
}

static void fs_lock_wake_waiters(void)
{
    wait_queue_wake_all(&g_file_lock_waiters, 0, PROC_WAKE_EVENT);
}

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
                             int owner_kind, uintptr_t owner, short type,
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

int fs_locks_get(vfile_t *vf, fs_flock_t *lk, int owner_kind, uintptr_t owner)
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
        lk->l_pid = (g_file_locks[i].owner_kind == FS_LOCK_OWNER_PID) ?
                    (int)g_file_locks[i].owner : -1;
        spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);

    lk->l_type = F_UNLCK;
    return 0;
}

int fs_locks_set(vfile_t *vf, const fs_flock_t *lk, int owner_kind,
                 uintptr_t owner, int wait)
{
    if (!lk) return -EINVAL;
    if (lk->l_type != F_RDLCK && lk->l_type != F_WRLCK && lk->l_type != F_UNLCK)
        return -EINVAL;

    int64_t start, end;
    int r = fs_lock_range(vf, lk, &start, &end);
    if (r < 0) return r;
    uintptr_t key = fs_lock_key(vf);

    fs_lock_range_t *sc = fs_lock_scratch_alloc();
    if (!sc) return -ENOMEM;

    /* Deadlock detection: track the conflicting holder across retries.
     * If we repeatedly conflict with the same holder who also blocks
     * on a lock we own, that's a deadlock cycle. */
    uintptr_t prev_blocker = 0;
    int deadlock_retries = 0;
#define FS_LOCK_MAX_RETRIES 1024

retry:
    if (deadlock_retries >= FS_LOCK_MAX_RETRIES) {
        kfree(sc);
        return -EDEADLK;
    }
    deadlock_retries++;

    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    int changed = 0;

    /* 1. Check if another process/owner conflicts with the new lock (only if we are NOT unlocking) */
    if (lk->l_type != F_UNLCK) {
        for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
            if (fs_lock_conflicts(&g_file_locks[i], key, owner_kind, owner,
                                   lk->l_type, start, end)) {
                if (!wait) {
                    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
                    kfree(sc);
                    return -EAGAIN;
                }
                /* Simple deadlock heuristic: if we keep conflicting
                 * with the same holder, check whether that holder is
                 * itself blocked on a lock we own. */
                uintptr_t blocker = g_file_locks[i].owner;
                if (blocker == prev_blocker && blocker) {
                    /* Check if the blocker is waiting on one of our locks */
                    for (int j = 0; j < FS_FILE_LOCK_MAX; j++) {
                        if (g_file_locks[j].used &&
                            g_file_locks[j].owner_kind == owner_kind &&
                            g_file_locks[j].owner == owner &&
                            g_file_locks[j].key != key) {
                            /* We hold a lock on a different file —
                             * could be part of a deadlock cycle */
                            if (deadlock_retries > 4) {
                                spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
                                kfree(sc);
                                return -EDEADLK;
                            }
                        }
                    }
                }
                prev_blocker = blocker;
                r = fs_lock_wait(flags);
                if (r < 0) {
                    kfree(sc);
                    return r;
                }
                goto retry;
            }
        }
    }

    /* 2. Collect the ranges this owner already holds on this file and
     * subtract [start, end] from each, leaving the non-overlapping left and
     * right remnants.  Nothing in the table is mutated yet, so an -ENOLCK
     * outcome below leaves the existing locks exactly as they were. */
    int own_count = 0;
    int count = 0;
    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (!g_file_locks[i].used || g_file_locks[i].key != key ||
            g_file_locks[i].owner_kind != owner_kind ||
            g_file_locks[i].owner != owner)
            continue;
        own_count++;
        int64_t s_L = g_file_locks[i].start;
        int64_t e_L = g_file_locks[i].end;
        int16_t t_L = g_file_locks[i].type;

        if (e_L < start || s_L > end) {
            sc[count].start = s_L;   /* No overlap */
            sc[count].end = e_L;
            sc[count].type = t_L;
            count++;
        } else {
            if (s_L < start) {
                sc[count].start = s_L;
                sc[count].end = start - 1;
                sc[count].type = t_L;
                count++;
            }
            if (e_L > end) {
                sc[count].start = end + 1;
                sc[count].end = e_L;
                sc[count].type = t_L;
                count++;
            }
        }
    }

    /* 3. Add the new lock range if it is not an unlock request */
    if (lk->l_type != F_UNLCK) {
        sc[count].start = start;
        sc[count].end = end;
        sc[count].type = (int16_t)lk->l_type;
        count++;
    }

    /* 4. Sort by start address to facilitate merging.  Insertion sort: the
     * remnants arrive in table order, which is already almost sorted. */
    for (int i = 1; i < count; i++) {
        fs_lock_range_t tmp = sc[i];
        int j = i - 1;
        while (j >= 0 && sc[j].start > tmp.start) {
            sc[j + 1] = sc[j];
            j--;
        }
        sc[j + 1] = tmp;
    }

    /* 5. Merge adjacent or overlapping locks of the same type, in place. */
    int merged_count = 0;
    for (int i = 0; i < count; i++) {
        if (merged_count > 0 &&
            sc[merged_count - 1].type == sc[i].type &&
            sc[merged_count - 1].end + 1 >= sc[i].start) {
            if (sc[i].end > sc[merged_count - 1].end)
                sc[merged_count - 1].end = sc[i].end;
        } else {
            sc[merged_count] = sc[i];
            merged_count++;
        }
    }

    /* 6. The result has to fit once this owner's old ranges are withdrawn. */
    int used = 0;
    for (int i = 0; i < FS_FILE_LOCK_MAX; i++)
        if (g_file_locks[i].used)
            used++;
    if (used - own_count + merged_count > FS_FILE_LOCK_MAX) {
        spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
        kfree(sc);
        return -ENOLCK;
    }

    /* 7. Withdraw this owner's old ranges and write the merged ones back. */
    if (own_count)
        changed = 1;
    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (g_file_locks[i].used && g_file_locks[i].key == key &&
            g_file_locks[i].owner_kind == owner_kind &&
            g_file_locks[i].owner == owner)
            g_file_locks[i].used = 0;
    }
    int write_idx = 0;
    for (int i = 0; i < FS_FILE_LOCK_MAX && write_idx < merged_count; i++) {
        if (!g_file_locks[i].used) {
            g_file_locks[i].used = 1;
            g_file_locks[i].key = key;
            g_file_locks[i].owner_kind = owner_kind;
            g_file_locks[i].owner = owner;
            g_file_locks[i].start = sc[write_idx].start;
            g_file_locks[i].end = sc[write_idx].end;
            g_file_locks[i].type = sc[write_idx].type;
            write_idx++;
        }
    }

    int wake_waiters = changed || lk->l_type == F_UNLCK;
    if (wake_waiters)
        g_file_lock_generation++;
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
    if (wake_waiters)
        fs_lock_wake_waiters();
    kfree(sc);
    return 0;
}

void fs_locks_release_process(int pid)
{
    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    int changed = 0;
    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (g_file_locks[i].used &&
            g_file_locks[i].owner_kind == FS_LOCK_OWNER_PID &&
            g_file_locks[i].owner == (uintptr_t)pid) {
            g_file_locks[i].used = 0;
            changed = 1;
        }
    }
    if (changed)
        g_file_lock_generation++;
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
    if (changed)
        fs_lock_wake_waiters();
}

void fs_locks_release_process_file(vfile_t *vf, int pid)
{
    uintptr_t key = fs_lock_key(vf);
    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    int changed = 0;
    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (g_file_locks[i].used &&
            g_file_locks[i].key == key &&
            g_file_locks[i].owner_kind == FS_LOCK_OWNER_PID &&
            g_file_locks[i].owner == (uintptr_t)pid) {
            g_file_locks[i].used = 0;
            changed = 1;
        }
    }
    if (changed)
        g_file_lock_generation++;
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
    if (changed)
        fs_lock_wake_waiters();
}

void fs_locks_release_file(vfile_t *vf, uintptr_t owner)
{
    uintptr_t key = fs_lock_key(vf);
    task_t *cur = proc_current();
    int cur_pid = cur ? cur->pid : -1;
    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    int changed = 0;
    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (g_file_locks[i].used &&
            g_file_locks[i].owner_kind == FS_LOCK_OWNER_OFD &&
            g_file_locks[i].owner == owner) {
            g_file_locks[i].used = 0;
            changed = 1;
        }
        if (g_file_locks[i].used &&
            g_file_locks[i].key == key &&
            g_file_locks[i].owner_kind == FS_LOCK_OWNER_PID &&
            g_file_locks[i].owner == (uintptr_t)cur_pid) {
            g_file_locks[i].used = 0;
            changed = 1;
        }
        if (g_bsd_flocks[i].used &&
            g_bsd_flocks[i].key == key &&
            g_bsd_flocks[i].owner == vf) {
            g_bsd_flocks[i].used = 0;
            changed = 1;
        }
    }
    if (changed)
        g_file_lock_generation++;
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
    if (changed)
        fs_lock_wake_waiters();
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
        int changed = 0;
        for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
            if (g_bsd_flocks[i].used &&
                g_bsd_flocks[i].key == key &&
                g_bsd_flocks[i].owner == vf) {
                g_bsd_flocks[i].used = 0;
                changed = 1;
            }
        }
        if (changed)
            g_file_lock_generation++;
        spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
        if (changed)
            fs_lock_wake_waiters();
        return 0;
    }

    int type = (op == LOCK_EX) ? F_WRLCK : F_RDLCK;

    int flock_retries = 0;
#define FS_FLOCK_MAX_RETRIES 1024

retry:
    if (flock_retries >= FS_FLOCK_MAX_RETRIES)
        return -EDEADLK;
    flock_retries++;

    uint64_t flags = spin_lock_irqsave(&g_file_lock_table_lock);
    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (!g_bsd_flocks[i].used || g_bsd_flocks[i].key != key ||
            g_bsd_flocks[i].owner == vf)
            continue;
        if (g_bsd_flocks[i].type == F_RDLCK && type == F_RDLCK)
            continue;
        if (operation & LOCK_NB) {
            spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
            return -EAGAIN;
        }
        int r = fs_lock_wait(flags);
        if (r < 0)
            return r;
        goto retry;
    }

    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (g_bsd_flocks[i].used &&
            g_bsd_flocks[i].key == key &&
            g_bsd_flocks[i].owner == vf) {
            g_bsd_flocks[i].type = type;
            g_file_lock_generation++;
            spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
            fs_lock_wake_waiters();
            return 0;
        }
    }

    for (int i = 0; i < FS_FILE_LOCK_MAX; i++) {
        if (!g_bsd_flocks[i].used) {
            g_bsd_flocks[i].used = 1;
            g_bsd_flocks[i].key = key;
            g_bsd_flocks[i].owner = vf;
            g_bsd_flocks[i].type = type;
            g_file_lock_generation++;
            spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
            fs_lock_wake_waiters();
            return 0;
        }
    }
    spin_unlock_irqrestore(&g_file_lock_table_lock, flags);
    return -ENOLCK;
}
