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
    vnode_t        *fifo_inode;     /* pinned while any open description exists */
    struct pipe_buf *fifo_next;     /* protected by fifo_registry_lock */
    unsigned        readers, writers;
    uint64_t        reader_generation, writer_generation;
    wait_queue_t    open_waiters;
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

static mutex_t fifo_registry_lock = MUTEX_INIT;
static pipe_buf_t *fifo_registry;
static vfile_ops_t g_fifo_ops;
static vfile_ops_t g_fifo_path_ops;

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

/* The closing description owns its pipe reference until after notifications.
 * Concurrent closes cannot free the ring while a peer publishes its wakeup. */
static int pipe_close(vfile_t *vf)
{
    pipe_buf_t *pb = vf->priv;
    if (!pb) return 0;
    int access = vf->flags & O_ACCMODE;
    spin_lock(&pb->lock);
    if (access != O_WRONLY) --pb->readers;
    if (access != O_RDONLY) --pb->writers;
    pb->reader_closed = !pb->readers;
    pb->writer_closed = !pb->writers;
    spin_unlock(&pb->lock);
    pipe_wake_readers(pb, 1);
    pipe_wake_writers(pb, 1);
    if (pb->fifo_inode) mutex_lock(&fifo_registry_lock);
    spin_lock(&pb->lock);
    int refs = --pb->ref;
    spin_unlock(&pb->lock);
    vnode_t *anchor = pb->fifo_inode;
    if (anchor) {
        if (!refs) {
            pipe_buf_t **link = &fifo_registry;
            while (*link != pb) link = &(*link)->fifo_next;
            *link = pb->fifo_next;
        }
        mutex_unlock(&fifo_registry_lock);
    }
    if (!refs) {
        vnode_put(anchor);
        kfree(pb->data);
        kfree(pb);
    }
    return 0;
}

static vfile_ops_t g_pipe_read_ops;
static vfile_ops_t g_pipe_write_ops;

static size_t pipe_poll_sources(vfile_t *vf, short events,
                                readiness_source_t *sources, size_t max)
{
    pipe_buf_t *pb = vf ? vf->priv : NULL;
    if (!pb || !sources || !max) return 0;
    size_t n = 0;
    int access = vf->flags & O_ACCMODE;
    spin_lock(&pb->lock);
    if (access != O_WRONLY && ((events & POLLIN) || access == O_RDONLY)) {
        pb->polled_readers = 1;
        sources[n++] = (readiness_source_t){ .queue = &pb->read_waiters };
    }
    if (access != O_RDONLY && n < max) {
        pb->polled_writers = 1;
        sources[n++] = (readiness_source_t){ .queue = &pb->write_waiters };
    }
    spin_unlock(&pb->lock);
    return n;
}

static vfile_ops_t g_pipe_read_ops  = {
    .read = pipe_read, .write = pipe_null_write, .poll = pipe_poll_events,
    .poll_sources = pipe_poll_sources, .close = pipe_close
};
static vfile_ops_t g_pipe_write_ops = {
    .read = pipe_null_read, .write = pipe_write, .poll = pipe_poll_events,
    .poll_sources = pipe_poll_sources, .close = pipe_close
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
    return vf && (vf->ops == &g_pipe_read_ops || vf->ops == &g_pipe_write_ops || vf->ops == &g_fifo_ops);
}

