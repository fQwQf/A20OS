#include "fs/pipe.h"

#include "core/consts.h"
#include "core/string.h"
#include "core/sync.h"
#include "fs/file.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/signal.h"

#define PIPE_DEFAULT_SIZE (256 * PIPE_BUF_SIZE)

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
    wait_queue_wake_all(&pb->read_waiters);
}

static void pipe_wake_writers(pipe_buf_t *pb)
{
    wait_queue_wake_all(&pb->write_waiters);
}

static int pipe_wait_interruptible(pipe_buf_t *pb, wait_queue_t *wq)
{
    task_t *t = proc_current();
    if (!t) {
        proc_yield();
        return 0;
    }
    if (signal_task_has_unblocked(t))
        return -ERESTARTSYS;

    wait_queue_sleep(wq);

    if (signal_task_has_unblocked(t))
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
        spin_unlock(&pb->lock);
        int wr = pipe_wait_interruptible(pb, &pb->read_waiters);
        if (wr < 0) return wr;
        spin_lock(&pb->lock);
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
    int was_full = (pb->used + n == pb->capacity);
    spin_unlock(&pb->lock);

    if (was_full)
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
                spin_unlock(&pb->lock);
                int wr = pipe_wait_interruptible(pb, &pb->write_waiters);
                if (wr < 0) return n ? (int)n : wr;
                spin_lock(&pb->lock);
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
                spin_unlock(&pb->lock);
                int wr = pipe_wait_interruptible(pb, &pb->write_waiters);
                if (wr < 0) return n ? (int)n : wr;
                spin_lock(&pb->lock);
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

    char *new_data = (char *)kmalloc(new_capacity);
    if (!new_data) {
        spin_unlock(&pb->lock);
        return -ENOMEM;
    }
    for (size_t i = 0; i < pb->used; i++)
        new_data[i] = pb->data[(pb->tail + i) % pb->capacity];
    kfree(pb->data);
    pb->data = new_data;
    pb->capacity = new_capacity;
    pb->logical_size = new_capacity;
    pb->tail = 0;
    pb->head = pb->used % pb->capacity;
    spin_unlock(&pb->lock);
    return (int)new_capacity;
}

static int pipe_read_close(vfile_t *vf)
{
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (pb) {
        spin_lock(&pb->lock);
        int last_reader = refcount_read(&vf->ref_count) == 0;
        if (last_reader) {
            pb->reader_closed = 1;
            spin_unlock(&pb->lock);
            pipe_wake_writers(pb);
        } else {
            spin_unlock(&pb->lock);
        }
        pb->ref--;
        if (!pb->ref) {
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
        int last_writer = refcount_read(&vf->ref_count) == 0;
        if (last_writer) {
            pb->writer_closed = 1;
            spin_unlock(&pb->lock);
            pipe_wake_readers(pb);
        } else {
            spin_unlock(&pb->lock);
        }
        pb->ref--;
        if (!pb->ref) {
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
    task_t *cur = proc_current();
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

    memset(rd, 0, sizeof(*rd)); rd->ops = &g_pipe_read_ops;  rd->priv = pb; rd->flags = O_RDONLY; refcount_set(&rd->ref_count, 1);
    memset(wr, 0, sizeof(*wr)); wr->ops = &g_pipe_write_ops; wr->priv = pb; wr->flags = O_WRONLY; refcount_set(&wr->ref_count, 1);

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
