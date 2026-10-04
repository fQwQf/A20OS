#include "fs/aio.h"

#include "core/consts.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "proc/park.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

/*
 * Kernel AIO context table.
 *
 * Each context owns a completion queue of aio_event_kern_t records plus a
 * list of pending submissions (for io_cancel).  The queue is bounded by the
 * context's nr_events; when full, new submissions block until the consumer
 * drains events.  Because A20OS VFS I/O is synchronous, io_submit() executes
 * each supported opcode inline and publishes the completion immediately; the
 * event is therefore almost always available when io_getevents() runs, which
 * keeps the common libaio/glibc flow simple and correct.
 */

#define AIO_HASH_BUCKETS 64
/*
 * A submission is only "pending" between the moment io_submit() publishes it
 * and the moment synchronous execution inserts its completion, so at most one
 * entry per concurrently-submitting task is live.  The pool is inline rather
 * than a kmalloc'd list so that no slab round trip ever happens under
 * c->lock; when it is exhausted the iocb simply goes untracked, and
 * io_cancel() then reaches the same -EAGAIN it reports for a request that is
 * executing (an untracked iocb is by definition still executing).
 */
#define AIO_MAX_PENDING 64

typedef struct aio_ctx {
    int used;
    int dying;                  /* io_destroy started: reject new users */
    int user_refs;              /* in-flight users (protected by table lock) */
    struct mm_struct *mm;       /* owning address space */
    uint64_t ctx;               /* opaque context id handed to user */
    unsigned nr_events;         /* queue capacity */
    spinlock_t lock;
    wait_queue_t readers;       /* io_getevents waiters */
    wait_queue_t writers;       /* io_submit waiters when the queue is full */
    aio_event_kern_t *events;   /* completion ring, capacity nr_events */
    unsigned head;
    unsigned count;
    struct aio_ctx *hnext;      /* hash chain, protected by table lock */
    uint64_t pending[AIO_MAX_PENDING];
    unsigned pending_count;
} aio_ctx_t;

static aio_ctx_t g_aio[AIO_MAX_CONTEXTS];
static aio_ctx_t *g_aio_hash[AIO_HASH_BUCKETS];
static spinlock_t g_aio_table_lock = SPINLOCK_INIT;
static uint64_t g_aio_next_id = 1;

static unsigned aio_ctx_bucket(uint64_t ctx)
{
    uint64_t h = ctx * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 32;
    return (unsigned)(h & (AIO_HASH_BUCKETS - 1));
}

/* Caller holds g_aio_table_lock. */
static void aio_ctx_link_locked(aio_ctx_t *c)
{
    unsigned b = aio_ctx_bucket(c->ctx);
    c->hnext = g_aio_hash[b];
    g_aio_hash[b] = c;
}

/* Unpublish a context and hand its completion ring back to the caller.  The
 * ring is deliberately *not* freed here: kfree() must not run with the global
 * table lock held and interrupts disabled, so every caller drops it after
 * dropping the lock. */
static aio_event_kern_t *aio_ctx_release_locked(aio_ctx_t *c)
{
    unsigned b = aio_ctx_bucket(c->ctx);
    aio_ctx_t **pp = &g_aio_hash[b];
    while (*pp && *pp != c)
        pp = &(*pp)->hnext;
    if (*pp)
        *pp = c->hnext;
    aio_event_kern_t *events = c->events;
    c->used = 0;
    c->events = NULL;
    return events;
}

static aio_ctx_t *aio_ctx_lookup(struct mm_struct *mm, uint64_t ctx)
{
    if (!ctx)
        return NULL;
    for (aio_ctx_t *c = g_aio_hash[aio_ctx_bucket(ctx)]; c; c = c->hnext) {
        if (c->used && c->mm == mm && c->ctx == ctx)
            return c;
    }
    return NULL;
}

/* Look up a context and take a user reference for the duration of one
 * operation.  Returns NULL if the context does not exist or is dying.  The
 * caller must pair this with aio_ctx_put(). */
static aio_ctx_t *aio_ctx_get(struct mm_struct *mm, uint64_t ctx)
{
    unsigned long flags = spin_lock_irqsave(&g_aio_table_lock);
    aio_ctx_t *c = aio_ctx_lookup(mm, ctx);
    if (c && !c->dying)
        c->user_refs++;
    else
        c = NULL;
    spin_unlock_irqrestore(&g_aio_table_lock, flags);
    return c;
}

/* Drop a user reference; if the context is dying and this was the last
 * user, free it (the destroy path marked it dying but deferred the free). */
