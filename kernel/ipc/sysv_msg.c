#include "ipc/sysv_msg.h"

#include "core/consts.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

/*
 * SysV message queues.
 *
 * Linux msgid64_ds wire layout (riscv64/loongarch64 shared):
 *   struct msqid64_ds {
 *     struct ipc_perm msq_perm;   // 48 bytes (64-bit perm layout)
 *     unsigned long msq_stime;    // 8
 *     unsigned long msq_rtime;    // 8
 *     unsigned long msq_ctime;    // 8
 *     unsigned long msg_cbytes;   // 8
 *     unsigned long msg_qnum;     // 8
 *     unsigned long msg_qbytes;   // 8
 *     unsigned long msg_lspid;    // 4 (+4 pad)
 *     unsigned long msg_lrpid;    // 4
 *   }                                  => 96 bytes total
 * The 64-bit ipc_perm layout is: key(4) uid(4) gid(4) cuid(4) cgid(4) mode(4)
 * seq(4) __pad1(4) __unused1(8) __unused2(8) = 48 bytes.
 */

#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_NOWAIT  04000
#define IPC_64_BIT  0x100
#define IPC_RMID    0
#define IPC_SET     1
#define IPC_STAT    2
#define IPC_INFO    3

#define MSG_STAT    11
#define MSG_INFO    12
#define MSG_STAT_ANY 13

#define MSG_NOERROR 010000
#define MSG_MAX     8192
#define MSG_QBYTES  16384

#define SYSV_MSG_MAX 32

typedef struct {
    int key;
    unsigned int uid;
    unsigned int gid;
    unsigned int cuid;
    unsigned int cgid;
    unsigned int mode;
    int seq;
    unsigned int pad;
    unsigned long unused1;
    unsigned long unused2;
} sysv_ipc_perm64_t;

/* Kernel-side message node. */
typedef struct sysv_msg_node {
    struct sysv_msg_node *next;
    int64_t type;
    size_t  size;
    char    data[];
} sysv_msg_node_t;

typedef struct {
    int used;
    int key;
    sysv_ipc_perm64_t perm;
    uint64_t stime;
    uint64_t rtime;
    uint64_t ctime;
    size_t cbytes;
    size_t qnum;
    size_t qbytes;
    int lspid;
    int lrpid;
    sysv_msg_node_t *head;
    sysv_msg_node_t *tail;
    wait_queue_t recv_wq;
    wait_queue_t send_wq;
} sysv_msg_queue_t;

static sysv_msg_queue_t g_msg[SYSV_MSG_MAX];
static spinlock_t g_msg_lock = SPINLOCK_INIT;

static int msg_valid_locked(int msqid)
{
    return msqid >= 0 && msqid < SYSV_MSG_MAX && g_msg[msqid].used;
}

static int msg_calc_qbytes(void)
{
    return MSG_QBYTES;
}

static void msg_wake_blocked(sysv_msg_queue_t *q)
{
    wait_queue_wake_all(&q->recv_wq, 0, PROC_WAKE_EVENT);
    wait_queue_wake_all(&q->send_wq, 0, PROC_WAKE_EVENT);
}

