#include "ipc/posix_mq.h"

#include "core/consts.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "sys/usercopy.h"

/*
 * POSIX message queues.
 *
 * Linux exposes mq_* as fds: mq_open() returns an fd backed by a mqueuefs
 * inode.  A20OS keeps the descriptor as a small integer in a per-task table
 * (the "mqd"), which the ABI layer maps to/from the fd number so userland
 * sees normal fd semantics.  The core here owns queue objects, attributes,
 * message storage and the park/wake blocking protocol.
 */

#define MQ_MAX_QUEUES 64
#define MQ_DEFAULT_MAXMSG 10
#define MQ_DEFAULT_MSGSIZE 8192
#define MQ_MAX_MSGSIZE     8192
#define MQ_PRIO_MAX        32

#define MQ_O_ACCMODE 3
#define MQ_O_NONBLOCK 0x800
#define MQ_O_CLOEXEC 0x80000

typedef struct mq_message {
    struct mq_message *next;
    unsigned prio;
    size_t size;
    char data[];
} mq_message_t;

typedef struct mq_queue {
    int used;
    char name[MQ_NAME_MAX];
    int64_t mq_flags;
    int64_t mq_maxmsg;
    int64_t mq_msgsize;
    int64_t mq_curmsgs;
    mq_message_t *head;
    mq_message_t *tail;
    int unlinked;
    int refcount;          /* open descriptor count */
    wait_queue_t send_wq;
    wait_queue_t recv_wq;
    spinlock_t lock;
    /* mq_notify */
    uint64_t notify_sig;
    int notify_pid;
    int notify_pending;
} mq_queue_t;

static mq_queue_t g_mq[MQ_MAX_QUEUES];
static spinlock_t g_mq_lock = SPINLOCK_INIT;

/* Per-task descriptor table (mqd -> queue pointer). */
#define MQ_MAX_DESCS 256
static mq_queue_t *g_mq_desc[MQ_MAX_DESCS];
static spinlock_t g_mq_desc_lock = SPINLOCK_INIT;

static mq_queue_t *mq_lookup_locked(const char *name)
{
    for (int i = 0; i < MQ_MAX_QUEUES; i++)
        if (g_mq[i].used && strcmp(g_mq[i].name, name) == 0)
            return &g_mq[i];
    return NULL;
}