static void aio_ctx_put(aio_ctx_t *c)
{
    if (!c)
        return;
    aio_event_kern_t *events = NULL;
    unsigned long flags = spin_lock_irqsave(&g_aio_table_lock);
    if (c->used && c->user_refs > 0)
        c->user_refs--;
    if (c->used && c->dying && c->user_refs == 0)
        events = aio_ctx_release_locked(c);
    spin_unlock_irqrestore(&g_aio_table_lock, flags);
    kfree(events);
}

static int aio_ctx_insert_events_locked(aio_ctx_t *c, aio_event_kern_t *ev)
{
    if (c->count >= c->nr_events)
        return -EAGAIN;
    c->events[(c->head + c->count) % c->nr_events] = *ev;
    c->count++;
    return 0;
}

static void aio_ctx_wake_readers_locked(aio_ctx_t *c)
{
    if (c->count > 0)
        wait_queue_wake_all(&c->readers, 0, PROC_WAKE_EVENT);
}

static void aio_ctx_push_pending_locked(aio_ctx_t *c, uint64_t obj)
{
    if (c->pending_count < AIO_MAX_PENDING)
        c->pending[c->pending_count++] = obj;
}

static void aio_ctx_remove_pending_locked(aio_ctx_t *c, uint64_t obj)
{
    for (unsigned i = 0; i < c->pending_count; i++) {
        if (c->pending[i] != obj)
            continue;
        c->pending[i] = c->pending[--c->pending_count];
        return;
    }
}

static void aio_ctx_drop_all_pending_locked(aio_ctx_t *c)
{
    c->pending_count = 0;
}

int aio_context_create(struct mm_struct *mm, unsigned nr_events,
                       uint64_t *ctx_out)
{
    if (!mm || !ctx_out)
        return -EINVAL;
    if (nr_events == 0 || nr_events > AIO_MAX_EVENTS)
        return -EINVAL;

    unsigned long flags = spin_lock_irqsave(&g_aio_table_lock);
    aio_ctx_t *slot = NULL;
    for (int i = 0; i < AIO_MAX_CONTEXTS; i++) {
        if (!g_aio[i].used) {
            slot = &g_aio[i];
            break;
        }
    }
    if (!slot) {
        spin_unlock_irqrestore(&g_aio_table_lock, flags);
        return -EAGAIN;
    }

    aio_event_kern_t *ring = kcalloc(nr_events, sizeof(*ring));
    if (!ring) {
        spin_unlock_irqrestore(&g_aio_table_lock, flags);
        return -ENOMEM;
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->mm = mm;
    slot->ctx = g_aio_next_id++;
    if (slot->ctx == 0)
        slot->ctx = g_aio_next_id++;
    slot->nr_events = nr_events;
    spin_init(&slot->lock);
    wait_queue_init(&slot->readers);
    wait_queue_init(&slot->writers);
    slot->events = ring;
    aio_ctx_link_locked(slot);
    spin_unlock_irqrestore(&g_aio_table_lock, flags);

    *ctx_out = slot->ctx;
    return 0;
}

int aio_context_destroy(struct mm_struct *mm, uint64_t ctx)
{
    aio_event_kern_t *events = NULL;
    unsigned long flags = spin_lock_irqsave(&g_aio_table_lock);
    aio_ctx_t *c = aio_ctx_lookup(mm, ctx);
    if (!c) {
        spin_unlock_irqrestore(&g_aio_table_lock, flags);
        return -EINVAL;
    }
    /* Mark dying so new operations reject the context, then free the ring
     * once every in-flight user has released its reference. */
    c->dying = 1;
    spin_lock(&c->lock);
    aio_ctx_drop_all_pending_locked(c);
    /* Wake any thread blocked in io_getevents: the context is dying so it
     * will observe an empty queue and return. */
    wait_queue_wake_all(&c->readers, 0, PROC_WAKE_EVENT);
    wait_queue_wake_all(&c->writers, 0, PROC_WAKE_EVENT);
    spin_unlock(&c->lock);
    if (c->user_refs == 0)
        events = aio_ctx_release_locked(c);
    spin_unlock_irqrestore(&g_aio_table_lock, flags);
    kfree(events);
    return 0;
}

/* ---- synchronous iocb execution ---- */

/* pread/pwrite to a user buffer through the VFS, chunked over page segments. */
static int64_t aio_do_pread(int fd, uint64_t buf, uint64_t nbytes,
                            int64_t offset)
{
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf)
        return -EBADF;
    if (!vf->ops || !vf->ops->lseek) {
        vfs_put_file_ref((int)gfd, vf);
        return -ESPIPE;
    }

    mutex_lock(&vf->offset_lock);
    long saved = vf->ops->lseek(vf, 0, SEEK_CUR);
    if (saved < 0) {
        mutex_unlock(&vf->offset_lock);
        vfs_put_file_ref((int)gfd, vf);
        return saved;
    }
    if (vf->ops->lseek(vf, (long)offset, SEEK_SET) < 0) {
        mutex_unlock(&vf->offset_lock);
        vfs_put_file_ref((int)gfd, vf);
        return -ESPIPE;
    }

    int64_t total = 0;
    while ((size_t)total < nbytes) {
        void *kaddr;
        size_t chunk;
        if (user_buffer_segment((const void *)(uintptr_t)(buf + total),
                                (size_t)(nbytes - (size_t)total), 1,
                                &kaddr, &chunk) < 0) {
            if (total == 0) total = -EFAULT;
            break;
        }
        if (chunk > nbytes - (size_t)total)
            chunk = nbytes - (size_t)total;
        int64_t n = vfs_read_file(vf, kaddr, chunk);
        if (n <= 0) {
            if (total == 0)
                total = n;
            break;
        }
        total += n;
        if ((size_t)n < chunk)
            break;
    }
    long restore_r = vf->ops->lseek(vf, saved, SEEK_SET);
    mutex_unlock(&vf->offset_lock);
    vfs_put_file_ref((int)gfd, vf);
    if (restore_r < 0 && total >= 0)
        return restore_r;
    return total;
}

