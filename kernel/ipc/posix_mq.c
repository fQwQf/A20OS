#include "proc/resource_ns.h"
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
    uint64_t namespace_ino;
    char name[MQ_NAME_MAX];
    int64_t mq_flags;
    int64_t mq_maxmsg;
    int64_t mq_msgsize;
    int64_t mq_curmsgs;
    mq_message_t *head;
    mq_message_t *tail;
    int unlinked;
    int refcount;          /* open descriptor count */
    uint64_t notify_sig;
    int notify_pid;
    int notify_pending;
    wait_queue_t send_wq;
    wait_queue_t recv_wq;
    spinlock_t lock;
} mq_queue_t;

static mq_queue_t g_mq[MQ_MAX_QUEUES];
static spinlock_t g_mq_lock = SPINLOCK_INIT;

/* Per-task descriptor table (mqd -> queue pointer). */
#define MQ_MAX_DESCS 256
static mq_queue_t *g_mq_desc[MQ_MAX_DESCS];
static int g_mq_desc_nonblock[MQ_MAX_DESCS];
static spinlock_t g_mq_desc_lock = SPINLOCK_INIT;

#ifdef CONFIG_POSIX_MQ_RUST
int a20_posix_mq_notify_txn(void *context, int mqd, const unsigned char *sigevent);
int a20_posix_mq_open_txn(void *queues, size_t count, void *context,
                          const char *name, int oflag, const mq_attr_kern_t *attr);
int a20_posix_mq_unlink_txn(void *queues, size_t count, void *context, const char *name);
int a20_posix_mq_getsetattr_txn(void *context, int mqd,
                                const mq_attr_kern_t *newattr,
                                mq_attr_kern_t *oldattr);
int a20_posix_mq_close_txn(void *context, int mqd);
void a20_posix_mq_namespace_destroy_txn(void *queues, size_t count,
                                        void *context, uint64_t ino);
#endif

