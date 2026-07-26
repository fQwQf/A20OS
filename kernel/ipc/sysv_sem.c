#include "ipc/sysv_sem.h"

#include "core/consts.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "sys/usercopy.h"

#define IPC_CREAT       01000
#define IPC_EXCL        02000
#define IPC_NOWAIT      04000
#define IPC_64_BIT      0x100
#define IPC_RMID        0
#define IPC_SET         1
#define IPC_STAT        2
#define IPC_INFO        3

#define GETPID          11
#define GETVAL          12
#define GETALL          13
#define GETNCNT         14
#define GETZCNT         15
#define SETVAL          16
#define SETALL          17
#define SEM_STAT        18
#define SEM_INFO        19
#define SEM_STAT_ANY    20

#define SYSV_SEM_MAX        32
#define SYSV_SEM_PER_SET    64

typedef struct {
    unsigned short sem_num;
    short sem_op;
    short sem_flg;
} sysv_sembuf_t;

/* 64-bit Linux UAPI layout shared by riscv64 and loongarch64. */
typedef struct {
    int key;
    unsigned int uid;
    unsigned int gid;
    unsigned int cuid;
    unsigned int cgid;
    unsigned int mode;
    int seq;
    unsigned long unused1;
    unsigned long unused2;
} sysv_ipc_perm64_t;

typedef struct {
    sysv_ipc_perm64_t sem_perm;
    long sem_otime;
    long sem_ctime;
    unsigned long sem_nsems;
    unsigned long unused3;
    unsigned long unused4;
} sysv_semid64_ds_t;

typedef struct {
    int semmap;
    int semmni;
    int semmns;
    int semmnu;
    int semmsl;
    int semopm;
    int semume;
    int semusz;
    int semvmx;
    int semaem;
} sysv_seminfo_t;

typedef struct {
    int used;
    int key;
    int nsems;
    unsigned int uid;
    unsigned int gid;
    unsigned int cuid;
    unsigned int cgid;
    unsigned int mode;
    unsigned short val[SYSV_SEM_PER_SET];
    unsigned short ncnt[SYSV_SEM_PER_SET];
    unsigned short zcnt[SYSV_SEM_PER_SET];
    int last_pid;
    wait_queue_t waiters;
} sysv_sem_set_t;

static sysv_sem_set_t g_sem[SYSV_SEM_MAX];
static spinlock_t g_sem_lock = SPINLOCK_INIT;

static int sem_valid_locked(int semid)
{
    return semid >= 0 && semid < SYSV_SEM_MAX && g_sem[semid].used;
}

int sysv_sem_get(int key, int nsems, int semflg)
{
    if (nsems < 0 || nsems > SYSV_SEM_PER_SET)
        return -EINVAL;

    task_t *cur = proc_current();
    unsigned int uid = cur ? (unsigned int)cur->cred.euid : 0;
    unsigned int gid = cur ? (unsigned int)cur->cred.egid : 0;
    uint64_t flags = spin_lock_irqsave(&g_sem_lock);
    for (int i = 0; i < SYSV_SEM_MAX; i++) {
        if (g_sem[i].used && g_sem[i].key == key && key != 0) {
            if ((semflg & IPC_CREAT) && (semflg & IPC_EXCL)) {
                spin_unlock_irqrestore(&g_sem_lock, flags);
                return -EEXIST;
            }
            if (nsems > 0 && nsems > g_sem[i].nsems) {
                spin_unlock_irqrestore(&g_sem_lock, flags);
                return -EINVAL;
            }
            spin_unlock_irqrestore(&g_sem_lock, flags);
            return i;
        }
    }

    if (!(semflg & IPC_CREAT)) {
        spin_unlock_irqrestore(&g_sem_lock, flags);
        return -ENOENT;
    }
    if (nsems <= 0) {
        spin_unlock_irqrestore(&g_sem_lock, flags);
        return -EINVAL;
    }

    for (int i = 0; i < SYSV_SEM_MAX; i++) {
        if (!g_sem[i].used) {
            memset(&g_sem[i], 0, sizeof(g_sem[i]));
            g_sem[i].used = 1;
            g_sem[i].key = key;
            g_sem[i].nsems = nsems;
            g_sem[i].uid = uid;
            g_sem[i].gid = gid;
            g_sem[i].cuid = uid;
            g_sem[i].cgid = gid;
            g_sem[i].mode = (unsigned int)semflg & 0777;
            wait_queue_init(&g_sem[i].waiters);
            spin_unlock_irqrestore(&g_sem_lock, flags);
            return i;
        }
    }

    spin_unlock_irqrestore(&g_sem_lock, flags);
    return -ENOSPC;
}

static int sem_copy_all_to_user(sysv_sem_set_t *set, void *arg)
{
    if (!arg)
        return -EINVAL;
    return copy_to_user(arg, set->val, (size_t)set->nsems * sizeof(unsigned short)) < 0 ?
           -EFAULT : 0;
}

static int sem_copy_all_from_user(sysv_sem_set_t *set, void *arg)
{
    if (!arg)
        return -EINVAL;
    return copy_from_user(set->val, arg, (size_t)set->nsems * sizeof(unsigned short)) < 0 ?
           -EFAULT : 0;
}

