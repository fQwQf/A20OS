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

typedef a20_pending_event_t a20_event_t;

/* ---- Event queue creation ---- */

static inline a20_status_t a20_event_queue_create(a20_handle_t *out)
{
    a20_event_queue_create_args_t args;
    args.size          = sizeof(args);
    args.version       = 1;
    args.capacity_hint = 0;
    args.flags         = 0;
    args.out_queue     = A20_HANDLE_NULL;

    a20_status_t r = a20_syscall6(A20_SYS_event_queue_create,
                                   (uint64_t)&args, 0, 0, 0, 0, 0);
    if (r < 0) return r;
    *out = (args.out_queue != A20_HANDLE_NULL) ? args.out_queue : (a20_handle_t)r;
    return A20_OK;
}

/* ---- Watch: register interest in handle events ---- */

static inline a20_status_t a20_event_watch(a20_handle_t queue, a20_handle_t handle,
                                            uint64_t event_mask, uint64_t user_data)
{
    a20_event_watch_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.queue      = queue;
    args.target     = handle;
    args.event_mask = event_mask;
    args.user_data  = user_data;

    return a20_syscall6(A20_SYS_event_watch, (uint64_t)&args, 0, 0, 0, 0, 0);
}

/* ---- Watch filesystem path events ---- */

static inline a20_status_t a20_event_watch_fs(a20_handle_t queue, a20_handle_t dir,
                                               const char *path, uint32_t path_len,
                                               uint32_t event_mask, uint64_t user_data)
{
    a20_event_watch_fs_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.queue      = queue;
    args.dir        = dir;
    args.path       = (uint64_t)path;
    args.path_len   = path_len;
    args.event_mask = event_mask;
    args.user_data  = user_data;

    return a20_syscall6(A20_SYS_event_watch_fs, (uint64_t)&args, 0, 0, 0, 0, 0);
}

/* ---- Wait: block for events ---- */

static inline a20_status_t a20_event_wait(a20_handle_t queue, a20_time_t timeout,
                                           a20_event_t *out)
{
    a20_event_wait_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.queue      = queue;
    args._pad       = 0;
    args.events     = (uint64_t)out;
    args.max_events = (out ? 1 : 0);
    args._pad2      = 0;
    args.timeout_ns = timeout.secs * 1000000000ULL + timeout.nsecs;
    args.flags      = 0;
    args.out_count  = 0;

    a20_status_t r = a20_syscall6(A20_SYS_event_wait, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (r < 0) return r;
    if (args.out_count != 0) return (a20_status_t)args.out_count;
    return r;
}

/* ---- Cancel watch ---- */

static inline a20_status_t a20_event_cancel(a20_handle_t queue, a20_handle_t target)
{
    return a20_syscall6(A20_SYS_event_cancel, queue, target, 0, 0, 0, 0);
}

/* ---- Convenience: wait on single handle ---- */

static inline a20_status_t a20_object_wait_one(a20_handle_t h, uint32_t event_mask,
                                               a20_time_t timeout, a20_event_t *out)
{
    a20_handle_t queue;
    a20_status_t r = a20_event_queue_create(&queue);
    if (r < 0) return r;

    r = a20_event_watch(queue, h, (uint64_t)event_mask, (uint64_t)h);
    if (r < 0) {
        a20_hdl_close(queue);
        return r;
    }

    r = a20_event_wait(queue, timeout, out);
    a20_hdl_close(queue);
    return r;
}

#endif
