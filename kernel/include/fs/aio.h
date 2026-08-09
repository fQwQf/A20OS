#ifndef _FS_AIO_H
#define _FS_AIO_H

/*
 * A20OS Linux AIO subsystem (ABI-independent).
 *
 * Implements the classic Linux aio_context / iocb / io_event model:
 *
 *   - io_setup() creates an aio context and returns an opaque context id.
 *   - io_submit() submits an array of iocbs.  A20OS executes the supported
 *     operations (pread/pwrite/fsync/fdatasync) through the VFS synchronously
 *     in the submitting task and publishes completion io_event records into a
 *     kernel completion queue.
 *   - io_getevents()/io_pgetevents() wait for at least min_nr completed
 *     events and copy them to the caller.
 *   - io_cancel() removes a not-yet-completed iocb; with synchronous
 *     execution the cancellable window is the moment a request is queued
 *     before it executes, so the implementation covers the ABI surface.
 *   - io_destroy() tears the context down.
 *
 * Contexts are owned by the creating task's mm and are reaped automatically
 * when the mm is destroyed, so a process that forgets io_destroy() cannot
 * leak ring pages.
 *
 * The ABI layer (kernel/abi/linux/sys_aio.c) translates the Linux wire
 * structures and user pointers and delegates here.  This file owns the
 * context table and completion queue; it never touches user pointers.
 */

#include "core/types.h"

struct mm_struct;

#define AIO_MAX_EVENTS 1024
#define AIO_MAX_CONTEXTS 32

/* Linux iocb opcodes (subset supported by A20OS). */
#define IOCB_CMD_PREAD    0
#define IOCB_CMD_PWRITE   1
#define IOCB_CMD_FSYNC    2
#define IOCB_CMD_FDSYNC   3
#define IOCB_CMD_POLL     5
#define IOCB_CMD_NOOP     6

/* Kernel-side iocb descriptor: the ABI layer translates the 64-byte Linux
 * iocb into this validated form before calling aio_context_submit(). */
typedef struct aio_iocb_kern {
    uint64_t data;      /* aio_data, echoed into io_event */
    uint64_t obj;       /* user iocb pointer, echoed into io_event */
    int      opcode;    /* IOCB_CMD_* */
    int      fd;        /* user fd number */
    uint64_t buf;       /* user buffer address (for pread/pwrite) */
    uint64_t nbytes;    /* transfer length */
    int64_t  offset;    /* file offset (pread/pwrite) */
} aio_iocb_kern_t;

/* Kernel-side completion record (Linux io_event wire layout). */
typedef struct aio_event_kern {
    uint64_t data;      /* aio_data */
    uint64_t obj;       /* user iocb pointer */
    int64_t  res;       /* primary result */
    int64_t  res2;      /* secondary result */
} aio_event_kern_t;

/* ---- Context API ---- */

/* Create a context with @nr_events capacity owned by @mm.  Returns 0 on
 * success and stores the opaque context id into *ctx_out. */
int aio_context_create(struct mm_struct *mm, unsigned nr_events,
                       uint64_t *ctx_out);

/* Destroy a context.  Returns 0 or a negative errno. */
int aio_context_destroy(struct mm_struct *mm, uint64_t ctx);

/* Submit @nr iocbs (kernel-side array).  Returns the number submitted or a
 * negative errno.  Synchronous execution publishes completions immediately. */
long aio_context_submit(struct mm_struct *mm, uint64_t ctx,
                        const aio_iocb_kern_t *iocbs, long nr);

/* Wait for at least min_nr completions and copy up to nr events into
 * @events (kernel buffer of nr * sizeof(aio_event_kern_t)).  @deadline is an
 * absolute tick deadline (0 = wait forever, or use aio_getevents_nowait).
 * Returns the number of events copied, or a negative errno. */
long aio_context_getevents(struct mm_struct *mm, uint64_t ctx, long min_nr,
                           long nr, aio_event_kern_t *events,
                           uint64_t deadline);

/* Reap already-completed events without waiting. */
long aio_context_getevents_nowait(struct mm_struct *mm, uint64_t ctx,
                                  long nr, aio_event_kern_t *events);

/* Cancel a submitted iocb identified by its user pointer @obj.  If found
 * still pending, removes it and stores the completion into *result; returns 0.
 * Returns -EAGAIN if the iocb is not pending (already completed or unknown). */
int aio_context_cancel(struct mm_struct *mm, uint64_t ctx, uint64_t obj,
                       aio_event_kern_t *result);

/* Reap all contexts owned by @mm (called from mm teardown). */
void aio_context_reap_mm(struct mm_struct *mm);

#endif /* _FS_AIO_H */