int sysv_sem_control(int semid, int semnum, int cmd, void *arg)
{
    cmd &= ~IPC_64_BIT;

    uint64_t flags = spin_lock_irqsave(&g_sem_lock);
    if (cmd == IPC_INFO || cmd == SEM_INFO) {
        sysv_seminfo_t info;
        int highest = -1;
        int used = 0;
        for (int i = 0; i < SYSV_SEM_MAX; i++) {
            if (g_sem[i].used) {
                highest = i;
                used++;
            }
        }
        memset(&info, 0, sizeof(info));
        info.semmni = SYSV_SEM_MAX;
        info.semmns = SYSV_SEM_MAX * SYSV_SEM_PER_SET;
        info.semmsl = SYSV_SEM_PER_SET;
        info.semopm = SYSV_SEM_PER_SET;
        info.semusz = cmd == SEM_INFO ? used : 1;
        info.semvmx = 32767;
        spin_unlock_irqrestore(&g_sem_lock, flags);

        if (!arg || copy_to_user(arg, &info, sizeof(info)) < 0)
            return -EFAULT;
        return highest < 0 ? 0 : highest;
    }

    if (!sem_valid_locked(semid)) {
        spin_unlock_irqrestore(&g_sem_lock, flags);
        return -EINVAL;
    }
    sysv_sem_set_t *set = &g_sem[semid];

    if (semnum < 0 || (semnum >= set->nsems &&
        cmd != IPC_RMID && cmd != IPC_STAT && cmd != SEM_STAT &&
        cmd != SEM_STAT_ANY && cmd != SEM_INFO)) {
        spin_unlock_irqrestore(&g_sem_lock, flags);
        return -EINVAL;
    }

    switch (cmd) {
    case IPC_RMID:
        set->used = 0;
        spin_unlock_irqrestore(&g_sem_lock, flags);
        wait_queue_wake_all(&set->waiters, 0, PROC_WAKE_EVENT);
        return 0;
    case GETVAL: {
        int v = set->val[semnum];
        spin_unlock_irqrestore(&g_sem_lock, flags);
        return v;
    }
    case GETPID: {
        int pid = set->last_pid;
        spin_unlock_irqrestore(&g_sem_lock, flags);
        return pid;
    }
    case GETNCNT: {
        int count = set->ncnt[semnum];
        spin_unlock_irqrestore(&g_sem_lock, flags);
        return count;
    }
    case GETZCNT: {
        int count = set->zcnt[semnum];
        spin_unlock_irqrestore(&g_sem_lock, flags);
        return count;
    }
    case SETVAL: {
        uintptr_t raw = (uintptr_t)arg;
        set->val[semnum] = (unsigned short)(raw & 0xffff);
        set->last_pid = proc_current() ? proc_current()->pid : 0;
        spin_unlock_irqrestore(&g_sem_lock, flags);
        wait_queue_wake_all(&set->waiters, 0, PROC_WAKE_EVENT);
        return 0;
    }
    case GETALL: {
        int r = sem_copy_all_to_user(set, arg);
        spin_unlock_irqrestore(&g_sem_lock, flags);
        return r;
    }
    case SETALL: {
        int r = sem_copy_all_from_user(set, arg);
        if (r == 0)
            set->last_pid = proc_current() ? proc_current()->pid : 0;
        spin_unlock_irqrestore(&g_sem_lock, flags);
        if (r == 0)
            wait_queue_wake_all(&set->waiters, 0, PROC_WAKE_EVENT);
        return r;
    }
    case IPC_SET: {
        sysv_semid64_ds_t ds;
        spin_unlock_irqrestore(&g_sem_lock, flags);
        if (!arg || copy_from_user(&ds, arg, sizeof(ds)) < 0)
            return -EFAULT;

        flags = spin_lock_irqsave(&g_sem_lock);
        if (!sem_valid_locked(semid)) {
            spin_unlock_irqrestore(&g_sem_lock, flags);
            return -EINVAL;
        }
        set = &g_sem[semid];
        set->uid = ds.sem_perm.uid;
        set->gid = ds.sem_perm.gid;
        set->mode = (set->mode & ~0777U) | (ds.sem_perm.mode & 0777U);
        spin_unlock_irqrestore(&g_sem_lock, flags);
        return 0;
    }
    case IPC_STAT:
    case SEM_STAT:
    case SEM_STAT_ANY: {
        sysv_semid64_ds_t ds;
        memset(&ds, 0, sizeof(ds));
        ds.sem_perm.key = set->key;
        ds.sem_perm.uid = set->uid;
        ds.sem_perm.gid = set->gid;
        ds.sem_perm.cuid = set->cuid;
        ds.sem_perm.cgid = set->cgid;
        ds.sem_perm.mode = set->mode;
        ds.sem_perm.seq = semid;
        ds.sem_nsems = (unsigned long)set->nsems;
        spin_unlock_irqrestore(&g_sem_lock, flags);

        if (!arg || copy_to_user(arg, &ds, sizeof(ds)) < 0)
            return -EFAULT;
        return cmd == IPC_STAT ? 0 : semid;
    }
    default:
        spin_unlock_irqrestore(&g_sem_lock, flags);
        return -EINVAL;
    }
}