static mq_queue_t *mq_alloc_locked(const char *name, const mq_attr_kern_t *attr)
{
    int slot = -1;
    for (int i = 0; i < MQ_MAX_QUEUES; i++) {
        if (!g_mq[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return NULL;
    mq_queue_t *q = &g_mq[slot];
    memset(q, 0, sizeof(*q));
    q->used = 1;
    strncpy(q->name, name, MQ_NAME_MAX - 1);
    q->name[MQ_NAME_MAX - 1] = '\0';
    q->mq_maxmsg = attr ? attr->mq_maxmsg : MQ_DEFAULT_MAXMSG;
    q->mq_msgsize = attr ? attr->mq_msgsize : MQ_DEFAULT_MSGSIZE;
    if (q->mq_maxmsg <= 0)
        q->mq_maxmsg = MQ_DEFAULT_MAXMSG;
    if (q->mq_msgsize <= 0 || q->mq_msgsize > MQ_MAX_MSGSIZE)
        q->mq_msgsize = MQ_DEFAULT_MSGSIZE;
    q->mq_flags = attr ? (attr->mq_flags & MQ_O_NONBLOCK) : 0;
    wait_queue_init(&q->send_wq);
    wait_queue_init(&q->recv_wq);
    spin_init(&q->lock);
    return q;
}

static int mq_alloc_descriptor(mq_queue_t *q)
{
    uint64_t flags = spin_lock_irqsave(&g_mq_desc_lock);
    for (int i = 0; i < MQ_MAX_DESCS; i++) {
        if (!g_mq_desc[i]) {
            g_mq_desc[i] = q;
            spin_unlock_irqrestore(&g_mq_desc_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&g_mq_desc_lock, flags);
    return -EMFILE;
}

static mq_queue_t *mq_desc_get(int mqd)
{
    if (mqd < 0 || mqd >= MQ_MAX_DESCS)
        return NULL;
    return g_mq_desc[mqd];
}

int posix_mq_open(const char *name, int oflag, int mode,
                  const mq_attr_kern_t *attr)
{
    (void)mode;
    if (!name || !name[0])
        return -EINVAL;
    /* musl strips the leading '/' from the name before the syscall; Linux
     * mqueuefs accepts both forms.  Normalize to the bare name. */
    if (name[0] == '/')
        name++;

    uint64_t flags = spin_lock_irqsave(&g_mq_lock);
    mq_queue_t *q = mq_lookup_locked(name);
    if (q) {
        if ((oflag & 0x40 /* O_CREAT */) && (oflag & 0x80 /* O_EXCL */)) {
            spin_unlock_irqrestore(&g_mq_lock, flags);
            return -EEXIST;
        }
        if (oflag & 0x40) {
            /* O_CREAT on existing: return existing (attribute ignored). */
        }
    } else if (oflag & 0x40) {
        q = mq_alloc_locked(name, attr);
        if (!q) {
            spin_unlock_irqrestore(&g_mq_lock, flags);
            return -ENOSPC;
        }
    } else {
        spin_unlock_irqrestore(&g_mq_lock, flags);
        return -ENOENT;
    }

    /* Per-process open tracking: keep a refcount and per-task descriptor. */
    q->refcount++;
    int mqd = mq_alloc_descriptor(q);
    if (mqd < 0) {
        q->refcount--;
        if (q->refcount <= 0 && q->unlinked) {
            /* Last descriptor gone on an unlinked queue: free it. */
            memset(q, 0, sizeof(*q));
        }
        spin_unlock_irqrestore(&g_mq_lock, flags);
        return mqd;
    }
    spin_unlock_irqrestore(&g_mq_lock, flags);
    return mqd;
}

int posix_mq_unlink(const char *name)
{
    if (!name)
        return -EINVAL;
    if (name[0] == '/')
        name++;
    uint64_t flags = spin_lock_irqsave(&g_mq_lock);
    mq_queue_t *q = mq_lookup_locked(name);
    if (!q) {
        spin_unlock_irqrestore(&g_mq_lock, flags);
        return -ENOENT;
    }
    q->unlinked = 1;
    memset(q->name, 0, sizeof(q->name));
    /* Wake all waiters; they re-check and fail with EBADF. */
    wait_queue_wake_all(&q->send_wq, 0, PROC_WAKE_EVENT);
    wait_queue_wake_all(&q->recv_wq, 0, PROC_WAKE_EVENT);
    if (q->refcount <= 0)
        memset(q, 0, sizeof(*q));
    spin_unlock_irqrestore(&g_mq_lock, flags);
    return 0;
}

int posix_mq_getsetattr(int mqd, const mq_attr_kern_t *newattr,
                        mq_attr_kern_t *oldattr)
{
    mq_queue_t *q = mq_desc_get(mqd);
    if (!q)
        return -EBADF;
    uint64_t flags = spin_lock_irqsave(&g_mq_lock);
    if (!q->used) {
        spin_unlock_irqrestore(&g_mq_lock, flags);
        return -EBADF;
    }
    mq_attr_kern_t attr;
    attr.mq_flags = q->mq_flags;
    attr.mq_maxmsg = q->mq_maxmsg;
    attr.mq_msgsize = q->mq_msgsize;
    attr.mq_curmsgs = q->mq_curmsgs;
    if (newattr) {
        /* Only mq_flags is modifiable; the rest are fixed by the kernel. */
        q->mq_flags = newattr->mq_flags & MQ_O_NONBLOCK;
        attr.mq_flags = q->mq_flags;
    }
    spin_unlock_irqrestore(&g_mq_lock, flags);
    if (oldattr)
        *oldattr = attr;
    return 0;
}

int posix_mq_notify(int mqd, const void *sigevent)
{
    mq_queue_t *q = mq_desc_get(mqd);
    if (!q)
        return -EBADF;
    uint64_t flags = spin_lock_irqsave(&g_mq_lock);
    if (!q->used) {
        spin_unlock_irqrestore(&g_mq_lock, flags);
        return -EBADF;
    }
    if (!sigevent) {
        q->notify_pid = 0;
        q->notify_sig = 0;
        spin_unlock_irqrestore(&g_mq_lock, flags);
        return 0;
    }
    /* sigevent: {sigev_value(8), sigev_signo(4), sigev_notify(4),
     * sigev_notify_attributes(8)} = 24 bytes on 64-bit.  We only record the
     * signo and pid; the signal is delivered on the next message arrival. */
    unsigned char ev[24];
    if (copy_from_user(ev, sigevent, sizeof(ev)) < 0) {
        spin_unlock_irqrestore(&g_mq_lock, flags);
        return -EFAULT;
    }
    int64_t signo;
    memcpy(&signo, ev + 8, 4);
    if (signo <= 0 || signo >= 64) {
        spin_unlock_irqrestore(&g_mq_lock, flags);
        return -EINVAL;
    }
    q->notify_sig = (uint64_t)(int)signo;
    q->notify_pid = proc_current() ? proc_current()->pid : 0;
    q->notify_pending = 0;
    spin_unlock_irqrestore(&g_mq_lock, flags);
    return 0;
}

int posix_mq_timedsend(int mqd, const char *msg, size_t msg_len,
                       unsigned prio, uint64_t deadline)
{
    if (prio >= MQ_PRIO_MAX)
        return -EINVAL;
    mq_queue_t *q = mq_desc_get(mqd);
    if (!q)
        return -EBADF;

    mq_message_t *node = kmalloc(sizeof(*node) + msg_len);
    if (!node)
        return -ENOMEM;
    node->prio = prio;
    node->size = msg_len;
    if (msg_len > 0)
        memcpy(node->data, msg, msg_len);

    uint64_t flags = spin_lock_irqsave(&g_mq_lock);
    for (;;) {
        if (!q->used) {
            spin_unlock_irqrestore(&g_mq_lock, flags);
            kfree(node);
            return -EBADF;
        }
        if (msg_len > (size_t)q->mq_msgsize) {
            spin_unlock_irqrestore(&g_mq_lock, flags);
            kfree(node);
            return -EMSGSIZE;
        }
        if (q->mq_curmsgs < q->mq_maxmsg)
            break;
        if (q->mq_flags & MQ_O_NONBLOCK) {
            spin_unlock_irqrestore(&g_mq_lock, flags);
            kfree(node);
            return -EAGAIN;
        }
        if (deadline != 0 && timer_get_ticks() >= deadline) {
            spin_unlock_irqrestore(&g_mq_lock, flags);
            kfree(node);
            return -ETIMEDOUT;
        }
        /* Park until a slot frees. */
        uint64_t wake = 0;
        if (deadline != 0)
            wake = deadline;
        spin_unlock_irqrestore(&g_mq_lock, flags);
        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE,
                                                    wake);
        if (!token.task) {
            kfree(node);
            return -EAGAIN;
        }
        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&g_mq_lock);
        if (!q->used || q->mq_curmsgs < q->mq_maxmsg) {
            spin_unlock_irqrestore(&g_mq_lock, flags);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            flags = spin_lock_irqsave(&g_mq_lock);
            continue;
        }
        bool linked = wait_queue_link(&q->send_wq, &entry, token, 0);
        spin_unlock_irqrestore(&g_mq_lock, flags);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&q->send_wq, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason)) {
            kfree(node);
            return -EINTR;
        }
        if (deadline != 0 && timer_get_ticks() >= deadline) {
            kfree(node);
            return -ETIMEDOUT;
        }
        flags = spin_lock_irqsave(&g_mq_lock);
    }

    /* Insert in priority order (highest first, FIFO within same prio). */
    mq_message_t **pp = &q->head;
    while (*pp && (*pp)->prio >= node->prio)
        pp = &(*pp)->next;
    node->next = *pp;
    *pp = node;
    if (!node->next)
        q->tail = node;
    q->mq_curmsgs++;

    int notify_pid = q->notify_pid;
    uint64_t notify_sig = q->notify_sig;
    if (notify_pid && !q->notify_pending) {
        q->notify_pending = 1;
    }
    wait_queue_wake_all(&q->recv_wq, 0, PROC_WAKE_EVENT);
    spin_unlock_irqrestore(&g_mq_lock, flags);

    if (notify_pid && notify_sig)
        signal_send(notify_pid, (int)notify_sig);
    return 0;
}

long posix_mq_timedreceive(int mqd, char *msg, size_t msg_len,
                           unsigned *prio, uint64_t deadline)
{
    mq_queue_t *q = mq_desc_get(mqd);
    if (!q)
        return -EBADF;

    uint64_t flags = spin_lock_irqsave(&g_mq_lock);
    for (;;) {
        if (!q->used) {
            spin_unlock_irqrestore(&g_mq_lock, flags);
            return -EBADF;
        }
        if (q->head) {
            mq_message_t *node = q->head;
            size_t n = node->size;
            if (n > msg_len) {
                spin_unlock_irqrestore(&g_mq_lock, flags);
                return -EMSGSIZE;
            }
            unsigned p = node->prio;
            q->head = node->next;
            if (!q->head)
                q->tail = NULL;
            q->mq_curmsgs--;
            /* A notification fires only when the queue was empty. */
            wait_queue_wake_all(&q->send_wq, 0, PROC_WAKE_EVENT);
            spin_unlock_irqrestore(&g_mq_lock, flags);

            if (n > 0)
                memcpy(msg, node->data, n);
            if (prio)
                *prio = p;
            kfree(node);
            return (long)n;
        }
        if (q->mq_flags & MQ_O_NONBLOCK) {
            spin_unlock_irqrestore(&g_mq_lock, flags);
            return -EAGAIN;
        }
        if (deadline != 0 && timer_get_ticks() >= deadline) {
            spin_unlock_irqrestore(&g_mq_lock, flags);
            return -ETIMEDOUT;
        }
        uint64_t wake = 0;
        if (deadline != 0)
            wake = deadline;
        spin_unlock_irqrestore(&g_mq_lock, flags);
        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE,
                                                    wake);
        if (!token.task)
            return -EAGAIN;
        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&g_mq_lock);
        if (!q->used || q->head) {
            spin_unlock_irqrestore(&g_mq_lock, flags);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            flags = spin_lock_irqsave(&g_mq_lock);
            continue;
        }
        bool linked = wait_queue_link(&q->recv_wq, &entry, token, 0);
        spin_unlock_irqrestore(&g_mq_lock, flags);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&q->recv_wq, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason))
            return -EINTR;
        if (deadline != 0 && timer_get_ticks() >= deadline)
            return -ETIMEDOUT;
        flags = spin_lock_irqsave(&g_mq_lock);
    }
}

int posix_mq_close(int mqd)
{
    if (mqd < 0 || mqd >= MQ_MAX_DESCS)
        return -EBADF;
    mq_queue_t *q = g_mq_desc[mqd];
    if (!q)
        return -EBADF;

    uint64_t flags = spin_lock_irqsave(&g_mq_desc_lock);
    g_mq_desc[mqd] = NULL;
    spin_unlock_irqrestore(&g_mq_desc_lock, flags);

    uint64_t qflags = spin_lock_irqsave(&g_mq_lock);
    if (q->used) {
        q->refcount--;
        if (q->refcount <= 0 && q->unlinked) {
            mq_message_t *n = q->head;
            while (n) {
                mq_message_t *nx = n->next;
                kfree(n);
                n = nx;
            }
            memset(q, 0, sizeof(*q));
        }
    }
    spin_unlock_irqrestore(&g_mq_lock, qflags);
    return 0;
}

void posix_mq_release_task(struct task_t *t)
{
    (void)t;
    /* Descriptors are kernel-global (mqd), not per-task; the fd-close path
     * in the ABI layer calls posix_mq_close when the mq fd is closed.  There
     * is no per-task descriptor storage to release here. */
}
