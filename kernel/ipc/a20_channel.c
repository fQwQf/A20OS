#include "core/types.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/sync.h"
#include "mm/slab.h"
#include "sys/usercopy.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"

static a20_ch_message_t *ch_msg_alloc(const void *data, uint32_t data_len,
                                      a20_ch_handle_info_t *handles,
                                      uint32_t handle_count)
{
    uint32_t total = sizeof(a20_ch_message_t) + data_len +
                     handle_count * sizeof(a20_ch_handle_info_t);
    a20_ch_message_t *msg = kmalloc(total);
    if (!msg) return NULL;
    msg->data_len = data_len;
    msg->handle_count = handle_count;
    msg->next = NULL;
    if (data_len > 0 && data)
        memcpy(msg->data, data, data_len);
    if (handle_count > 0 && handles) {
        void *hbuf = msg->data + data_len;
        memcpy(hbuf, handles, handle_count * sizeof(a20_ch_handle_info_t));
    }
    return msg;
}

static void ch_msg_free(a20_ch_message_t *msg)
{
    kfree(msg);
}

/*
 * ch_check_send_types — verify handles are allowed by the channel's
 * send_handle_types bitmask (docs/native-abi/05-ipc.md §2.3).
 */
static int ch_check_send_types(a20_channel_type_t *ct,
                               a20_ch_handle_info_t *handles, uint32_t count)
{
    if (!ct) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t bit = 1u << handles[i].type;
        if (!(ct->send_handle_types & bit))
            return -A20_ERR_TYPE_MISMATCH;
    }
    return 0;
}

/*
 * ch_check_recv_types — verify received handles are allowed by the
 * channel's recv_handle_types bitmask (docs/native-abi/05-ipc.md §2.3).
 * Without this check a receiver could be forced to accept handle types
 * it did not agree to handle, breaking the typed-channel contract.
 */
static int ch_check_recv_types(a20_channel_type_t *ct,
                               a20_ch_handle_info_t *handles, uint32_t count)
{
    if (!ct) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t bit = 1u << handles[i].type;
        if (!(ct->recv_handle_types & bit))
            return -A20_ERR_TYPE_MISMATCH;
    }
    return 0;
}

a20_channel_ep_t *a20_channel_create(uint32_t msg_cap, a20_channel_type_t *type)
{
    if (msg_cap == 0) msg_cap = A20_CH_DEFAULT_CAP;

    a20_channel_ep_t *ep0 = kmalloc(sizeof(*ep0));
    a20_channel_ep_t *ep1 = kmalloc(sizeof(*ep1));
    if (!ep0 || !ep1) { kfree(ep0); kfree(ep1); return NULL; }

    memset(ep0, 0, sizeof(*ep0));
    memset(ep1, 0, sizeof(*ep1));

    refcount_set(&ep0->refcount, 1);
    refcount_set(&ep1->refcount, 1);
    spin_init(&ep0->lock);
    spin_init(&ep1->lock);
    wait_queue_init(&ep0->waiters);
    wait_queue_init(&ep1->waiters);
    ep0->peer = ep1;
    ep1->peer = ep0;
    ep0->msg_cap = msg_cap;
    ep1->msg_cap = msg_cap;
    ep0->chan_type = type;
    ep1->chan_type = type;

    return ep0;
}

int64_t a20_channel_send(a20_channel_ep_t *ep, const void *data, uint32_t data_len,
                         a20_ch_handle_info_t *handles, uint32_t handle_count,
                         struct a20_ht_internal *sender_ht)
{
    (void)sender_ht;
    if (!ep) return -A20_ERR_BAD_HANDLE;
    if (data_len > A20_CH_MAX_DATA) return -A20_ERR_INVALID_ARGUMENT;
    if (handle_count > A20_CH_MAX_HANDLES) return -A20_ERR_INVALID_ARGUMENT;