#ifndef CONFIG_POSIX_MQ_RUST
static mq_queue_t *mq_lookup_locked(const char *name)
{
    for (int i = 0; i < MQ_MAX_QUEUES; i++)
        if (g_mq[i].used && !g_mq[i].unlinked && g_mq[i].namespace_ino == resource_ns_ino(PROCNS_IPC) && strcmp(g_mq[i].name, name) == 0)
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
    q->namespace_ino = resource_ns_ino(PROCNS_IPC);
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
#endif

static int mq_alloc_descriptor(mq_queue_t *q, int nonblock)
{
    uint64_t flags = spin_lock_irqsave(&g_mq_desc_lock);
    for (int i = 0; i < MQ_MAX_DESCS; i++) {
        if (!g_mq_desc[i]) {
            g_mq_desc[i] = q;
            g_mq_desc_nonblock[i] = nonblock;
            spin_unlock_irqrestore(&g_mq_desc_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&g_mq_desc_lock, flags);
    return -EMFILE;
}

#ifdef CONFIG_POSIX_MQ_RUST
void a20_posix_mq_queue_reset(void *context, void *queue_opaque)
{
    (void)context;
    mq_queue_t *q = queue_opaque;
    memset(q, 0, sizeof(*q));
    wait_queue_init(&q->send_wq);
    wait_queue_init(&q->recv_wq);
    spin_init(&q->lock);
}

int a20_posix_mq_desc_install(void *context, void *queue, int nonblock)
{
    (void)context;
    return mq_alloc_descriptor(queue, nonblock);
}

void *a20_posix_mq_desc_take(void *context, int mqd)
{
    (void)context;
    if (mqd < 0 || mqd >= MQ_MAX_DESCS) return NULL;
    uint64_t flags = spin_lock_irqsave(&g_mq_desc_lock);
    mq_queue_t *q = g_mq_desc[mqd];
    g_mq_desc[mqd] = NULL;
    g_mq_desc_nonblock[mqd] = 0;
    spin_unlock_irqrestore(&g_mq_desc_lock, flags);
    return q;
}

void *a20_posix_mq_desc_snapshot(void *context, int mqd, int *nonblock)
{
    (void)context;
    if (mqd < 0 || mqd >= MQ_MAX_DESCS) return NULL;
    uint64_t flags = spin_lock_irqsave(&g_mq_desc_lock);
    mq_queue_t *q = g_mq_desc[mqd];
    if (nonblock) *nonblock = q ? g_mq_desc_nonblock[mqd] : 0;
    spin_unlock_irqrestore(&g_mq_desc_lock, flags);
    return q;
}

int a20_posix_mq_desc_set_nonblock(void *context, int mqd, void *queue, int value)
{
    (void)context;
    if (mqd < 0 || mqd >= MQ_MAX_DESCS) return -EBADF;
    uint64_t flags = spin_lock_irqsave(&g_mq_desc_lock);
    if (g_mq_desc[mqd] != queue) {
        spin_unlock_irqrestore(&g_mq_desc_lock, flags);
        return -EBADF;
    }
    g_mq_desc_nonblock[mqd] = value;
    spin_unlock_irqrestore(&g_mq_desc_lock, flags);
    return 0;
}

uint64_t a20_posix_mq_namespace(void *context)
{
    (void)context;
    return resource_ns_ino(PROCNS_IPC);
}

int a20_posix_mq_pid(void *context)
{
    (void)context;
    task_t *task = proc_current();
    return task ? task->pid : 0;
}
#endif

static mq_queue_t *mq_desc_get(int mqd)
{
    if (mqd < 0 || mqd >= MQ_MAX_DESCS)
        return NULL;
    return g_mq_desc[mqd];
}

static int mq_desc_nonblock(int mqd)
{
    if (mqd < 0 || mqd >= MQ_MAX_DESCS)
        return 0;
    uint64_t flags = spin_lock_irqsave(&g_mq_desc_lock);
    int nonblock = g_mq_desc[mqd] ? g_mq_desc_nonblock[mqd] : 0;
    spin_unlock_irqrestore(&g_mq_desc_lock, flags);
    return nonblock;
}

int posix_mq_open(const char *name, int oflag, int mode,
                  const mq_attr_kern_t *attr)
{
#ifdef CONFIG_POSIX_MQ_RUST
    (void)mode;
    return a20_posix_mq_open_txn(g_mq, MQ_MAX_QUEUES, NULL, name, oflag, attr);
#else

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
    int mqd = mq_alloc_descriptor(q, (oflag & MQ_O_NONBLOCK) != 0);
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

#endif
}

int posix_mq_unlink(const char *name)
{
#ifdef CONFIG_POSIX_MQ_RUST
    return a20_posix_mq_unlink_txn(g_mq, MQ_MAX_QUEUES, NULL, name);
#else

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

#endif
}

int posix_mq_getsetattr(int mqd, const mq_attr_kern_t *newattr,
                        mq_attr_kern_t *oldattr)
{
#ifdef CONFIG_POSIX_MQ_RUST
    return a20_posix_mq_getsetattr_txn(NULL, mqd, newattr, oldattr);
#else

    mq_queue_t *q = mq_desc_get(mqd);
    if (!q)
        return -EBADF;
    uint64_t flags = spin_lock_irqsave(&g_mq_lock);
    if (!q->used) {
        spin_unlock_irqrestore(&g_mq_lock, flags);
        return -EBADF;
    }
    mq_attr_kern_t attr;
    attr.mq_flags = mq_desc_nonblock(mqd) ? MQ_O_NONBLOCK : 0;
    attr.mq_maxmsg = q->mq_maxmsg;
    attr.mq_msgsize = q->mq_msgsize;
    attr.mq_curmsgs = q->mq_curmsgs;
    if (newattr) {
        /* Only mq_flags is modifiable; the rest are fixed by the kernel. */
        uint64_t dflags = spin_lock_irqsave(&g_mq_desc_lock);
        if (g_mq_desc[mqd] != q) {
            spin_unlock_irqrestore(&g_mq_desc_lock, dflags);
            spin_unlock_irqrestore(&g_mq_lock, flags);
            return -EBADF;
        }
        g_mq_desc_nonblock[mqd] = (newattr->mq_flags & MQ_O_NONBLOCK) != 0;
        spin_unlock_irqrestore(&g_mq_desc_lock, dflags);
        attr.mq_flags = newattr->mq_flags & MQ_O_NONBLOCK;
    }
    spin_unlock_irqrestore(&g_mq_lock, flags);
    if (oldattr)
        *oldattr = attr;
    return 0;

#endif
}

int posix_mq_notify(int mqd, const void *sigevent)
{
#ifdef CONFIG_POSIX_MQ_RUST
    unsigned char ev[24];
    if (sigevent && copy_from_user(ev, sigevent, sizeof(ev)) < 0)
        return -EFAULT;
    return a20_posix_mq_notify_txn(NULL, mqd, sigevent ? ev : NULL);
#else
    mq_queue_t *q = mq_desc_get(mqd);
    if (!q)
        return -EBADF;
    unsigned char ev[24];
    int signo = 0;
    if (sigevent) {
        if (copy_from_user(ev, sigevent, sizeof(ev)) < 0)
            return -EFAULT;
        memcpy(&signo, ev + 8, sizeof(signo));
        if (signo <= 0 || signo >= 64)
            return -EINVAL;
    }
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
    if (q->notify_pid != 0) {
        spin_unlock_irqrestore(&g_mq_lock, flags);
        return -EBUSY;
    }
    q->notify_sig = (uint64_t)(int)signo;
    q->notify_pid = proc_current() ? proc_current()->pid : 0;
    q->notify_pending = 0;
    spin_unlock_irqrestore(&g_mq_lock, flags);
    return 0;
#endif
}

#ifdef CONFIG_POSIX_MQ_RUST
STATIC_ASSERT(__builtin_offsetof(mq_message_t, prio) == (sizeof(void *) == 8 ? 8 : 4), posix_mq_rust_message_prio);
STATIC_ASSERT(__builtin_offsetof(mq_message_t, size) == (sizeof(void *) == 8 ? 16 : 8), posix_mq_rust_message_size);
STATIC_ASSERT(sizeof(mq_message_t) == (sizeof(void *) == 8 ? 24 : 12), posix_mq_rust_message_layout);
STATIC_ASSERT(__builtin_offsetof(mq_queue_t, mq_flags) == 272, posix_mq_rust_flags);
STATIC_ASSERT(__builtin_offsetof(mq_queue_t, head) == 304, posix_mq_rust_head);
STATIC_ASSERT(__builtin_offsetof(mq_queue_t, tail) == (sizeof(void *) == 8 ? 312 : 308), posix_mq_rust_tail);
STATIC_ASSERT(__builtin_offsetof(mq_queue_t, unlinked) == (sizeof(void *) == 8 ? 320 : 312), posix_mq_rust_unlinked);
STATIC_ASSERT(__builtin_offsetof(mq_queue_t, notify_sig) == (sizeof(void *) == 8 ? 328 : 320), posix_mq_rust_notify_sig);
STATIC_ASSERT(__builtin_offsetof(mq_queue_t, notify_pid) == (sizeof(void *) == 8 ? 336 : 328), posix_mq_rust_notify_pid);
STATIC_ASSERT(__builtin_offsetof(mq_queue_t, notify_pending) == (sizeof(void *) == 8 ? 340 : 332), posix_mq_rust_notify_pending);
STATIC_ASSERT(sizeof(mq_queue_t) == (sizeof(void *) == 8 ? 616 : 536), posix_mq_rust_queue_layout);

int a20_posix_mq_send_txn(void *context, int mqd, const void *msg,
                          size_t msg_len, unsigned prio, uint64_t deadline);
int64_t a20_posix_mq_receive_txn(void *context, int mqd, void *msg,
                                 size_t msg_len, unsigned *prio,
                                 uint64_t deadline);
int a20_posix_mq_open_txn(void *queues, size_t count, void *context,
                          const char *name, int oflag, const mq_attr_kern_t *attr);
int a20_posix_mq_unlink_txn(void *queues, size_t count, void *context, const char *name);
int a20_posix_mq_getsetattr_txn(void *context, int mqd,
                                const mq_attr_kern_t *newattr,
                                mq_attr_kern_t *oldattr);
int a20_posix_mq_close_txn(void *context, int mqd);
int a20_posix_mq_notify_txn(void *context, int mqd, const unsigned char *sigevent);
void a20_posix_mq_namespace_destroy_txn(void *queues, size_t count,
                                        void *context, uint64_t ino);

typedef struct {
    proc_wait_token_t token;
    wait_queue_entry_t entry;
    wait_queue_t *queue;
} posix_mq_waiter_t;

void *a20_posix_mq_desc_get(void *context, int mqd)
{
    (void)context;
    return mq_desc_get(mqd);
}

int a20_posix_mq_desc_nonblock(void *context, int mqd)
{
    (void)context;
    return mq_desc_nonblock(mqd);
}

uint64_t a20_posix_mq_txn_lock(void *context)
{
    (void)context;
    return spin_lock_irqsave(&g_mq_lock);
}

void a20_posix_mq_txn_unlock(void *context, uint64_t flags)
{
    (void)context;
    spin_unlock_irqrestore(&g_mq_lock, flags);
}

uint64_t a20_posix_mq_txn_now(void *context)
{
    (void)context;
    return timer_get_ticks();
}

void *a20_posix_mq_message_alloc(void *context, const void *msg,
                                 size_t msg_len, unsigned prio)
{
    (void)context;
    mq_message_t *node = kmalloc(sizeof(*node) + msg_len);
    if (!node)
        return NULL;
    node->next = NULL;
    node->prio = prio;
    node->size = msg_len;
    if (msg_len > 0)
        memcpy(node->data, msg, msg_len);
    return node;
}

void a20_posix_mq_message_free(void *context, void *opaque)
{
    (void)context;
    kfree(opaque);
}

void a20_posix_mq_message_copy_out(void *context, void *opaque, void *msg,
                                   size_t msg_len, unsigned *prio)
{
    (void)context;
    (void)msg_len;
    mq_message_t *node = opaque;
    if (node->size > 0)
        memcpy(msg, node->data, node->size);
    if (prio)
        *prio = node->prio;
}

void *a20_posix_mq_wait_prepare(void *context, uint64_t deadline)
{
    posix_mq_waiter_t *waiter = context;
    if (!waiter)
        return NULL;
    memset(waiter, 0, sizeof(*waiter));
    waiter->token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
    if (!waiter->token.task)
        return NULL;
    return waiter;
}

bool a20_posix_mq_wait_link(void *context, void *opaque, void *queue_opaque,
                            bool send)
{
    (void)context;
    posix_mq_waiter_t *waiter = opaque;
    mq_queue_t *queue = queue_opaque;
    wait_queue_t *wq = send ? &queue->send_wq : &queue->recv_wq;
    bool linked = wait_queue_link(wq, &waiter->entry, waiter->token, 0);
    waiter->queue = linked ? wq : NULL;
    return linked;
}

bool a20_posix_mq_wait_finish(void *opaque, bool linked, bool commit)
{
    posix_mq_waiter_t *waiter = opaque;
    proc_wake_reason_t reason;
    if (commit && linked)
        reason = proc_park_commit(waiter->token);
    else {
        (void)proc_park_cancel(waiter->token);
        reason = PROC_WAKE_CANCEL;
    }
    if (linked && waiter->queue)
        wait_queue_unlink(waiter->queue, &waiter->entry);
    proc_park_finish(waiter->token);
    bool interrupted = proc_wake_reason_is_task_interrupt(reason);
    return interrupted;
}

void a20_posix_mq_wake(void *context, void *queue_opaque, bool send)
{
    (void)context;
    mq_queue_t *queue = queue_opaque;
    wait_queue_wake_all(send ? &queue->send_wq : &queue->recv_wq,
                        0, PROC_WAKE_EVENT);
}

void a20_posix_mq_signal(void *context, int pid, int signo)
{
    (void)context;
    signal_send(pid, signo);
}
#endif

#ifndef CONFIG_POSIX_MQ_RUST
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
        if (mq_desc_nonblock(mqd)) {
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
    bool was_empty = q->mq_curmsgs == 0;
    q->mq_curmsgs++;

    int notify_pid = q->notify_pid;
    uint64_t notify_sig = q->notify_sig;
    if (was_empty && notify_pid && !q->notify_pending) {
        q->notify_pending = 1;
        q->notify_pid = 0;
        q->notify_sig = 0;
    } else {
        notify_pid = 0;
        notify_sig = 0;
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
        if (mq_desc_nonblock(mqd)) {
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
#else
int posix_mq_timedsend(int mqd, const char *msg, size_t msg_len,
                       unsigned prio, uint64_t deadline)
{
    posix_mq_waiter_t waiter = {0};
    return a20_posix_mq_send_txn(&waiter, mqd, msg, msg_len, prio, deadline);
}

long posix_mq_timedreceive(int mqd, char *msg, size_t msg_len,
                           unsigned *prio, uint64_t deadline)
{
    posix_mq_waiter_t waiter = {0};
    return (long)a20_posix_mq_receive_txn(&waiter, mqd, msg, msg_len, prio,
                                          deadline);
}
#endif

int posix_mq_close(int mqd)
{
#ifdef CONFIG_POSIX_MQ_RUST
    return a20_posix_mq_close_txn(NULL, mqd);
#else

    if (mqd < 0 || mqd >= MQ_MAX_DESCS)
        return -EBADF;
    /* Match open's g_mq_lock -> descriptor-lock order so slot reuse cannot
     * race the queue pointer read or refcount update. */
    uint64_t qflags = spin_lock_irqsave(&g_mq_lock);
    uint64_t flags = spin_lock_irqsave(&g_mq_desc_lock);
    mq_queue_t *q = g_mq_desc[mqd];
    if (!q) {
        spin_unlock_irqrestore(&g_mq_desc_lock, flags);
        spin_unlock_irqrestore(&g_mq_lock, qflags);
        return -EBADF;
    }
    g_mq_desc[mqd] = NULL;
    g_mq_desc_nonblock[mqd] = 0;
    spin_unlock_irqrestore(&g_mq_desc_lock, flags);

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

#endif
}

void posix_mq_release_task(struct task_t *t)
{
    (void)t;
    /* Descriptors are kernel-global (mqd), not per-task; the fd-close path
     * in the ABI layer calls posix_mq_close when the mq fd is closed.  There
     * is no per-task descriptor storage to release here. */
}

void posix_mq_namespace_destroy(uint64_t ino)
{
#ifdef CONFIG_POSIX_MQ_RUST
    a20_posix_mq_namespace_destroy_txn(g_mq, MQ_MAX_QUEUES, NULL, ino);
#else
    uint64_t irq = spin_lock_irqsave(&g_mq_lock);
    for (int i = 0; i < MQ_MAX_QUEUES; i++) {
        mq_queue_t *q = &g_mq[i];
        if (!q->used || q->namespace_ino != ino) continue;
        q->unlinked = 1;
        q->name[0] = 0;
        if (q->refcount) continue;
        mq_message_t *m = q->head;
        while (m) { mq_message_t *next = m->next; kfree(m); m = next; }
        memset(q, 0, sizeof(*q));
    }
    spin_unlock_irqrestore(&g_mq_lock, irq);
#endif
}
