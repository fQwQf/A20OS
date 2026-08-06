/*
 * First Linux-personality brick: a pipe-shaped stream facade over a
 * Native channel pair (docs/hybrid-kernel/05-idl-and-personality.md).
 *
 * This is intentionally a small, message-preserving subset. It proves the
 * object translation and close/readiness path before adding a byte-stream
 * accumulator, fd table, mmap/VMO and socket personalities.
 */
#ifndef _A20_PERSONALITY_H
#define _A20_PERSONALITY_H

#include "a20_channel.h"
#include "a20_event.h"
#include "a20_handle.h"

typedef struct a20_personality_pipe {
    a20_handle_t read_end;
    a20_handle_t write_end;
    a20_handle_t wait_queue;
} a20_personality_pipe_t;

static inline a20_status_t a20_personality_pipe_create(
    a20_personality_pipe_t *pipe)
{
    if (!pipe)
        return -A20_ERR_FAULT;
    a20_channel_pair_t pair;
    a20_status_t r = a20_channel_create(&pair);
    if (r < 0)
        return r;
    pipe->write_end = pair.endpoints[0];
    pipe->read_end = pair.endpoints[1];
    r = a20_event_queue_create(&pipe->wait_queue);
    if (r < 0) {
        a20_hdl_close(pipe->read_end);
        a20_hdl_close(pipe->write_end);
        return r;
    }
    r = a20_event_watch(pipe->wait_queue, pipe->read_end,
                        A20_EVENT_MASK(A20_EVENT_MESSAGE_READY), 0);
    if (r < 0) {
        a20_hdl_close(pipe->wait_queue);
        a20_hdl_close(pipe->read_end);
        a20_hdl_close(pipe->write_end);
        return r;
    }
    return A20_OK;
}

static inline a20_status_t a20_personality_pipe_write(
    a20_personality_pipe_t *pipe, const void *data, uint32_t len)
{
    if (!pipe)
        return -A20_ERR_FAULT;
    return a20_channel_send(pipe->write_end, data, len, NULL, 0);
}

static inline a20_status_t a20_personality_pipe_read(
    a20_personality_pipe_t *pipe, void *data, uint32_t *len)
{
    if (!pipe || !len)
        return -A20_ERR_FAULT;
    uint32_t handles = 0;
    return a20_channel_recv(pipe->read_end, data, len, NULL, &handles);
}

static inline a20_status_t a20_personality_pipe_wait_readable(
    a20_personality_pipe_t *pipe, a20_time_t timeout, a20_event_t *event)
{
    if (!pipe)
        return -A20_ERR_FAULT;
    return a20_event_wait(pipe->wait_queue, timeout, event);
}

static inline void a20_personality_pipe_close(a20_personality_pipe_t *pipe)
{
    if (!pipe)
        return;
    if (pipe->read_end != A20_HANDLE_NULL)
        a20_hdl_close(pipe->read_end);
    if (pipe->write_end != A20_HANDLE_NULL)
        a20_hdl_close(pipe->write_end);
    if (pipe->wait_queue != A20_HANDLE_NULL)
        a20_hdl_close(pipe->wait_queue);
    pipe->read_end = pipe->write_end = pipe->wait_queue = A20_HANDLE_NULL;
}

#endif
