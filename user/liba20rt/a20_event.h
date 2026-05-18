/*
 * A20OS Native SDK — Event / Object waiting.
 *
 * Replaces select/poll/epoll with a unified object-wait model.
 * Similar to Zircon's zx_object_wait_one/many.
 */
#ifndef _A20_EVENT_H
#define _A20_EVENT_H

#include "a20_types.h"
#include "a20_syscall.h"

/* ---- Event queue creation ---- */

static inline a20_status_t a20_event_queue_create(a20_handle_t *out)
{
    return a20_syscall6(A20_SYS_event_queue_create, (uint64_t)out, 0, 0, 0, 0, 0);
}

/* ---- Watch: register interest in handle events ---- */

typedef struct {
    a20_handle_t handle;
    uint32_t     event_mask;
    uint64_t     cookie;
} a20_event_watch_args_t;

static inline a20_status_t a20_event_watch(a20_handle_t queue, a20_handle_t handle,
                                            uint32_t event_mask, uint64_t cookie)
{
    return a20_syscall6(A20_SYS_event_watch, queue, handle,
                        event_mask, cookie, 0, 0);
}

static inline a20_status_t a20_event_watch_fs(a20_handle_t queue, a20_handle_t dir_handle,
                                               uint32_t event_mask, uint64_t cookie)
{
    return a20_syscall6(A20_SYS_event_watch_fs, queue, dir_handle,
                        event_mask, cookie, 0, 0);
}

/* ---- Wait: block for events ---- */

typedef struct {
    uint64_t cookie;
    uint32_t event_type;
    uint32_t reserved;
    uint64_t data0;
    uint64_t data1;
} a20_event_t;

static inline a20_status_t a20_event_wait(a20_handle_t queue, a20_time_t timeout,
                                           a20_event_t *out)
{
    return a20_syscall6(A20_SYS_event_wait, queue,
                        timeout.secs, timeout.nsecs, (uint64_t)out, 0, 0);
}

/* ---- Cancel watch ---- */

static inline a20_status_t a20_event_cancel(a20_handle_t queue, uint64_t cookie)
{
    return a20_syscall6(A20_SYS_event_cancel, queue, cookie, 0, 0, 0, 0);
}

/* ---- Convenience: wait on single handle ---- */

static inline a20_status_t a20_object_wait_one(a20_handle_t h, uint32_t event_mask,
                                                a20_time_t timeout, a20_event_t *out)
{
    a20_handle_t queue;
    a20_status_t r = a20_event_queue_create(&queue);
    if (r < 0) return r;

    r = a20_event_watch(queue, h, event_mask, (uint64_t)h);
    if (r < 0) {
        a20_hdl_close(queue);
        return r;
    }

    r = a20_event_wait(queue, timeout, out);
    a20_hdl_close(queue);
    return r;
}

#endif
