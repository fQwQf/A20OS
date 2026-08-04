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

static inline a20_status_t a20_channel_create_typed(a20_channel_pair_t *out,
                                                      const a20_channel_type_t *type);

static inline a20_status_t a20_channel_create(a20_channel_pair_t *out)
{
    return a20_channel_create_typed(out, 0);
}

/*
 * a20_channel_create_typed — create a channel, optionally with a type
 * signature (docs/native-abi/05-ipc.md §2.3).  type == NULL yields an
 * untyped channel (backward compatible).
 */
static inline a20_status_t a20_channel_create_typed(a20_channel_pair_t *out,
                                                      const a20_channel_type_t *type)
{
    a20_channel_create_args_t args;
    args.size         = sizeof(args);
    args.version      = 1;
    args.msg_capacity = 0;
    args.flags        = 0;
    args.type         = (uint64_t)type;
    args.out_endpoints[0] = A20_HANDLE_NULL;
    args.out_endpoints[1] = A20_HANDLE_NULL;

    a20_status_t r = a20_syscall6(A20_SYS_channel_create, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (a20_status_is_ok(r)) {
        out->endpoints[0] = args.out_endpoints[0];
        out->endpoints[1] = args.out_endpoints[1];
    }
    return r;
}

typedef struct {
    const void *bytes;
    uint32_t    num_bytes;
    const void *handles;
    uint32_t    num_handles;
} a20_channel_msg_t;

static inline a20_status_t a20_channel_send_flags(a20_handle_t ep,
                                                    const void *bytes, uint32_t num_bytes,
                                                    const a20_handle_t *handles,
                                                    uint32_t num_handles, uint32_t flags)
{
    a20_msg_send_args_t args;
    args.size         = sizeof(args);
    args.version      = 1;
    args.channel      = ep;
    args._pad         = 0;
    args.data         = (uint64_t)bytes;
    args.data_len     = num_bytes;
    args.flags        = flags;
    args.handles      = (uint64_t)handles;
    args.handle_count = num_handles;
    args.transfer_rights = 0;
    return a20_syscall6(A20_SYS_channel_send, (uint64_t)&args, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_channel_send(a20_handle_t ep,
                                              const void *bytes, uint32_t num_bytes,
                                              const a20_handle_t *handles, uint32_t num_handles)
{
    return a20_channel_send_flags(ep, bytes, num_bytes, handles, num_handles, 0);
}

static inline a20_status_t a20_channel_recv_flags(a20_handle_t ep,
                                                    void *bytes, uint32_t *num_bytes,
                                                    a20_handle_t *handles,
                                                    uint32_t *num_handles, uint32_t flags)
{
    a20_msg_recv_args_t args;
    args.size             = sizeof(args);
    args.version          = 1;
    args.channel          = ep;
    args._pad             = 0;
    args.data_buf         = (uint64_t)bytes;
    args.data_buf_len     = *num_bytes;
    args._pad2            = 0;
    args.handle_buf       = (uint64_t)handles;
    args.handle_buf_count = *num_handles;
    args.flags            = flags;
    args.out_data_len     = 0;
    args.out_handle_count = 0;
    args.out_rights_buf   = 0;

    a20_status_t r = a20_syscall6(A20_SYS_channel_recv, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (a20_status_is_ok(r)) {
        *num_bytes   = (uint32_t)args.out_data_len;
        *num_handles = args.out_handle_count;
    }
    return r;
}

static inline a20_status_t a20_channel_recv(a20_handle_t ep,
                                               void *bytes, uint32_t *num_bytes,
                                               a20_handle_t *handles, uint32_t *num_handles)
{
    return a20_channel_recv_flags(ep, bytes, num_bytes, handles, num_handles, 0);
}

/*
 * a20_channel_call — fused RPC (docs/hybrid-kernel/00-design.md §4.1):
 * send a request and wait for the reply in a single trap.  Returns the
 * reply byte count (>= 0) or a negative a20 error.  With A20_MSG_NONBLOCK a
 * -A20_ERR_WOULD_BLOCK means the request was delivered but no reply was
 * queued yet; the reply can be collected with a20_channel_recv afterwards.
 */
static inline a20_status_t a20_channel_call_flags(a20_handle_t ep,
                                                  const void *bytes, uint32_t num_bytes,
                                                  const a20_handle_t *handles,
                                                  uint32_t num_handles,
                                                  void *reply, uint32_t *reply_bytes,
                                                  a20_handle_t *reply_handles,
                                                  uint32_t *reply_num_handles,
                                                  uint32_t flags)
{
    a20_channel_call_args_t args;
    args.size             = sizeof(args);
    args.version          = 1;
    args.channel          = ep;
    args.flags            = flags;
    args.data             = (uint64_t)bytes;
    args.data_len         = num_bytes;
    args._pad             = 0;
    args.handles          = (uint64_t)handles;
    args.handle_count     = num_handles;
    args.transfer_rights  = 0;
    args.reply_buf        = (uint64_t)reply;
    args.reply_buf_len    = reply_bytes ? *reply_bytes : 0;
    args._pad2            = 0;
    args.reply_handle_buf = (uint64_t)reply_handles;
    args.reply_handle_buf_count = reply_num_handles ? *reply_num_handles : 0;
    args.reply_rights_buf = 0;
    args.out_reply_len    = 0;
    args.out_reply_handles = 0;

    a20_status_t r = a20_syscall6(A20_SYS_channel_call, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (a20_status_is_ok(r)) {
        if (reply_bytes) *reply_bytes = args.out_reply_len;
        if (reply_num_handles) *reply_num_handles = args.out_reply_handles;
    }
    return r;
}

static inline a20_status_t a20_channel_call(a20_handle_t ep,
                                               const void *bytes, uint32_t num_bytes,
                                               const a20_handle_t *handles,
                                               uint32_t num_handles,
                                               void *reply, uint32_t *reply_bytes,
                                               a20_handle_t *reply_handles,
                                               uint32_t *reply_num_handles)
{
    return a20_channel_call_flags(ep, bytes, num_bytes, handles, num_handles,
                                  reply, reply_bytes, reply_handles,
                                  reply_num_handles, 0);
}

#endif
