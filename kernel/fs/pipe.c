#include "fs/pipe.h"

#include "core/consts.h"
#include "core/string.h"
#include "core/sync.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "fs/readiness.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "ipc/ipc.h"
#include "ipc/envelope.h"

#define PIPE_DEFAULT_SIZE (16 * PIPE_BUF_SIZE)

typedef struct pipe_buf {
    spinlock_t      lock;
    char           *data;
    size_t          capacity;
    size_t          head, tail, used;
    size_t          logical_size;
    int             writer_closed;
    int             reader_closed;
    int             ref;
    int             read_fd;        /* global fd of the read endpoint */
    int             write_fd;       /* global fd of the write endpoint */
    /* Protected by lock. Poll-source flags are sticky: once a readiness
     * subscriber has seen a queue we keep publishing every notification.
     * Direct blocking waiters are counted across link/commit/unlink. */
    unsigned        waiting_readers, waiting_writers;
    size_t          write_wait_min;
    unsigned        polled_readers, polled_writers;
    wait_queue_t    read_waiters;
    wait_queue_t    write_waiters;
} pipe_buf_t;

static void pipe_wake_readers(pipe_buf_t *pb, int queued)
{
    if (queued) wait_queue_wake_all(&pb->read_waiters, 0, PROC_WAKE_EVENT);
    if (pb->read_fd >= 0)
        a20_event_notify((void *)(uintptr_t)pb->read_fd,
                         A20_OBJ_PIPE_ENDPOINT, A20_EVENT_READABLE, 0, 0);
}

static void pipe_wake_writers(pipe_buf_t *pb, int queued)
{
    if (queued) wait_queue_wake_all(&pb->write_waiters, 0, PROC_WAKE_EVENT);
    if (pb->write_fd >= 0)
        a20_event_notify((void *)(uintptr_t)pb->write_fd,
                         A20_OBJ_PIPE_ENDPOINT, A20_EVENT_WRITABLE, 0, 0);
}

static int pipe_wait_interruptible_locked(pipe_buf_t *pb, wait_queue_t *wq,
                                          size_t needed)
{
    task_t *t = proc_current();
    if (!t) {
        spin_unlock(&pb->lock);
        proc_yield();
        spin_lock(&pb->lock);
        return 0;
    }
    if (signal_task_has_unblocked(t))
        return -ERESTARTSYS;

    spin_unlock(&pb->lock);
    proc_wait_token_t token =
        proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
    spin_lock(&pb->lock);
    if (!token.task)
        return 0;

    int should_wait =
        (wq == &pb->read_waiters) ?
            (pb->used == 0 && !pb->writer_closed) :
            (pb->capacity - pb->used < needed && !pb->reader_closed);
    int interrupted = signal_task_has_unblocked(t);
    if (!should_wait || interrupted) {
        spin_unlock(&pb->lock);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
        spin_lock(&pb->lock);
        return interrupted ? -ERESTARTSYS : 0;
    }

    wait_queue_entry_t entry = {0};
    unsigned *waiting = wq == &pb->read_waiters ?
                        &pb->waiting_readers : &pb->waiting_writers;
    if (wq == &pb->write_waiters &&
        (!*waiting || needed < pb->write_wait_min))
        pb->write_wait_min = needed;
    ++*waiting;
    bool linked = wait_queue_link(wq, &entry, token, 0);

    spin_unlock(&pb->lock);
    proc_wake_reason_t reason;
    if (linked)
        reason = proc_park_commit(token);
    else {
        (void)proc_park_cancel(token);
        reason = PROC_WAKE_CANCEL;
    }
    wait_queue_unlink(wq, &entry);
    proc_park_finish(token);
    spin_lock(&pb->lock);
    --*waiting;

    if (proc_wake_reason_is_task_interrupt(reason) ||
        signal_task_has_unblocked(t))
        return -ERESTARTSYS;
    return 0;
}

