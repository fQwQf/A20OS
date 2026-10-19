#include "ipc/eventfd.h"
#include "ipc/envelope.h"
#include "ipc/ipc.h"

#include "core/consts.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/string.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/readiness.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "proc/proc.h"

typedef struct {
    spinlock_t    lock;
    wait_queue_t  readers;
    wait_queue_t  writers;
    uint64_t      counter;
    int           semaphore;
} eventfd_t;

int a20_eventfd_read_txn(uint64_t *counter, bool semaphore, void *context,
                         bool nonblock, uint64_t *value_out);
int a20_eventfd_write_txn(uint64_t *counter, void *context, bool nonblock,
                          uint64_t value);
int a20_eventfd_poll_txn(uint64_t *counter, void *context, int events);

/* Narrow wait/lock boundary for the Rust-owned eventfd transactions. A waiter
 * allocation owns one park token and one queue entry until finish, including
 * cancellation, link failure and interrupted wake paths. */
typedef struct {
    proc_wait_token_t token;
    wait_queue_entry_t entry;
    wait_queue_t *queue;
} eventfd_waiter_t;

void a20_eventfd_txn_lock(void *context)
{
    eventfd_t *efd = context;
    spin_lock(&efd->lock);
}

void a20_eventfd_txn_unlock(void *context)
{
    eventfd_t *efd = context;
    spin_unlock(&efd->lock);
}

void *a20_eventfd_txn_wait_prepare(void *context)
{
    (void)context;
    eventfd_waiter_t *waiter = kmalloc(sizeof(*waiter));
    if (!waiter)
        return NULL;
    memset(waiter, 0, sizeof(*waiter));
    waiter->token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
    if (!waiter->token.task) {
        kfree(waiter);
        return NULL;
    }
    return waiter;
}

bool a20_eventfd_txn_wait_link(void *context, void *opaque, bool writers)
{
    eventfd_t *efd = context;
    eventfd_waiter_t *waiter = opaque;
    wait_queue_t *queue = writers ? &efd->writers : &efd->readers;
    bool linked = wait_queue_link(queue, &waiter->entry, waiter->token, 0);
    waiter->queue = linked ? queue : NULL;
    return linked;
}

int a20_eventfd_txn_wait_finish(void *opaque, bool linked, bool commit)
{
    eventfd_waiter_t *waiter = opaque;
    proc_wake_reason_t reason = PROC_WAKE_CANCEL;
    if (commit && linked)
        reason = proc_park_commit(waiter->token);
    else
        (void)proc_park_cancel(waiter->token);
    if (linked && waiter->queue)
        wait_queue_unlink(waiter->queue, &waiter->entry);
    proc_park_finish(waiter->token);
    int interrupted = proc_wake_reason_is_task_interrupt(reason);
    kfree(waiter);
    return interrupted ? -ERESTARTSYS : 0;
}

void a20_eventfd_txn_wake(void *context, bool writers)
{
    eventfd_t *efd = context;
    wait_queue_wake_all(writers ? &efd->writers : &efd->readers,
                        0, PROC_WAKE_EVENT);
}

static int eventfd_read(vfile_t *vf, char *buf, size_t count)
{
    eventfd_t *efd = vf ? vf->priv : NULL;
    if (!efd) return -EBADF;
    if (count < sizeof(uint64_t)) return -EINVAL;

    uint64_t val;
    int result = a20_eventfd_read_txn(&efd->counter, efd->semaphore != 0,
                                      efd, (vf->flags & O_NONBLOCK) != 0, &val);
    if (result < 0)
        return result;
    memcpy(buf, &val, sizeof(val));
    return sizeof(val);
}

static int eventfd_write(vfile_t *vf, const char *buf, size_t count)
{
    eventfd_t *efd = vf ? vf->priv : NULL;
    if (!efd) return -EBADF;
    if (count != sizeof(uint64_t)) return -EINVAL;
    uint64_t val;
    memcpy(&val, buf, sizeof(val));

    int result = a20_eventfd_write_txn(&efd->counter, efd,
                                       (vf->flags & O_NONBLOCK) != 0, val);
    if (result < 0)
        return result;
    return sizeof(val);
}

static int eventfd_poll(vfile_t *vf, short events)
{
    eventfd_t *efd = vf ? vf->priv : NULL;
    if (!efd)
        return POLLNVAL;
    return a20_eventfd_poll_txn(&efd->counter, efd, events);
}

static size_t eventfd_poll_sources(vfile_t *vf, short events,
                                   readiness_source_t *sources, size_t max)
{
    eventfd_t *efd = vf ? vf->priv : NULL;
    if (!efd || !sources)
        return 0;
    size_t count = 0;
    if ((events & POLLIN) && count < max)
        sources[count++] = (readiness_source_t){ &efd->readers, 0, 0 };
    if ((events & POLLOUT) && count < max)
        sources[count++] = (readiness_source_t){ &efd->writers, 0, 0 };
    return count;
}

static vfile_ops_t g_eventfd_ops = {
    .read = eventfd_read,
    .write = eventfd_write,
    .poll = eventfd_poll,
    .poll_sources = eventfd_poll_sources,
    .close = anonfd_free_priv_close,
};

int eventfd_vfile_is(vfile_t *vf)
{
    return vf && vf->ops == &g_eventfd_ops;
}

int eventfd_create(unsigned initval, int flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK | 1)) return -EINVAL;
    eventfd_t *efd = kmalloc(sizeof(*efd));
    vfile_t *vf = vfile_alloc();
    if (!efd || !vf) {
        if (efd) kfree(efd);
        if (vf) vfile_free(vf);
        return -ENOMEM;
    }
    memset(efd, 0, sizeof(*efd));
    spin_init(&efd->lock);
    wait_queue_init(&efd->readers);
    wait_queue_init(&efd->writers);
    efd->counter = initval;
    efd->semaphore = (flags & 1) != 0;
    vf->flags = O_RDWR | (flags & O_NONBLOCK);
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_eventfd_ops;
    vf->priv = efd;
    if (env_kind_tag_vfile(vf, A20_OBJ_EVENT_QUEUE) < 0) {
        vfile_free(vf);
        kfree(efd);
        return -EINVAL;
    }
    return anonfd_install_vfile(vf, flags);
}
