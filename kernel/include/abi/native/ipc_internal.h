#ifndef _ABI_NATIVE_IPC_INTERNAL_H
#define _ABI_NATIVE_IPC_INTERNAL_H

#include "core/types.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/sync.h"
#include "abi/native/types.h"
#include "abi/native/rights.h"

struct a20_ht_internal;

#define A20_CH_MAX_DATA      65536
#define A20_CH_MAX_HANDLES   8
#define A20_CH_DEFAULT_CAP   64
#define A20_EVQ_DEFAULT_CAP  256

typedef struct a20_ch_handle_info {
    void           *object;
    uint16_t        type;
    uint16_t        _pad;
    a20_rights_t    transfer_rights;
} a20_ch_handle_info_t;

typedef struct a20_ch_message {
    uint32_t                data_len;
    uint32_t                handle_count;
    struct a20_ch_message  *next;
    uint8_t                 data[];
} a20_ch_message_t;

typedef struct a20_channel_ep {
    refcount_t              refcount;
    spinlock_t              lock;
    wait_queue_t            waiters;
    struct a20_channel_ep  *peer;
    int                     peer_closed;
    a20_channel_type_t     *chan_type;

    a20_ch_message_t       *msg_head;
    a20_ch_message_t       *msg_tail;
    uint32_t                msg_count;
    uint32_t                msg_cap;
    uint32_t                total_data;
} a20_channel_ep_t;

typedef struct a20_watch_entry {
    a20_handle_t            target_handle;
    void                   *target_object;
    uint16_t                target_type;
    uint16_t                _pad;
    uint64_t                event_mask;
    uint64_t                user_data;
    struct a20_eventq      *owner_queue;
    struct a20_watch_entry *next;
} a20_watch_entry_t;

typedef struct a20_eventq {
    refcount_t              refcount;
    spinlock_t              lock;
    wait_queue_t            waiters;

    a20_watch_entry_t      *watches;
    uint32_t                watch_count;

    a20_pending_event_t    *ring;
    uint32_t                ring_cap;
    uint32_t                ring_head;
    uint32_t                ring_tail;
    uint32_t                ring_count;
} a20_eventq_t;

a20_channel_ep_t *a20_channel_create(uint32_t msg_cap, a20_channel_type_t *type);
int64_t a20_channel_send(a20_channel_ep_t *ep, const void *data, uint32_t data_len,
                         a20_ch_handle_info_t *handles, uint32_t handle_count,
                         struct a20_ht_internal *sender_ht);
int64_t a20_channel_recv(a20_channel_ep_t *ep, void *data, uint32_t *data_len,
                         a20_ch_handle_info_t *handles, uint32_t *handle_count,
                         struct a20_ht_internal *receiver_ht);
void a20_channel_ep_release(a20_channel_ep_t *ep);

a20_eventq_t *a20_eventq_create(uint32_t capacity_hint);
int64_t a20_eventq_watch(a20_eventq_t *eq, a20_handle_t target_h, void *target_obj,
                         uint16_t target_type, uint64_t event_mask, uint64_t user_data);
int64_t a20_eventq_wait(a20_eventq_t *eq, a20_pending_event_t *out, uint64_t timeout_ns);
int64_t a20_eventq_cancel(a20_eventq_t *eq, a20_handle_t target_h);
void a20_eventq_release(a20_eventq_t *eq);

void a20_event_notify(void *target_object, uint16_t target_type,
                      uint32_t event_type, uint64_t data0, uint64_t data1);
void a20_eventq_on_object_destroy(void *object);
void a20_timer_tick(void);

#endif