static int64_t aio_do_pwrite(int fd, uint64_t buf, uint64_t nbytes,
                             int64_t offset)
{
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf)
        return -EBADF;
    if (!vf->ops || !vf->ops->lseek) {
        vfs_put_file_ref((int)gfd, vf);
        return -ESPIPE;
    }

    mutex_lock(&vf->offset_lock);
    long saved = vf->ops->lseek(vf, 0, SEEK_CUR);
    if (saved < 0) {
        mutex_unlock(&vf->offset_lock);
        vfs_put_file_ref((int)gfd, vf);
        return saved;
    }
    if (vf->ops->lseek(vf, (long)offset, SEEK_SET) < 0) {
        mutex_unlock(&vf->offset_lock);
        vfs_put_file_ref((int)gfd, vf);
        return -ESPIPE;
    }

    int64_t total = 0;
    while ((size_t)total < nbytes) {
        void *kaddr;
        size_t chunk;
        if (user_buffer_segment((const void *)(uintptr_t)(buf + total),
                                (size_t)(nbytes - (size_t)total), 0,
                                &kaddr, &chunk) < 0) {
            if (total == 0) total = -EFAULT;
            break;
        }
        if (chunk > nbytes - (size_t)total)
            chunk = nbytes - (size_t)total;
        int64_t n = vfs_write_file(vf, kaddr, chunk);
        if (n <= 0) {
            if (total == 0)
                total = n;
            break;
        }
        total += n;
    }
    long restore_r = vf->ops->lseek(vf, saved, SEEK_SET);
    mutex_unlock(&vf->offset_lock);
    vfs_put_file_ref((int)gfd, vf);
    if (restore_r < 0 && total >= 0)
        return restore_r;
    return total;
}

static int64_t aio_do_fsync(int fd, int datasync)
{
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return gfd;
    return datasync ? vfs_fdatasync((int)gfd) : vfs_fsync((int)gfd);
}

static int64_t aio_exec_iocb(const aio_iocb_kern_t *iocb)
{
    switch (iocb->opcode) {
    case IOCB_CMD_PREAD:
        return aio_do_pread(iocb->fd, iocb->buf, iocb->nbytes, iocb->offset);
    case IOCB_CMD_PWRITE:
        return aio_do_pwrite(iocb->fd, iocb->buf, iocb->nbytes, iocb->offset);
    case IOCB_CMD_FSYNC:
        return aio_do_fsync(iocb->fd, 0);
    case IOCB_CMD_FDSYNC:
        return aio_do_fsync(iocb->fd, 1);
    default:
        return -EINVAL;
    }
}

