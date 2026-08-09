#include "syscall_impl.h"

#include "fs/aio.h"
#include "core/timer.h"

/*
 * Linux AIO syscalls.
 *
 * The 64-bit Linux wire layouts (struct iocb = 64 bytes, struct io_event = 32
 * bytes, aio_context_t = u64) are translated here; kernel semantics live in
 * kernel/fs/aio.c.
 */

typedef struct linux_iocb {
    uint64_t aio_data;
    uint32_t aio_key;
    uint32_t aio_reserved1;
    uint16_t aio_lio_opcode;
    int16_t  aio_reqprio;
    uint32_t aio_fildes;
    uint64_t aio_buf;
    uint64_t aio_nbytes;
    int64_t  aio_offset;
    uint64_t aio_reserved2;
    uint32_t aio_flags;
    uint32_t aio_resfd;
} __attribute__((packed)) linux_iocb_t;

typedef struct linux_io_event {
    uint64_t data;
    uint64_t obj;
    int64_t  res;
    int64_t  res2;
} linux_io_event_t;

typedef struct linux_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
} linux_timespec64_t;

_Static_assert(sizeof(linux_iocb_t) == 64, "iocb must be 64 bytes");
_Static_assert(sizeof(linux_io_event_t) == 32, "io_event must be 32 bytes");

static int64_t aio_timeout_to_deadline(const void *timeout)
{
    if (!timeout)
        return 0;
    linux_timespec64_t ts;
    if (copy_from_user(&ts, timeout, sizeof(ts)) < 0)
        return -1;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL)
        return -2;
    uint64_t ticks = (uint64_t)ts.tv_sec * TICKS_PER_SEC +
                     (uint64_t)ts.tv_nsec * TICKS_PER_SEC / 1000000000ULL;
    if ((ts.tv_sec || ts.tv_nsec) && ticks == 0)
        ticks = 1;
    return (int64_t)(timer_get_ticks() + ticks);
}

int64_t sys_io_setup(unsigned nr_events, void *ctxp)
{
    if (!ctxp)
        return -EFAULT;
    uint64_t ctx = 0;
    int r = aio_context_create(proc_current()->mm, nr_events, &ctx);
    if (r < 0)
        return r;
    if (copy_to_user(ctxp, &ctx, sizeof(ctx)) < 0) {
        aio_context_destroy(proc_current()->mm, ctx);
        return -EFAULT;
    }
    return 0;
}

int64_t sys_io_destroy(uint64_t ctx)
{
    task_t *t = proc_current();
    if (!t || !t->mm)
        return -EINVAL;
    return aio_context_destroy(t->mm, ctx);
}

int64_t sys_io_submit(uint64_t ctx, long nr, const void *iocbpp)
{
    task_t *t = proc_current();
    if (!t || !t->mm)
        return -EINVAL;
    if (nr < 0)
        return -EINVAL;
    if (nr == 0)
        return 0;
    if (!iocbpp)
        return -EFAULT;
    if (nr > AIO_MAX_EVENTS)
        return -EAGAIN;

    /* Copy the array of user iocb pointers, then the iocbs themselves.  The
     * kernel scratch buffer is a single reusable buffer, so read the pointer
     * array into the scratch buffer first and copy it to a stack array before
     * the next scratch allocation overwrites it. */
    uintptr_t up[AIO_MAX_EVENTS];
    if (nr > AIO_MAX_EVENTS)
        return -EAGAIN;
    void *ptrs = proc_scratch_buffer((size_t)nr * sizeof(uintptr_t));
    if (!ptrs)
        return -ENOMEM;
    if (copy_from_user(ptrs, iocbpp, (size_t)nr * sizeof(uintptr_t)) < 0)
        return -EFAULT;
    memcpy(up, ptrs, (size_t)nr * sizeof(uintptr_t));

    aio_iocb_kern_t *kern = proc_scratch_buffer((size_t)nr * sizeof(*kern));
    if (!kern)
        return -ENOMEM;

    long submitted = 0;
    for (long i = 0; i < nr; i++) {
        if (!up[i]) {
            if (i == 0)
                return -EFAULT;
            break;
        }
        linux_iocb_t iocb;
        if (copy_from_user(&iocb, (const void *)up[i], sizeof(iocb)) < 0) {
            if (i == 0)
                return -EFAULT;
            break;
        }
        kern[submitted].data = iocb.aio_data;
        kern[submitted].obj = up[i];
        kern[submitted].opcode = iocb.aio_lio_opcode;
        kern[submitted].fd = (int)iocb.aio_fildes;
        kern[submitted].buf = iocb.aio_buf;
        kern[submitted].nbytes = iocb.aio_nbytes;
        kern[submitted].offset = iocb.aio_offset;
        submitted++;
    }
    if (submitted == 0)
        return -EINVAL;

    return aio_context_submit(t->mm, ctx, kern, submitted);
}

int64_t sys_io_getevents(uint64_t ctx, long min_nr, long nr, void *events,
                         const void *timeout)
{
    task_t *t = proc_current();
    if (!t || !t->mm)
        return -EINVAL;
    if (nr < 0 || min_nr < 0)
        return -EINVAL;
    if (!events || nr == 0)
        return -EINVAL;

    int64_t deadline = aio_timeout_to_deadline(timeout);
    if (deadline == -1)
        return -EFAULT;
    if (deadline == -2)
        return -EINVAL;

    void *kbuf = proc_scratch_buffer((size_t)nr * sizeof(linux_io_event_t));
    if (!kbuf)
        return -ENOMEM;

    long got;
    if (deadline == 0 && !timeout)
        got = aio_context_getevents(t->mm, ctx, min_nr, nr,
                                    (aio_event_kern_t *)kbuf, 0);
    else
        got = aio_context_getevents(t->mm, ctx, min_nr, nr,
                                    (aio_event_kern_t *)kbuf,
                                    (uint64_t)deadline);
    if (got < 0)
        return got;
    if (got > 0 && copy_to_user(events, kbuf,
                                (size_t)got * sizeof(linux_io_event_t)) < 0)
        return -EFAULT;
    return got;
}

int64_t sys_io_pgetevents(uint64_t ctx, long min_nr, long nr, void *events,
                          const void *timeout, const void *sigmask,
                          size_t sigsetsize)
{
    (void)sigmask;
    (void)sigsetsize;
    return sys_io_getevents(ctx, min_nr, nr, events, timeout);
}

int64_t sys_io_cancel(uint64_t ctx, const void *iocb, void *result)
{
    task_t *t = proc_current();
    if (!t || !t->mm)
        return -EINVAL;
    if (!iocb || !result)
        return -EFAULT;

    aio_event_kern_t ev;
    int r = aio_context_cancel(t->mm, ctx, (uint64_t)(uintptr_t)iocb, &ev);
    if (r < 0)
        return r;
    linux_io_event_t out;
    out.data = ev.data;
    out.obj = ev.obj;
    out.res = ev.res;
    out.res2 = ev.res2;
    if (copy_to_user(result, &out, sizeof(out)) < 0)
        return -EFAULT;
    return 0;
}