static int pipe_read(vfile_t *vf, char *buf, size_t count)
{
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return -EBADF;
    if (count == 0) return 0;

    spin_lock(&pb->lock);
    while (pb->used == 0) {
        if (pb->writer_closed) {
            spin_unlock(&pb->lock);
            return 0;
        }
        if (vf->flags & O_NONBLOCK) {
            spin_unlock(&pb->lock);
            return -EAGAIN;
        }
        int wr = pipe_wait_interruptible_locked(pb, &pb->read_waiters, 1);
        if (wr < 0) {
            spin_unlock(&pb->lock);
            return wr;
        }
    }
    size_t n = pb->used < count ? pb->used : count;
    size_t first = pb->capacity - pb->tail;
    if (first > n)
        first = n;
    memcpy(buf, pb->data + pb->tail, first);
    size_t second = n - first;
    if (second)
        memcpy(buf + first, pb->data, second);
    pb->tail += n;
    if (pb->tail >= pb->capacity) pb->tail -= pb->capacity;
    pb->used -= n;
    /* An atomic writer cannot proceed until its whole record fits. Avoid
     * waking it after every smaller read only to have it immediately park.
     * The minimum is conservative while multiple writers are waiting; it is
     * reset when the next first writer registers under this same lock. */
    int queued = pb->polled_writers ||
        (pb->waiting_writers && pb->capacity - pb->used >= pb->write_wait_min);
    spin_unlock(&pb->lock);

    pipe_wake_writers(pb, queued);
    return (int)n;
}

static int pipe_write(vfile_t *vf, const char *buf, size_t count)
{
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return -EBADF;
    if (!count) return 0;
    size_t n = 0;
    /* Atomicity applies to the original write, not a large write's tail. */
    size_t needed = count <= PIPE_BUF_SIZE ? count : 1;
    spin_lock(&pb->lock);
    while (n < count) {
        /* Recheck even when space is available: a reader can close while
         * this writer drops the lock to publish a wakeup. */
        if (pb->reader_closed) {
            spin_unlock(&pb->lock);
            task_t *t = proc_current();
            if (t && t->abi_mode == 0)
                signal_send_thread(t->pid, SIGPIPE);
            return n ? (int)n : -EPIPE;
        }
        size_t space = pb->capacity - pb->used;
        if (space < needed) {
            if (vf->flags & O_NONBLOCK) {
                spin_unlock(&pb->lock);
                return n ? (int)n : -EAGAIN;
            }
            int wr = pipe_wait_interruptible_locked(pb, &pb->write_waiters, needed);
            if (wr < 0) {
                spin_unlock(&pb->lock);
                return n ? (int)n : wr;
            }
            continue;
        }
        size_t chunk = count - n;
        if (chunk > space) chunk = space;
        size_t first = pb->capacity - pb->head;
        if (first > chunk) first = chunk;
        memcpy(pb->data + pb->head, buf + n, first);
        if (chunk > first) memcpy(pb->data, buf + n + first, chunk - first);
        pb->head += chunk;
        if (pb->head >= pb->capacity) pb->head -= pb->capacity;
        pb->used += chunk;
        n += chunk;
        int queued = pb->waiting_readers || pb->polled_readers;
        spin_unlock(&pb->lock);
        pipe_wake_readers(pb, queued);
        /* A completed write needs no further ownership of the ring. */
        if (n == count) return (int)n;
        spin_lock(&pb->lock);
    }
    spin_unlock(&pb->lock);
    return (int)n;
}

static int pipe_null_read(vfile_t *vf, char *buf, size_t count)
{
    (void)vf; (void)buf; (void)count;
    return 0;
}

static int pipe_null_write(vfile_t *vf, const char *buf, size_t count)
{
    (void)vf; (void)buf;
    return (int)count;
}

static int pipe_resize(pipe_buf_t *pb, size_t new_capacity)
{
    if (!pb) return -EINVAL;
    if (new_capacity < PIPE_BUF_SIZE)
        new_capacity = PIPE_BUF_SIZE;

    spin_lock(&pb->lock);
    if (new_capacity < pb->used) {
        spin_unlock(&pb->lock);
        return -EBUSY;
    }
    if (new_capacity == pb->capacity) {
        pb->logical_size = new_capacity;
        spin_unlock(&pb->lock);
        return (int)new_capacity;
    }
    spin_unlock(&pb->lock);

    char *new_data = (char *)kmalloc(new_capacity);
    if (!new_data)
        return -ENOMEM;

    spin_lock(&pb->lock);
    if (new_capacity < pb->used) {
        spin_unlock(&pb->lock);
        kfree(new_data);
        return -EBUSY;
    }
    if (new_capacity == pb->capacity) {
        pb->logical_size = new_capacity;
        spin_unlock(&pb->lock);
        kfree(new_data);
        return (int)new_capacity;
    }

    size_t first = pb->capacity - pb->tail;
    if (first > pb->used) first = pb->used;
    memcpy(new_data, pb->data + pb->tail, first);
    if (pb->used > first)
        memcpy(new_data + first, pb->data, pb->used - first);
    char *old_data = pb->data;
    pb->data = new_data;
    pb->capacity = new_capacity;
    pb->logical_size = new_capacity;
    pb->tail = 0;
    pb->head = pb->used == pb->capacity ? 0 : pb->used;
    int queued = pb->waiting_writers || pb->polled_writers;
    spin_unlock(&pb->lock);

    /* Capacity changes are readiness changes too. A full-pipe writer must
     * not remain asleep after F_SETPIPE_SZ creates free space. */
    pipe_wake_writers(pb, queued);
    kfree(old_data);
    return (int)new_capacity;
}