long aio_context_submit(struct mm_struct *mm, uint64_t ctx,
                        const aio_iocb_kern_t *iocbs, long nr)
{
    if (!mm || !iocbs || nr <= 0)
        return -EINVAL;

    aio_ctx_t *c = aio_ctx_get(mm, ctx);
    if (!c)
        return -EINVAL;

    long submitted = 0;
    for (long i = 0; i < nr; i++) {
        const aio_iocb_kern_t *iocb = &iocbs[i];
        aio_event_kern_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.data = iocb->data;
        ev.obj = iocb->obj;

        /* Queue the iocb as pending so io_cancel() can find it before
         * execution, then execute synchronously. */
        spin_lock(&c->lock);
        if (c->count >= c->nr_events) {
            /* Ring full: wait for a consumer. */
            aio_ctx_push_pending_locked(c, iocb->obj);
            for (;;) {
                proc_wait_token_t token =
                    proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
                if (!token.task) {
                    spin_lock(&c->lock);
                    aio_ctx_remove_pending_locked(c, iocb->obj);
                    spin_unlock(&c->lock);
                    aio_ctx_put(c);
                    return submitted ? submitted : -EAGAIN;
                }
                wait_queue_entry_t entry = {0};
                bool linked = wait_queue_link(&c->writers, &entry, token, 0);
                spin_unlock(&c->lock);
                proc_wake_reason_t reason;
                if (linked)
                    reason = proc_park_commit(token);
                else {
                    (void)proc_park_cancel(token);
                    reason = PROC_WAKE_CANCEL;
                }
                wait_queue_unlink(&c->writers, &entry);
                proc_park_finish(token);
                if (proc_wake_reason_is_task_interrupt(reason)) {
                    spin_lock(&c->lock);
                    aio_ctx_remove_pending_locked(c, iocb->obj);
                    spin_unlock(&c->lock);
                    aio_ctx_put(c);
                    return submitted ? submitted : -EINTR;
                }
                spin_lock(&c->lock);
                if (c->count < c->nr_events) {
                    aio_ctx_remove_pending_locked(c, iocb->obj);
                    break;
                }
            }
        } else {
            aio_ctx_push_pending_locked(c, iocb->obj);
        }

        /* Execute with the ring lock released so VFS I/O can sleep. */
        spin_unlock(&c->lock);
        int64_t res = aio_exec_iocb(iocb);
        ev.res = res;
        ev.res2 = 0;

        spin_lock(&c->lock);
        aio_ctx_remove_pending_locked(c, iocb->obj);
        if (aio_ctx_insert_events_locked(c, &ev) == 0) {
            aio_ctx_wake_readers_locked(c);
            submitted++;
        }
        wait_queue_wake_all(&c->writers, 0, PROC_WAKE_EVENT);
        spin_unlock(&c->lock);
    }
    aio_ctx_put(c);
    return submitted;
}

long aio_context_getevents(struct mm_struct *mm, uint64_t ctx, long min_nr,
                           long nr, aio_event_kern_t *events,
                           uint64_t deadline)
{
    if (!mm || !events || nr <= 0)
        return -EINVAL;

    aio_ctx_t *c = aio_ctx_get(mm, ctx);
    if (!c)
        return -EINVAL;

    uint64_t now = timer_get_ticks();
    for (;;) {
        spin_lock(&c->lock);
        if (c->dying) {
            /* io_destroy() ran while we were waiting; drain whatever is left
             * and return. */
            long take = (long)c->count < nr ? (long)c->count : nr;
            for (long i = 0; i < take; i++)
                events[i] = c->events[(c->head + (unsigned)i) % c->nr_events];
            c->head = (c->head + (unsigned)take) % c->nr_events;
            c->count -= (unsigned)take;
            wait_queue_wake_all(&c->writers, 0, PROC_WAKE_EVENT);
            spin_unlock(&c->lock);
            aio_ctx_put(c);
            return take;
        }
        if (c->count > 0) {
            /* Copy events to the caller's kernel buffer. */
            long take = (long)c->count < nr ? (long)c->count : nr;
            for (long i = 0; i < take; i++)
                events[i] = c->events[(c->head + (unsigned)i) % c->nr_events];
            c->head = (c->head + (unsigned)take) % c->nr_events;
            c->count -= (unsigned)take;
            wait_queue_wake_all(&c->writers, 0, PROC_WAKE_EVENT);
            spin_unlock(&c->lock);
            aio_ctx_put(c);
            return take;
        }
        if (min_nr <= 0) {
            spin_unlock(&c->lock);
            aio_ctx_put(c);
            return 0;
        }
        if (deadline != 0 && now >= deadline) {
            /* Linux io_getevents returns the number of events read, which is
             * 0 when the timeout expires before min_nr events arrive. */
            spin_unlock(&c->lock);
            aio_ctx_put(c);
            return 0;
        }
        if (deadline == 0) {
            spin_unlock(&c->lock);
            proc_wait_token_t token =
                proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
            if (!token.task) {
                aio_ctx_put(c);
                return -EAGAIN;
            }
            wait_queue_entry_t entry = {0};
            spin_lock(&c->lock);
            bool linked = wait_queue_link(&c->readers, &entry, token, 0);
            spin_unlock(&c->lock);
            proc_wake_reason_t reason;
            if (linked)
                reason = proc_park_commit(token);
            else {
                (void)proc_park_cancel(token);
                reason = PROC_WAKE_CANCEL;
            }
            wait_queue_unlink(&c->readers, &entry);
            proc_park_finish(token);
            if (proc_wake_reason_is_task_interrupt(reason)) {
                aio_ctx_put(c);
                return -EINTR;
            }
            continue;
        }
        /* Timed wait. */
        spin_unlock(&c->lock);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
        if (!token.task) {
            aio_ctx_put(c);
            return -EAGAIN;
        }
        wait_queue_entry_t entry = {0};
        spin_lock(&c->lock);
        if (c->count > 0) {
            spin_unlock(&c->lock);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            continue;
        }
        bool linked = wait_queue_link(&c->readers, &entry, token, 0);
        spin_unlock(&c->lock);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&c->readers, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason)) {
            aio_ctx_put(c);
            return -EINTR;
        }
        now = timer_get_ticks();
    }
}

