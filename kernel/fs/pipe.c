#include "fs/pipe.h"

#include "core/consts.h"
#include "core/string.h"
#include "core/sync.h"
#include "fs/file.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/signal.h"

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
    wait_queue_t    read_waiters;
    wait_queue_t    write_waiters;
} pipe_buf_t;

static void pipe_wake_readers(pipe_buf_t *pb)
{
    wait_queue_wake_all(&pb->read_waiters, 0, PROC_WAKE_EVENT);
}

static void pipe_wake_writers(pipe_buf_t *pb)
{
    wait_queue_wake_all(&pb->write_waiters, 0, PROC_WAKE_EVENT);
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

    if (reason == PROC_WAKE_SIGNAL || signal_task_has_unblocked(t))
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
    pb->tail = (pb->tail + n) % pb->capacity;
    pb->used -= n;
    spin_unlock(&pb->lock);

    pipe_wake_writers(pb);
    return (int)n;
}

static int pipe_write(vfile_t *vf, const char *buf, size_t count)
{
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return -EBADF;
    if (count == 0) return 0;

    spin_lock(&pb->lock);
    if (pb->reader_closed) {
        spin_unlock(&pb->lock);
        task_t *t = proc_current();
        if (t)
            signal_send(t->pid, SIGPIPE);
        return -EPIPE;
    }
    size_t n = 0;
    while (n < count) {
        size_t remaining = count - n;
        size_t space = pb->capacity - pb->used;
        if (remaining <= PIPE_BUF_SIZE) {
            while (space < remaining) {
                if (pb->reader_closed) {
                    spin_unlock(&pb->lock);
                    if (n > 0) return (int)n;
                    task_t *t = proc_current();
                    if (t)
                        signal_send(t->pid, SIGPIPE);
                    return -EPIPE;
                }
                if (vf->flags & O_NONBLOCK) {
                    spin_unlock(&pb->lock);
                    return n ? (int)n : -EAGAIN;
                }
                int wr = pipe_wait_interruptible_locked(
                    pb, &pb->write_waiters, remaining);
                if (wr < 0) {
                    spin_unlock(&pb->lock);
                    return n ? (int)n : wr;
                }
                space = pb->capacity - pb->used;
            }
            size_t chunk = remaining;
            size_t first = pb->capacity - pb->head;
            if (first > chunk)
                first = chunk;
            memcpy(pb->data + pb->head, buf + n, first);
            size_t second = chunk - first;
            if (second)
                memcpy(pb->data, buf + n + first, second);
            pb->head = (pb->head + chunk) % pb->capacity;
            pb->used += chunk;
            n += chunk;
            spin_unlock(&pb->lock);
            pipe_wake_readers(pb);
            spin_lock(&pb->lock);
        } else {
            if (space == 0) {
                if (pb->reader_closed) {
                    spin_unlock(&pb->lock);
                    if (n > 0) return (int)n;
                    task_t *t = proc_current();
                    if (t)
                        signal_send(t->pid, SIGPIPE);
                    return -EPIPE;
                }
                if (vf->flags & O_NONBLOCK) {
                    spin_unlock(&pb->lock);
                    return n ? (int)n : -EAGAIN;
                }
                int wr = pipe_wait_interruptible_locked(
                    pb, &pb->write_waiters, 1);
                if (wr < 0) {
                    spin_unlock(&pb->lock);
                    return n ? (int)n : wr;
                }
                continue;
            }
            size_t chunk = remaining < space ? remaining : space;
            size_t first = pb->capacity - pb->head;
            if (first > chunk)
                first = chunk;
            memcpy(pb->data + pb->head, buf + n, first);
            size_t second = chunk - first;
            if (second)
                memcpy(pb->data, buf + n + first, second);
            pb->head = (pb->head + chunk) % pb->capacity;
            pb->used += chunk;
            n += chunk;
            spin_unlock(&pb->lock);
            pipe_wake_readers(pb);
            spin_lock(&pb->lock);
        }
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

    for (size_t i = 0; i < pb->used; i++)
        new_data[i] = pb->data[(pb->tail + i) % pb->capacity];
    char *old_data = pb->data;
    pb->data = new_data;
    pb->capacity = new_capacity;
    pb->logical_size = new_capacity;
    pb->tail = 0;
    pb->head = pb->used % pb->capacity;
    spin_unlock(&pb->lock);

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
            pipe_wake_writers(pb);
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
            pipe_wake_readers(pb);
        if (!refs) {
            if (pb->data) kfree(pb->data);
            kfree(pb);
        }
    }
    return 0;
}

static vfile_ops_t g_pipe_read_ops  = { .read = pipe_read,       .write = pipe_null_write, .close = pipe_read_close  };
static vfile_ops_t g_pipe_write_ops = { .read = pipe_null_read,  .write = pipe_write,      .close = pipe_write_close };

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
        if ((events & POLLIN) && (pb->used > 0 || pb->writer_closed))
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

int pipe_create(int pipefd[2])
{
    pipe_buf_t *pb = (pipe_buf_t *)kmalloc(sizeof(pipe_buf_t));
    if (!pb) return -ENOMEM;
    memset(pb, 0, sizeof(*pb));
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

    memset(rd, 0, sizeof(*rd)); rd->ops = &g_pipe_read_ops;  rd->priv = pb; rd->flags = O_RDONLY; vfile_ref_init(rd, 1);
    memset(wr, 0, sizeof(*wr)); wr->ops = &g_pipe_write_ops; wr->priv = pb; wr->flags = O_WRONLY; vfile_ref_init(wr, 1);

    int fdrd = vfs_alloc_fd(rd);
    int fdwr = vfs_alloc_fd(wr);
    if (fdrd < 0 || fdwr < 0) {
        if (fdrd >= 0) vfs_close(fdrd);
        else vfile_free(rd);
        if (fdwr >= 0) vfs_close(fdwr);
        else vfile_free(wr);
        if (pb->ref > 0) {
            if (pb->data) kfree(pb->data);
            kfree(pb);
        }
        return -EMFILE;
    }
    pipefd[0] = fdrd;
    pipefd[1] = fdwr;
    return 0;
}