int sysv_msg_get(int key, int msgflg)
{
    task_t *cur = proc_current();
    unsigned int uid = cur ? (unsigned int)cur->cred.euid : 0;
    unsigned int gid = cur ? (unsigned int)cur->cred.egid : 0;
    uint64_t flags = spin_lock_irqsave(&g_msg_lock);

    for (int i = 0; i < SYSV_MSG_MAX; i++) {
        if (g_msg[i].used && g_msg[i].key == key && key != 0) {
            if ((msgflg & IPC_CREAT) && (msgflg & IPC_EXCL)) {
                spin_unlock_irqrestore(&g_msg_lock, flags);
                return -EEXIST;
            }
            spin_unlock_irqrestore(&g_msg_lock, flags);
            return i;
        }
    }
    if (!(msgflg & IPC_CREAT)) {
        spin_unlock_irqrestore(&g_msg_lock, flags);
        return -ENOENT;
    }

    int slot = -1;
    for (int i = 0; i < SYSV_MSG_MAX; i++) {
        if (!g_msg[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&g_msg_lock, flags);
        return -ENOSPC;
    }

    sysv_msg_queue_t *q = &g_msg[slot];
    memset(q, 0, sizeof(*q));
    q->used = 1;
    q->key = key;
    q->perm.key = key;
    q->perm.uid = uid;
    q->perm.gid = gid;
    q->perm.cuid = uid;
    q->perm.cgid = gid;
    q->perm.mode = (unsigned int)(msgflg & 0777);
    q->ctime = timer_get_ticks();
    q->qbytes = (size_t)msg_calc_qbytes();
    wait_queue_init(&q->recv_wq);
    wait_queue_init(&q->send_wq);
    spin_unlock_irqrestore(&g_msg_lock, flags);
    return slot;
}

int sysv_msg_send(int msqid, const void *msgp, size_t msgsz, int msgflg)
{
    if (!msgp)
        return -EINVAL;
    if (msgsz > MSG_MAX)
        return -EINVAL;

    /* Read {long mtype; char mtext[msgsz]} from user. */
    int64_t mtype = 0;
    if (copy_from_user(&mtype, msgp, sizeof(mtype)) < 0)
        return -EFAULT;
    if (mtype <= 0)
        return -EINVAL;

    sysv_msg_node_t *node = kmalloc(sizeof(*node) + msgsz);
    if (!node)
        return -ENOMEM;
    if (msgsz > 0 &&
        copy_from_user(node->data, (const char *)msgp + sizeof(mtype), msgsz) < 0) {
        kfree(node);
        return -EFAULT;
    }
    node->type = mtype;
    node->size = msgsz;
    node->next = NULL;

    task_t *cur = proc_current();
    uint64_t flags = spin_lock_irqsave(&g_msg_lock);
    sysv_msg_queue_t *q = msg_valid_locked(msqid) ? &g_msg[msqid] : NULL;
    if (!q) {
        spin_unlock_irqrestore(&g_msg_lock, flags);
        kfree(node);
        return -EINVAL;
    }
    if (q->cbytes + msgsz > q->qbytes) {
        if (msgflg & IPC_NOWAIT) {
            spin_unlock_irqrestore(&g_msg_lock, flags);
            kfree(node);
            return -EAGAIN;
        }
        /* Block until space is available. */
        for (;;) {
            spin_unlock_irqrestore(&g_msg_lock, flags);
            proc_wait_token_t token =
                proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
            if (!token.task) {
                kfree(node);
                return -EAGAIN;
            }
            wait_queue_entry_t entry = {0};
            flags = spin_lock_irqsave(&g_msg_lock);
            if (g_msg[msqid].used &&
                g_msg[msqid].cbytes + msgsz <= g_msg[msqid].qbytes) {
                spin_unlock_irqrestore(&g_msg_lock, flags);
                (void)proc_park_cancel(token);
                proc_park_finish(token);
                flags = spin_lock_irqsave(&g_msg_lock);
                break;
            }
            bool linked = wait_queue_link(&g_msg[msqid].send_wq, &entry,
                                          token, 0);
            spin_unlock_irqrestore(&g_msg_lock, flags);
            proc_wake_reason_t reason;
            if (linked)
                reason = proc_park_commit(token);
            else {
                (void)proc_park_cancel(token);
                reason = PROC_WAKE_CANCEL;
            }
            wait_queue_unlink(&g_msg[msqid].send_wq, &entry);
            proc_park_finish(token);
            if (proc_wake_reason_is_task_interrupt(reason)) {
                kfree(node);
                return -EINTR;
            }
            flags = spin_lock_irqsave(&g_msg_lock);
            if (!g_msg[msqid].used) {
                spin_unlock_irqrestore(&g_msg_lock, flags);
                kfree(node);
                return -EIDRM;
            }
        }
        q = &g_msg[msqid];
    }

    if (!q->tail)
        q->head = q->tail = node;
    else {
        q->tail->next = node;
        q->tail = node;
    }
    q->cbytes += msgsz;
    q->qnum++;
    q->stime = timer_get_ticks();
    q->lspid = cur ? cur->pid : 0;
    wait_queue_wake_all(&q->recv_wq, 0, PROC_WAKE_EVENT);
    spin_unlock_irqrestore(&g_msg_lock, flags);
    return 0;
}

long sysv_msg_recv(int msqid, void *msgp, size_t msgsz, int64_t msgtyp,
                   int msgflg)
{
    if (!msgp)
        return -EINVAL;

    task_t *cur = proc_current();
    uint64_t flags = spin_lock_irqsave(&g_msg_lock);
    sysv_msg_queue_t *q = msg_valid_locked(msqid) ? &g_msg[msqid] : NULL;
    if (!q) {
        spin_unlock_irqrestore(&g_msg_lock, flags);
        return -EINVAL;
    }

    /* Select the message by type: 0 = first, >0 = first with that type,
     * <0 = first with type <= |msgtyp|. */
    for (;;) {
        sysv_msg_node_t *prev = NULL;
        sysv_msg_node_t *node = q->head;
        while (node) {
            int match = 0;
            if (msgtyp == 0)
                match = 1;
            else if (msgtyp > 0)
                match = node->type == msgtyp;
            else
                match = node->type <= -msgtyp;
            if (match)
                break;
            prev = node;
            node = node->next;
        }
        if (node) {
            size_t n = node->size;
            if (n > msgsz) {
                if (!(msgflg & MSG_NOERROR)) {
                    spin_unlock_irqrestore(&g_msg_lock, flags);
                    return -E2BIG;
                }
                n = msgsz;
            }
            int64_t type = node->type;
            /* Unlink. */
            if (prev)
                prev->next = node->next;
            else
                q->head = node->next;
            if (!node->next)
                q->tail = prev;
            q->cbytes -= node->size;
            q->qnum--;
            q->rtime = timer_get_ticks();
            q->lrpid = cur ? cur->pid : 0;
            wait_queue_wake_all(&q->send_wq, 0, PROC_WAKE_EVENT);
            spin_unlock_irqrestore(&g_msg_lock, flags);

            if (copy_to_user(msgp, &type, sizeof(type)) < 0) {
                kfree(node);
                return -EFAULT;
            }
            if (n > 0 &&
                copy_to_user((char *)msgp + sizeof(type), node->data, n) < 0) {
                kfree(node);
                return -EFAULT;
            }
            kfree(node);
            return (long)n;
        }
        if (msgflg & IPC_NOWAIT) {
            spin_unlock_irqrestore(&g_msg_lock, flags);
            return -ENOMSG;
        }
        /* Block until a message arrives. */
        spin_unlock_irqrestore(&g_msg_lock, flags);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
        if (!token.task)
            return -EAGAIN;
        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&g_msg_lock);
        if (!g_msg[msqid].used) {
            spin_unlock_irqrestore(&g_msg_lock, flags);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return -EIDRM;
        }
        bool linked = wait_queue_link(&g_msg[msqid].recv_wq, &entry, token, 0);
        spin_unlock_irqrestore(&g_msg_lock, flags);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&g_msg[msqid].recv_wq, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason))
            return -EINTR;
        flags = spin_lock_irqsave(&g_msg_lock);
    }
}