    if (handles && handle_count > 0) {
        int r = ch_check_send_types(ep->chan_type, handles, handle_count);
        if (r < 0) return r;
    }

    a20_ch_message_t *msg = ch_msg_alloc(data, data_len, handles, handle_count);
    if (!msg) return -A20_ERR_NO_MEMORY;

    a20_channel_ep_t *peer = ep->peer;
    if (!peer) { ch_msg_free(msg); return -A20_ERR_CANCELED; }

    spin_lock(&peer->lock);
    if (peer->peer_closed) {
        spin_unlock(&peer->lock);
        ch_msg_free(msg);
        return -A20_ERR_CANCELED;
    }
    if (peer->msg_count >= peer->msg_cap) {
        spin_unlock(&peer->lock);
        ch_msg_free(msg);
        return -A20_ERR_WOULD_BLOCK;
    }

    if (peer->msg_tail)
        peer->msg_tail->next = msg;
    else
        peer->msg_head = msg;
    peer->msg_tail = msg;
    peer->msg_count++;
    peer->total_data += data_len;

    wait_queue_wake_one(&peer->waiters);
    a20_event_notify(peer, A20_OBJ_CHANNEL_ENDPOINT, 0, 0, 0);
    spin_unlock(&peer->lock);
    return A20_OK;
}

int64_t a20_channel_recv(a20_channel_ep_t *ep, void *data, uint32_t *data_len,
                         a20_ch_handle_info_t *handles, uint32_t *handle_count,
                         struct a20_ht_internal *receiver_ht)
{
    (void)receiver_ht;
    if (!ep) return -A20_ERR_BAD_HANDLE;

    spin_lock(&ep->lock);
    if (ep->msg_count == 0) {
        if (ep->peer_closed) {
            spin_unlock(&ep->lock);
            return -A20_ERR_CANCELED;
        }
        spin_unlock(&ep->lock);
        return -A20_ERR_WOULD_BLOCK;
    }

    a20_ch_message_t *msg = ep->msg_head;
    ep->msg_head = msg->next;
    if (!ep->msg_head) ep->msg_tail = NULL;
    ep->msg_count--;
    ep->total_data -= msg->data_len;
    spin_unlock(&ep->lock);

    uint32_t out_len = msg->data_len;
    if (data && *data_len >= out_len) {
        memcpy(data, msg->data, out_len);
    }
    *data_len = out_len;

    uint32_t hc = msg->handle_count;
    if (handles && handle_count && hc > 0) {
        a20_ch_handle_info_t *hbuf = (a20_ch_handle_info_t *)(msg->data + msg->data_len);

        /* docs/native-abi/05-ipc.md §2.3: recv_handle_types enforcement */
        int tr = ch_check_recv_types(ep->chan_type, hbuf, hc);
        if (tr < 0) {
            ch_msg_free(msg);
            return tr;
        }

        uint32_t copy = hc < *handle_count ? hc : *handle_count;
        memcpy(handles, hbuf, copy * sizeof(a20_ch_handle_info_t));
    }
    if (handle_count) *handle_count = hc;

    if (ep->peer) {
        wait_queue_wake_one(&ep->peer->waiters);
    }

    ch_msg_free(msg);
    return A20_OK;
}

void a20_channel_ep_release(a20_channel_ep_t *ep)
{
    if (!ep) return;
    if (!refcount_dec_and_test(&ep->refcount)) return;

    a20_channel_ep_t *peer = ep->peer;
    if (peer) {
        spin_lock(&peer->lock);
        peer->peer_closed = 1;
        wait_queue_wake_all(&peer->waiters);
        a20_event_notify(peer, A20_OBJ_CHANNEL_ENDPOINT, 1, 0, 0);
        spin_unlock(&peer->lock);
    }

    a20_ch_message_t *msg = ep->msg_head;
    while (msg) {
        a20_ch_message_t *next = msg->next;
        ch_msg_free(msg);
        msg = next;
    }

    kfree(ep);
}
