#ifndef _ABI_NATIVE_CHANNEL_RECV_TXN_H
#define _ABI_NATIVE_CHANNEL_RECV_TXN_H

#ifdef A20_CHANNEL_RECV_TXN_HOST_TEST
#include <stdint.h>
typedef uint32_t a20_handle_t;
typedef uint64_t a20_rights_t;
#define A20_CH_MAX_HANDLES 8
#define A20_ERR_NO_MEMORY 6
#define A20_ERR_FAULT 8
#define A20_ERR_NO_SPACE 13
#define A20_ERR_PROTOCOL 21
typedef struct a20_channel_ep { uint32_t opaque; } a20_channel_ep_t;
struct a20_ht_internal { uint32_t opaque; };
typedef struct a20_ch_handle_info {
    void *object;
    uint16_t type;
    uint16_t _pad;
    a20_rights_t transfer_rights;
    uint64_t expiry_tick;
    uint32_t remaining_ops;
    uint32_t temporal_flags;
    uint8_t security_label;
    uint8_t _pad2[7];
} a20_ch_handle_info_t;
int64_t a20_channel_recv_begin(a20_channel_ep_t *, uint32_t, uint32_t *, uint32_t *);
int64_t a20_channel_recv_begin_donate(a20_channel_ep_t *, uint32_t, uint32_t *, uint32_t *);
void a20_channel_recv_abort(a20_channel_ep_t *);
int64_t a20_channel_recv_finish(a20_channel_ep_t *, void *, uint32_t *,
                                a20_ch_handle_info_t *, uint32_t *);
void a20_object_release(void *, uint16_t);
#else
#include "ipc/ipc.h"
#include "ipc/handle_table.h"
#endif

/* Publishes the final syscall argument structure only after data and handle
 * arrays have been copied. A nonzero result is treated as A20_ERR_FAULT and
 * causes every installed handle to be removed. */
typedef int (*a20_channel_recv_publish_fn)(void *context,
                                            uint32_t data_len,
                                            uint32_t handle_count);

#ifdef A20_CHANNEL_RECV_TXN_HOST_TEST
int64_t a20_channel_recv_txn_c(a20_channel_ep_t *ep,
                               struct a20_ht_internal *ht,
                               uint32_t flags, int donate,
                               void *user_data, uint32_t data_capacity,
                               void *user_handles, uint32_t handle_capacity,
                               void *user_rights, void *publish_context,
                               a20_channel_recv_publish_fn publish);
#endif

#if defined(CONFIG_CHANNEL_RECV_TXN_RUST) || defined(A20_CHANNEL_RECV_TXN_HOST_TEST)
int64_t a20_rust_channel_recv_txn(a20_channel_ep_t *ep,
                                  struct a20_ht_internal *ht,
                                  uint32_t flags, int donate,
                                  void *user_data, uint32_t data_capacity,
                                  void *user_handles, uint32_t handle_capacity,
                                  void *user_rights, void *publish_context,
                                  a20_channel_recv_publish_fn publish);
#endif

static inline int64_t a20_channel_recv_txn(a20_channel_ep_t *ep,
                                            struct a20_ht_internal *ht,
                                            uint32_t flags, int donate,
                                            void *user_data,
                                            uint32_t data_capacity,
                                            void *user_handles,
                                            uint32_t handle_capacity,
                                            void *user_rights,
                                            void *publish_context,
                                            a20_channel_recv_publish_fn publish)
{
    return a20_rust_channel_recv_txn(ep, ht, flags, donate, user_data,
                                    data_capacity, user_handles,
                                    handle_capacity, user_rights,
                                    publish_context, publish);
}

#endif
