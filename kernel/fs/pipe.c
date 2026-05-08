#include "fs/pipe.h"

#include "core/consts.h"
#include "core/string.h"
#include "fs/file.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/signal.h"

#define PIPE_DEFAULT_SIZE (16 * PIPE_BUF_SIZE)

typedef struct pipe_buf {
    char    *data;
    size_t   capacity;
    size_t   head, tail, used;
    size_t   logical_size;
    int      writer_closed;
    int      reader_closed;
    int      ref;
    task_t  *read_waiters;
    task_t  *write_waiters;
} pipe_buf_t;

static void pipe_wake_all(task_t **head)
{
    if (!head)
        return;
    task_t *t = *head;
    *head = NULL;
    while (t) {
        task_t *next = t->wait_next;
        t->wait_next = NULL;
        proc_set_wake_time(t, 0);
        if (t->state == PROC_BLOCKED)
            proc_make_ready(t);
        t = next;
    }
}

static void pipe_unlink_waiter(task_t **head, task_t *target)
{
    if (!head || !target)
        return;
    task_t **pp = head;
    while (*pp) {
        if (*pp == target) {
            *pp = target->wait_next;
            target->wait_next = NULL;
            return;
        }
        pp = &(*pp)->wait_next;
    }
}

static int pipe_wait_interruptible(pipe_buf_t *pb, task_t **head, const char *op)
{
    task_t *t = proc_current();
    if (!t) {
        proc_yield();
        return 0;
    }
    (void)pb;
    (void)op;
    if (signal_task_has_unblocked(t))
        return -ERESTARTSYS;
    if (head) {
        t->wait_next = *head;
        *head = t;
    }
    proc_set_wake_time(t, 0);
    t->state = PROC_BLOCKED;
    sched();
    if (head)
        pipe_unlink_waiter(head, t);
    if (t->state == PROC_BLOCKED)
        t->state = PROC_RUNNING;
    proc_set_wake_time(t, 0);
    return signal_task_has_unblocked(t) ? -ERESTARTSYS : 0;
}

static int pipe_read(vfile_t *vf, char *buf, size_t count)
{
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return -EBADF;
    if (count == 0) return 0;
    while (pb->used == 0) {
        if (pb->writer_closed) return 0;
        if (vf->flags & O_NONBLOCK) return -EAGAIN;
        int wr = pipe_wait_interruptible(pb, &pb->read_waiters, "read");
        if (wr < 0) return wr;
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
    pipe_wake_all(&pb->write_waiters);
    return (int)n;
}

static int pipe_write(vfile_t *vf, const char *buf, size_t count)
{
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (!pb) return -EBADF;
    if (count == 0) return 0;
    if (pb->reader_closed) {
        task_t *t = proc_current();
        if (t)
            signal_send(t->pid, SIGPIPE);
        return -EPIPE;
    }
    size_t n = 0;
    while (n < count) {
        while (pb->used == pb->capacity) {
            if (pb->reader_closed) {
                task_t *t = proc_current();
                if (t)
                    signal_send(t->pid, SIGPIPE);
                return n ? (int)n : -EPIPE;
            }
            if (vf->flags & O_NONBLOCK) return n ? (int)n : -EAGAIN;
            int wr = pipe_wait_interruptible(pb, &pb->write_waiters, "write");
            if (wr < 0) return n ? (int)n : wr;
        }
        size_t space = pb->capacity - pb->used;
        size_t chunk = count - n;
        if (chunk > space)
            chunk = space;
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
        pipe_wake_all(&pb->read_waiters);
    }
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
    if (new_capacity < pb->used)
        return -EBUSY;
    if (new_capacity == pb->capacity) {
        pb->logical_size = new_capacity;
        return (int)new_capacity;
    }

    char *new_data = (char *)kmalloc(new_capacity);
    if (!new_data)
        return -ENOMEM;
    for (size_t i = 0; i < pb->used; i++)
        new_data[i] = pb->data[(pb->tail + i) % pb->capacity];
    kfree(pb->data);
    pb->data = new_data;
    pb->capacity = new_capacity;
    pb->logical_size = new_capacity;
    pb->tail = 0;
    pb->head = pb->used % pb->capacity;
    return (int)new_capacity;
}

static int pipe_read_close(vfile_t *vf)
{
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    if (pb) {
        int last_reader = refcount_read(&vf->ref_count) == 0;
        if (last_reader) {
            pb->reader_closed = 1;
            pipe_wake_all(&pb->write_waiters);
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
        int last_writer = refcount_read(&vf->ref_count) == 0;
        if (last_writer) {
            pb->writer_closed = 1;
            pipe_wake_all(&pb->read_waiters);
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
    return revents;
}

int pipe_get_size(vfile_t *vf)
{
    if (!pipe_vfile_is(vf)) return -EINVAL;
    pipe_buf_t *pb = (pipe_buf_t *)vf->priv;
    return pb && pb->logical_size ? (int)pb->logical_size : PIPE_BUF_SIZE;
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
    pb->ref = 2;
    task_t *cur = proc_current();
    pb->logical_size = proc_has_cap(cur, CAP_SYS_ADMIN) ? PIPE_DEFAULT_SIZE : PIPE_BUF_SIZE;
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