static int sem_ops_can_apply(sysv_sem_set_t *set, const sysv_sembuf_t *ops,
                             size_t nsops, unsigned *wait_sem, int *wait_zero)
{
    for (size_t i = 0; i < nsops; i++) {
        unsigned n = ops[i].sem_num;
        short op = ops[i].sem_op;
        if (n >= (unsigned)set->nsems)
            return -EFBIG;
        if (op < 0 && set->val[n] < (unsigned short)(-op)) {
            *wait_sem = n;
            *wait_zero = 0;
            return 0;
        }
        if (op == 0 && set->val[n] != 0) {
            *wait_sem = n;
            *wait_zero = 1;
            return 0;
        }
    }
    return 1;
}

static void sem_ops_apply(sysv_sem_set_t *set, const sysv_sembuf_t *ops,
                          size_t nsops)
{
    for (size_t i = 0; i < nsops; i++) {
        unsigned n = ops[i].sem_num;
        short op = ops[i].sem_op;
        if (op < 0)
            set->val[n] = (unsigned short)(set->val[n] - (unsigned short)(-op));
        else
            set->val[n] = (unsigned short)(set->val[n] + (unsigned short)op);
    }
    set->last_pid = proc_current() ? proc_current()->pid : 0;
}

int sysv_sem_timedop(int semid, const void *sops, size_t nsops, uint64_t deadline)
{
    if (!sops || nsops == 0 || nsops > 64)
        return -EINVAL;

    sysv_sembuf_t ops[64];
    if (copy_from_user(ops, sops, nsops * sizeof(sysv_sembuf_t)) < 0)
        return -EFAULT;

    proc_wait_token_t token = {0};
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&g_sem_lock);
        if (!sem_valid_locked(semid)) {
            spin_unlock_irqrestore(&g_sem_lock, flags);
            if (token.task) {
                (void)proc_park_cancel(token);
                proc_park_finish(token);
            }
            return -EINVAL;
        }
        sysv_sem_set_t *set = &g_sem[semid];
        unsigned wait_sem = 0;
        int wait_zero = 0;
        int can = sem_ops_can_apply(set, ops, nsops, &wait_sem, &wait_zero);
        if (can < 0) {
            spin_unlock_irqrestore(&g_sem_lock, flags);
            if (token.task) {
                (void)proc_park_cancel(token);
                proc_park_finish(token);
            }
            return can;
        }
        if (can > 0) {
            sem_ops_apply(set, ops, nsops);
            spin_unlock_irqrestore(&g_sem_lock, flags);
            if (token.task) {
                (void)proc_park_cancel(token);
                proc_park_finish(token);
            }
            wait_queue_wake_all(&set->waiters, 0, PROC_WAKE_EVENT);
            return 0;
        }

        int nowait = 0;
        for (size_t i = 0; i < nsops; i++) {
            if (ops[i].sem_flg & IPC_NOWAIT) {
                nowait = 1;
                break;
            }
        }
        if (nowait) {
            spin_unlock_irqrestore(&g_sem_lock, flags);
            if (token.task) {
                (void)proc_park_cancel(token);
                proc_park_finish(token);
            }
            return -EAGAIN;
        }
        if (signal_task_has_unblocked(proc_current())) {
            spin_unlock_irqrestore(&g_sem_lock, flags);
            if (token.task) {
                (void)proc_park_cancel(token);
                proc_park_finish(token);
            }
            return -EINTR;
        }
        if (deadline && (int64_t)(timer_get_ticks() - deadline) >= 0) {
            spin_unlock_irqrestore(&g_sem_lock, flags);
            if (token.task) {
                (void)proc_park_cancel(token);
                proc_park_finish(token);
            }
            return -EAGAIN;
        }

        if (!token.task) {
            spin_unlock_irqrestore(&g_sem_lock, flags);
            token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
            if (!token.task)
                return -EAGAIN;
            continue;
        }

        wait_queue_entry_t entry = {0};
        bool linked = wait_queue_link(&set->waiters, &entry, token, 0);
        if (wait_zero)
            set->zcnt[wait_sem]++;
        else
            set->ncnt[wait_sem]++;
        spin_unlock_irqrestore(&g_sem_lock, flags);

        if (linked)
            (void)proc_park_commit(token);
        else
            (void)proc_park_cancel(token);
        wait_queue_unlink(&set->waiters, &entry);
        proc_park_finish(token);
        token = (proc_wait_token_t){0};

        flags = spin_lock_irqsave(&g_sem_lock);
        if (wait_zero) {
            if (set->zcnt[wait_sem] > 0)
                set->zcnt[wait_sem]--;
        } else if (set->ncnt[wait_sem] > 0) {
            set->ncnt[wait_sem]--;
        }
        spin_unlock_irqrestore(&g_sem_lock, flags);
    }
}

int sysv_sem_op(int semid, const void *sops, size_t nsops)
{
    return sysv_sem_timedop(semid, sops, nsops, 0);
}