static int pipe_read_close(vfile_t *vf)
{
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (pb) {
        spin_lock(&pb->lock);
        int last_reader = vfile_ref_read(vf) == 0;
        if (last_reader)
            pb->reader_closed = 1;
        int refs = --pb->ref;
        spin_unlock(&pb->lock);

        if (last_reader)
            pipe_wake_writers(pb, 1);
        if (!refs) {
            if (pb->data) kfree(pb->data);
            kfree(pb);
        }
    }
    return 0;
}

static int pipe_write_close(vfile_t *vf)
{
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (pb) {
        spin_lock(&pb->lock);
        int last_writer = vfile_ref_read(vf) == 0;
        if (last_writer)
            pb->writer_closed = 1;
        int refs = --pb->ref;
        spin_unlock(&pb->lock);

        if (last_writer)
            pipe_wake_readers(pb, 1);
        if (!refs) {
            if (pb->data) kfree(pb->data);
            kfree(pb);
        }
    }
    return 0;
}

static vfile_ops_t g_pipe_read_ops;
static vfile_ops_t g_pipe_write_ops;

static size_t pipe_poll_sources(vfile_t *vf, short events,
                                readiness_source_t *sources, size_t max)
{
    (void)events;
    pipe_buf_t *pb = vf ? vf->priv : NULL;
    if (!pb || !sources || max == 0)
        return 0;
    spin_lock(&pb->lock);
    if (vf->ops == &g_pipe_read_ops) {
        pb->polled_readers = 1;
        sources[0].queue = &pb->read_waiters;
    } else {
        pb->polled_writers = 1;
        sources[0].queue = &pb->write_waiters;
    }
    spin_unlock(&pb->lock);
    sources[0].key = 0;
    sources[0].deadline = 0;
    return 1;
}

static vfile_ops_t g_pipe_read_ops  = {
    .read = pipe_read, .write = pipe_null_write, .poll = pipe_poll_events,
    .poll_sources = pipe_poll_sources, .close = pipe_read_close
};
static vfile_ops_t g_pipe_write_ops = {
    .read = pipe_null_read, .write = pipe_write, .poll = pipe_poll_events,
    .poll_sources = pipe_poll_sources, .close = pipe_write_close
};

/* Peek up to count bytes from a pipe without consuming them.  Blocks while
 * the pipe is empty and the writer is open, unless the read end is
 * O_NONBLOCK.  Returns the number of bytes copied, 0 when the writer has
 * closed, or a negative error.  Used by tee(2). */
int pipe_peek(vfile_t *vf, char *buf, size_t count)
{
    pipe_buf_t *pb = vf ? (pipe_buf_t *)vf->priv : NULL;
    if (!pb || !pipe_vfile_is(vf))
        return -EINVAL;
    if (count == 0)
        return 0;

    spin_lock(&pb->lock);
    while (pb->used == 0) {
        if (pb->writer_closed) {
            spin_unlock(&pb->lock);
            return 0;
        }
        if (vf->flags & O_NONBLOCK) {
            spin_unlock(&pb->lock);
            return -EAGAIN;
        }
        int wr = pipe_wait_interruptible_locked(pb, &pb->read_waiters, 1);
        if (wr < 0) {
            spin_unlock(&pb->lock);
            return wr;
        }
    }
    size_t n = pb->used < count ? pb->used : count;
    size_t first = pb->capacity - pb->tail;
    if (first > n)
        first = n;
    memcpy(buf, pb->data + pb->tail, first);
    size_t second = n - first;
    if (second)
        memcpy(buf + first, pb->data, second);
    spin_unlock(&pb->lock);
    return (int)n;
}