int pipe_poll_events(vfile_t *vf, short events)
{
    if (!pipe_vfile_is(vf)) return POLLNVAL;
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return POLLNVAL;
    short revents = 0;
    spin_lock(&pb->lock);
    int access = vf->flags & O_ACCMODE;
    if (access != O_WRONLY) {
        if ((events & POLLIN) && pb->used > 0)
            revents |= POLLIN;
        if (pb->writer_closed && (!pb->fifo_inode ||
            pb->writer_generation != vf->offset))
            revents |= POLLHUP;
    }
    if (access != O_RDONLY) {
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
    pb->readers = pb->writers = 1;
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

/* Filesystems persist only S_IFIFO and directory metadata. All live FIFO
 * contents and endpoints use the same bounded ring/atomic writes as pipe(2).
 * Keys are mount+inode, so hard links and distinct vnode lookups communicate;
 * a pinned inode prevents reuse until the last open description is closed. */
static vfile_ops_t g_fifo_ops = {
    .read = pipe_read, .write = pipe_write, .poll = pipe_poll_events,
    .poll_sources = pipe_poll_sources, .close = pipe_close,
};

vfile_t *fifo_open_vnode(vnode_t *vn, int flags)
{
    task_t *task = proc_current();
    int error = -ENOMEM;
    vfile_t *vf = vfile_alloc();
    if (!vf) goto fail;
    if (flags & O_PATH) {
        vf->ops = &g_fifo_path_ops;
        vf->flags = flags;
        vf->vnode = vn;
        vnode_get(vn);
        vfile_ref_init(vf, 1);
        return vf;
    }
    int access = flags & O_ACCMODE;
    mutex_lock(&fifo_registry_lock);
    pipe_buf_t *pb;
    for (pb = fifo_registry; pb; pb = pb->fifo_next)
        if (pb->fifo_inode->mnt == vn->mnt && pb->fifo_inode->ino == vn->ino)
            break;
    if (!pb) {
        pb = kmalloc(sizeof(*pb));
        if (!pb) goto fail_locked;
        memset(pb, 0, sizeof(*pb));
        pb->data = kmalloc(PIPE_DEFAULT_SIZE);
        if (!pb->data) { kfree(pb); goto fail_locked; }
        spin_init(&pb->lock);
        wait_queue_init(&pb->read_waiters);
        wait_queue_init(&pb->write_waiters);
        wait_queue_init(&pb->open_waiters);
        pb->capacity = pb->logical_size = PIPE_DEFAULT_SIZE;
        pb->read_fd = pb->write_fd = -1;
        pb->reader_closed = pb->writer_closed = 1;
        pb->fifo_inode = vn;
        vnode_get(vn);
        pb->fifo_next = fifo_registry;
        fifo_registry = pb;
    }
    spin_lock(&pb->lock);
    if (access == O_WRONLY && (flags & O_NONBLOCK) && !pb->readers) {
        spin_unlock(&pb->lock);
        if (!pb->ref) {
            fifo_registry = pb->fifo_next; /* new, empty entry is always head */
            vnode_put(pb->fifo_inode);
            kfree(pb->data);
            kfree(pb);
        }
        error = -ENXIO;
        goto fail_locked;
    }
    ++pb->ref;
    /* Save the peer generation before publishing this endpoint. A peer
     * opening and closing before finish_open still satisfies rendezvous. */
    vf->offset = access == O_WRONLY ?
        pb->reader_generation - (pb->readers != 0) :
        pb->writer_generation - (pb->writers != 0);
    if (access != O_WRONLY) { ++pb->readers; ++pb->reader_generation; }
    if (access != O_RDONLY) { ++pb->writers; ++pb->writer_generation; }
    pb->reader_closed = !pb->readers;
    pb->writer_closed = !pb->writers;
    spin_unlock(&pb->lock);
    mutex_unlock(&fifo_registry_lock);
    vf->ops = &g_fifo_ops;
    vf->priv = pb;
    vf->flags = flags;
    vf->vnode = vn;
    vnode_get(vn);
    vfile_ref_init(vf, 1);
    wait_queue_wake_all(&pb->open_waiters, 0, PROC_WAKE_EVENT);
    pipe_wake_readers(pb, 1);
    pipe_wake_writers(pb, 1);
    return vf;
fail_locked:
    mutex_unlock(&fifo_registry_lock);
    vfile_free(vf);
fail:
    if (task) task->vfs_open_errno = error;
    return NULL;
}

int fifo_finish_open(vfile_t *vf)
{
    if (!vf || vf->ops != &g_fifo_ops || (vf->flags & O_NONBLOCK)) return 0;
    int access = vf->flags & O_ACCMODE;
    if (access == O_RDWR) return 0;
    pipe_buf_t *pb = vf->priv;
    task_t *task = proc_current();
    for (;;) {
        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
        spin_lock(&pb->lock);
        bool ready = access == O_RDONLY ?
            (pb->writers || pb->writer_generation != vf->offset) :
            (pb->readers || pb->reader_generation != vf->offset);
        bool interrupted = task && signal_task_has_unblocked(task);
        wait_queue_entry_t entry = {0};
        bool linked = !ready && !interrupted && token.task &&
            wait_queue_link(&pb->open_waiters, &entry, token, 0);
        spin_unlock(&pb->lock);
        proc_wake_reason_t reason = PROC_WAKE_CANCEL;
        if (linked) reason = proc_park_commit(token);
        else (void)proc_park_cancel(token);
        wait_queue_unlink(&pb->open_waiters, &entry);
        proc_park_finish(token);
        if (ready) return 0;
        if (interrupted || proc_wake_reason_is_task_interrupt(reason)) return -ERESTARTSYS;
        if (!token.task) proc_yield();
    }
}