long aio_context_getevents_nowait(struct mm_struct *mm, uint64_t ctx, long nr,
                                  aio_event_kern_t *events)
{
    if (!mm || !events || nr <= 0)
        return -EINVAL;
    aio_ctx_t *c = aio_ctx_get(mm, ctx);
    if (!c)
        return -EINVAL;

    spin_lock(&c->lock);
    if (c->count == 0) {
        spin_unlock(&c->lock);
        aio_ctx_put(c);
        return 0;
    }
    long take = (long)c->count < nr ? (long)c->count : nr;
    for (long i = 0; i < take; i++)
        events[i] = c->events[(c->head + (unsigned)i) % c->nr_events];
    c->head = (c->head + (unsigned)take) % c->nr_events;
    c->count -= (unsigned)take;
    wait_queue_wake_all(&c->writers, 0, PROC_WAKE_EVENT);
    spin_unlock(&c->lock);
    aio_ctx_put(c);
    return take;
}

int aio_context_cancel(struct mm_struct *mm, uint64_t ctx, uint64_t obj,
                       aio_event_kern_t *result)
{
    if (!mm || !result)
        return -EINVAL;
    aio_ctx_t *c = aio_ctx_get(mm, ctx);
    if (!c)
        return -EINVAL;

    spin_lock(&c->lock);
    /* With synchronous execution a submitted iocb is either still in the
     * pending set (not yet executed) or already completed in the ring.  We
     * cannot abort an executing iocb, so report it as not-cancellable. */
    for (unsigned i = 0; i < c->pending_count; i++) {
        if (c->pending[i] != obj)
            continue;
        spin_unlock(&c->lock);
        aio_ctx_put(c);
        return -EAGAIN; /* already being executed; cannot cancel */
    }
    /* Search completed events for the iocb and return its result. */
    for (unsigned i = 0; i < c->count; i++) {
        aio_event_kern_t *ev = &c->events[(c->head + i) % c->nr_events];
        if (ev->obj == obj) {
            *result = *ev;
            spin_unlock(&c->lock);
            aio_ctx_put(c);
            return 0;
        }
    }
    spin_unlock(&c->lock);
    aio_ctx_put(c);
    return -EAGAIN;
}

void aio_context_reap_mm(struct mm_struct *mm)
{
    if (!mm)
        return;
    /* The owning mm is being destroyed, so no task can still be inside a
     * get/put on these contexts; free unconditionally. */
    aio_event_kern_t *rings[AIO_MAX_CONTEXTS];
    int nrings = 0;
    unsigned long flags = spin_lock_irqsave(&g_aio_table_lock);
    for (int i = 0; i < AIO_MAX_CONTEXTS; i++) {
        aio_ctx_t *c = &g_aio[i];
        if (c->used && c->mm == mm) {
            spin_lock(&c->lock);
            aio_ctx_drop_all_pending_locked(c);
            spin_unlock(&c->lock);
            rings[nrings++] = aio_ctx_release_locked(c);
        }
    }
    spin_unlock_irqrestore(&g_aio_table_lock, flags);
    for (int i = 0; i < nrings; i++)
        kfree(rings[i]);
}