int pipe_vfile_is(vfile_t *vf)
{
    return vf && (vf->ops == &g_pipe_read_ops || vf->ops == &g_pipe_write_ops);
}

int pipe_poll_events(vfile_t *vf, short events)
{
    if (!pipe_vfile_is(vf)) return POLLNVAL;
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return POLLNVAL;
    short revents = 0;
    spin_lock(&pb->lock);
    if (vf->ops == &g_pipe_read_ops) {
        if ((events & POLLIN) && pb->used > 0)
            revents |= POLLIN;
        if (pb->writer_closed)
            revents |= POLLHUP;
    } else {
        if ((events & POLLOUT) && pb->used < pb->capacity && !pb->reader_closed)
            revents |= POLLOUT;
        if (pb->reader_closed)
            revents |= POLLERR;
    }
    spin_unlock(&pb->lock);
    return revents;
}

int pipe_get_available(vfile_t *vf)
{
    if (!pipe_vfile_is(vf)) return -EINVAL;
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return -EINVAL;
    spin_lock(&pb->lock);
    int available = (int)pb->used;
    spin_unlock(&pb->lock);
    return available;
}

int pipe_get_size(vfile_t *vf)
{
    if (!pipe_vfile_is(vf)) return -EINVAL;
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return -EINVAL;
    spin_lock(&pb->lock);
    int sz = pb->logical_size ? (int)pb->logical_size : PIPE_BUF_SIZE;
    spin_unlock(&pb->lock);
    return sz;
}

int pipe_set_size(vfile_t *vf, size_t size)
{
    if (!pipe_vfile_is(vf)) return -EINVAL;
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return -EINVAL;
    return pipe_resize(pb, size);
}

int pipe_create_vfiles(vfile_t **read_end, vfile_t **write_end, int flags)
{
    pipe_buf_t *pb = (pipe_buf_t *)kmalloc(sizeof(pipe_buf_t));
    if (!pb) return -ENOMEM;
    memset(pb, 0, sizeof(*pb));
    pb->read_fd = pb->write_fd = -1;
    spin_init(&pb->lock);
    wait_queue_init(&pb->read_waiters);
    wait_queue_init(&pb->write_waiters);
    pb->ref = 2;
    pb->logical_size = PIPE_DEFAULT_SIZE;
    pb->capacity = pb->logical_size;
    pb->data = (char *)kmalloc(pb->capacity);
    if (!pb->data) { kfree(pb); return -ENOMEM; }

    vfile_t *rd = vfile_alloc();
    vfile_t *wr = vfile_alloc();
    if (!rd || !wr) {
        kfree(pb->data);
        kfree(pb);
        if (rd) vfile_free(rd);
        if (wr) vfile_free(wr);
        return -ENOMEM;
    }

    /* vfile_alloc() initialized the object and assigned its stable identity;
     * do not memset it here or aliases lose that identity. */
    rd->ops = &g_pipe_read_ops; rd->priv = pb; rd->flags = O_RDONLY | flags;
    vfile_ref_init(rd, 1);
    wr->ops = &g_pipe_write_ops; wr->priv = pb; wr->flags = O_WRONLY | flags;
    vfile_ref_init(wr, 1);
    if (env_kind_tag_vfile(rd, A20_OBJ_PIPE_ENDPOINT) < 0 ||
        env_kind_tag_vfile(wr, A20_OBJ_PIPE_ENDPOINT) < 0) {
        vfile_free(rd);
        vfile_free(wr);
        kfree(pb->data);
        kfree(pb);
        return -EINVAL;
    }

    *read_end = rd;
    *write_end = wr;
    return 0;
}

int pipe_create(int pipefd[2])
{
    vfile_t *rd, *wr;
    int error = pipe_create_vfiles(&rd, &wr, 0);
    if (error < 0)
        return error;
    pipe_buf_t *pb = rd->priv;
    int fdrd = fdtable_install_current_vfile(rd, 0);
    int fdwr = fdtable_install_current_vfile(wr, 0);
    if (fdrd < 0 || fdwr < 0) {
        if (fdrd >= 0) fdtable_close_current(fdrd);
        else vfs_put_file(rd);
        if (fdwr >= 0) fdtable_close_current(fdwr);
        else vfs_put_file(wr);
        return -EMFILE;
    }
    pipefd[0] = fdrd;
    pipefd[1] = fdwr;
    pb->read_fd = fdrd;
    pb->write_fd = fdwr;
    return 0;
}
