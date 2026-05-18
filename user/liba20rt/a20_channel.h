/*
 * A20OS Native SDK — IPC channels.
 *
 * Typed, bidirectional message channels.
 * Each endpoint is a separate handle with its own rights.
 */
#ifndef _A20_CHANNEL_H
#define _A20_CHANNEL_H

#include "a20_types.h"
#include "a20_syscall.h"

typedef struct {
    a20_handle_t endpoints[2];
} a20_channel_pair_t;

static inline a20_status_t a20_channel_create(a20_channel_pair_t *out)
{
    return a20_syscall6(A20_SYS_channel_create, (uint64_t)out, 0, 0, 0, 0, 0);
}

typedef struct {
    const void *bytes;
    uint32_t    num_bytes;
    const void *handles;
    uint32_t    num_handles;
} a20_channel_msg_t;

static inline a20_status_t a20_channel_send(a20_handle_t ep,
                                             const void *bytes, uint32_t num_bytes,
                                             const a20_handle_t *handles, uint32_t num_handles)
{
    return a20_syscall6(A20_SYS_channel_send, ep,
                        (uint64_t)bytes, num_bytes,
                        (uint64_t)handles, num_handles, 0);
}

static inline a20_status_t a20_channel_recv(a20_handle_t ep,
                                             void *bytes, uint32_t *num_bytes,
                                             a20_handle_t *handles, uint32_t *num_handles)
{
    return a20_syscall6(A20_SYS_channel_recv, ep,
                        (uint64_t)bytes, (uint64_t)num_bytes,
                        (uint64_t)handles, (uint64_t)num_handles, 0);
}

#endif