int sysv_msg_control(int msqid, int cmd, void *arg)
{
    uint64_t flags = spin_lock_irqsave(&g_msg_lock);
    sysv_msg_queue_t *q = msg_valid_locked(msqid) ? &g_msg[msqid] : NULL;
    if (!q) {
        spin_unlock_irqrestore(&g_msg_lock, flags);
        return -EINVAL;
    }

    int r = 0;
    switch (cmd) {
    case IPC_RMID: {
        /* Drain all messages and mark the queue free. */
        sysv_msg_node_t *n = q->head;
        while (n) {
            sysv_msg_node_t *nx = n->next;
            kfree(n);
            n = nx;
        }
        msg_wake_blocked(q);
        memset(q, 0, sizeof(*q));
        break;
    }
    case IPC_STAT:
    case MSG_STAT:
    case MSG_STAT_ANY: {
        /* 96-byte msqid64_ds layout. */
        char ds[96];
        memset(ds, 0, sizeof(ds));
        sysv_ipc_perm64_t *perm = (sysv_ipc_perm64_t *)ds;
        *perm = q->perm;
        ((unsigned long *)(ds + 48))[0] = q->stime;
        ((unsigned long *)(ds + 56))[0] = q->rtime;
        ((unsigned long *)(ds + 64))[0] = q->ctime;
        ((unsigned long *)(ds + 72))[0] = q->cbytes;
        ((unsigned long *)(ds + 80))[0] = q->qnum;
        ((unsigned long *)(ds + 88))[0] = q->qbytes;
        if (cmd != IPC_STAT) {
            /* MSG_STAT returns the id; the caller passes the seq in arg. */
            r = msqid;
        }
        spin_unlock_irqrestore(&g_msg_lock, flags);
        if (arg && copy_to_user(arg, ds, sizeof(ds)) < 0)
            return -EFAULT;
        return r;
    }
    case IPC_SET: {
        if (!arg) {
            spin_unlock_irqrestore(&g_msg_lock, flags);
            return -EINVAL;
        }
        char ds[96];
        spin_unlock_irqrestore(&g_msg_lock, flags);
        if (copy_from_user(ds, arg, sizeof(ds)) < 0)
            return -EFAULT;
        flags = spin_lock_irqsave(&g_msg_lock);
        q = msg_valid_locked(msqid) ? &g_msg[msqid] : NULL;
        if (!q) {
            spin_unlock_irqrestore(&g_msg_lock, flags);
            return -EINVAL;
        }
        sysv_ipc_perm64_t *perm = (sysv_ipc_perm64_t *)ds;
        q->perm.uid = perm->uid;
        q->perm.gid = perm->gid;
        q->perm.mode = perm->mode;
        q->ctime = timer_get_ticks();
        break;
    }
    default:
        r = -EINVAL;
        break;
    }
    spin_unlock_irqrestore(&g_msg_lock, flags);
    return r;
}
