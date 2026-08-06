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
    uint8_t pending[A20_CH_MAX_DATA];
    uint32_t pending_off;
    uint32_t pending_len;
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
    pipe->pending_off = 0;
    pipe->pending_len = 0;
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
    const uint8_t *p = (const uint8_t *)data;
    uint32_t left = len;
    while (left) {
        uint32_t n = left > A20_CH_MAX_DATA ? A20_CH_MAX_DATA : left;
        a20_status_t r = a20_channel_send(pipe->write_end, p, n, NULL, 0);
        if (r < 0)
            return r;
        p += n;
        left -= n;
    }
    return A20_OK;
}

static inline a20_status_t a20_personality_pipe_read(
    a20_personality_pipe_t *pipe, void *data, uint32_t *len)
{
    if (!pipe || !len)
        return -A20_ERR_FAULT;
    if (*len == 0)
        return A20_OK;
    uint8_t *dst = (uint8_t *)data;
    uint32_t want = *len;
    uint32_t got = 0;
    for (;;) {
        if (pipe->pending_off == pipe->pending_len) {
            pipe->pending_off = 0;
            pipe->pending_len = A20_CH_MAX_DATA;
            uint32_t handles = 0;
            a20_status_t r = a20_channel_recv_flags(
                pipe->read_end, pipe->pending, &pipe->pending_len,
                NULL, &handles, A20_MSG_NONBLOCK);
            if (r == -A20_ERR_WOULD_BLOCK) {
                pipe->pending_len = 0;
                break; /* no more queued messages */
            }
            if (r < 0) {
                pipe->pending_len = 0;
                return (got > 0) ? A20_OK : r;
            }
        }
        uint32_t available = pipe->pending_len - pipe->pending_off;
        uint32_t n = want - got < available ? want - got : available;
        a20_memcpy(dst + got, &pipe->pending[pipe->pending_off], n);
        pipe->pending_off += n;
        got += n;
        if (got == want)
            break;
    }
    *len = got;
    return A20_OK;
}

static inline a20_status_t a20_personality_pipe_wait_readable(
    a20_personality_pipe_t *pipe, a20_time_t timeout, a20_event_t *event)
{
    if (!pipe)
        return -A20_ERR_FAULT;
    if (pipe->pending_off < pipe->pending_len) {
        if (event) {
            a20_memset(event, 0, sizeof(*event));
            event->source = pipe->read_end;
            event->type = A20_EVENT_MESSAGE_READY;
            event->events = A20_EVENT_MASK(A20_EVENT_MESSAGE_READY);
        }
        return 1;
    }
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
    pipe->pending_off = pipe->pending_len = 0;
}

#endif
