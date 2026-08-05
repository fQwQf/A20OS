/*
 * A20OS Native ABI — Channel endpoints.
 * Design reference: docs/native-abi/05-ipc.md §2
 * Design inspiration: Zircon (Fuchsia) Channel handle-transfer semantics
 * and seL4 Endpoint capability transfer; see docs/ACKNOWLEDGMENTS.md §3.
 *
 * CH_PEER_TEARDOWN_PROTOCOL:
 * Both endpoints are freed independently once their refcount hits zero, so
 * all peer dereferences are serialized by g_ch_lock: an endpoint reads its
 * peer pointer under g_ch_lock, then takes peer->lock before dropping
 * g_ch_lock.  Release takes g_ch_lock and then peer->lock before kfree,
 * which guarantees a sender holding peer->lock never touches freed memory.
 */
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
#include "proc/proc.h"

extern void a20_object_ref(void *object, uint16_t type);
extern void a20_object_release(void *object, uint16_t type);

static spinlock_t g_ch_lock = SPINLOCK_INIT;

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

/*
 * ch_msg_free — release a message.  Handle references taken at send time
 * travel with the message: they are dropped here on discard, and transfer
 * to the receiver's new handles on successful delivery (release_refs=0).
 */
static void ch_msg_free(a20_ch_message_t *msg, int release_refs)
{
    if (!msg) return;
    if (release_refs && msg->handle_count > 0) {
        a20_ch_handle_info_t *hbuf =
            (a20_ch_handle_info_t *)(msg->data + msg->data_len);
        for (uint32_t i = 0; i < msg->handle_count; i++)
            a20_object_release(hbuf[i].object, hbuf[i].type);
    }
    kfree(msg);
}

/*
 * ch_check_send_types — verify handles are allowed by the channel's
 * send_handle_types bitmask (docs/native-abi/05-ipc.md §2.3).
 */
static int ch_check_send_types(const a20_channel_type_t *ct,
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
static int ch_check_recv_types(const a20_channel_type_t *ct,
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

a20_channel_ep_t *a20_channel_create(uint32_t msg_cap, const a20_channel_type_t *type)
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

    /* Typed channel (docs/native-abi/05-ipc.md §2.3): each endpoint keeps
     * its own copy of the type signature so there is no shared-lifetime
     * problem between the two endpoints. */
    if (type) {
        ep0->chan_type_storage = *type;
        ep1->chan_type_storage = *type;
        ep0->chan_type = &ep0->chan_type_storage;
        ep1->chan_type = &ep1->chan_type_storage;
    }

    return ep0;
}

/*
 * ch_try_enqueue — one non-blocking enqueue attempt.
 * Returns 1 on success, 0 when the peer queue is full, < 0 on error.
 * defer_wake skips the RECV wait-queue wake (channel_call donation path);
 * the caller then owns delivering the wake or a direct handoff.
 */
static int ch_try_enqueue(a20_channel_ep_t *ep, a20_ch_message_t *msg,
                          int defer_wake)
{
    spin_lock(&g_ch_lock);
    a20_channel_ep_t *peer = ep->peer;
    if (peer)
        spin_lock(&peer->lock);
    spin_unlock(&g_ch_lock);

    if (!peer)
        return -A20_ERR_CANCELED;
    if (peer->peer_closed) {
        spin_unlock(&peer->lock);
        return -A20_ERR_CANCELED;
    }
    if (peer->msg_count >= peer->msg_cap) {
        spin_unlock(&peer->lock);
        return 0;
    }

    if (peer->msg_tail)
        peer->msg_tail->next = msg;
    else
        peer->msg_head = msg;
    peer->msg_tail = msg;
    peer->msg_count++;
    peer->total_data += msg->data_len;

    if (!defer_wake)
        wait_queue_wake_one(&peer->waiters, A20_CH_WAIT_RECV, PROC_WAKE_EVENT);
    a20_event_notify(peer, A20_OBJ_CHANNEL_ENDPOINT,
                     A20_EVENT_MESSAGE_READY, 0, 0);
    spin_unlock(&peer->lock);
    return 1;
}

int64_t a20_channel_send_dwc(a20_channel_ep_t *ep, const void *data, uint32_t data_len,
                             a20_ch_handle_info_t *handles, uint32_t handle_count,
                             struct a20_ht_internal *sender_ht, uint32_t flags,
                             int defer_wake)
{
    (void)sender_ht;
    if (!ep) return -A20_ERR_BAD_HANDLE;
    if (data_len > A20_CH_MAX_DATA) return -A20_ERR_INVALID_ARGUMENT;
    if (handle_count > A20_CH_MAX_HANDLES) return -A20_ERR_INVALID_ARGUMENT;
    if (flags & ~A20_MSG_NONBLOCK) return -A20_ERR_INVALID_ARGUMENT;

    /* Hold an endpoint reference for the syscall duration so a concurrent
     * handle_close cannot free ep (or its peer via cascade) under us. */
    refcount_inc(&ep->refcount);
    int64_t result;

    /* Typed channel enforcement (docs/native-abi/05-ipc.md §2.3) */
    const a20_channel_type_t *ct = ep->chan_type;
    if (ct) {
        if (ct->max_data_size && data_len > ct->max_data_size) {
            result = -A20_ERR_INVALID_ARGUMENT;
            goto out;
        }
        if (ct->max_handles && handle_count > ct->max_handles) {
            result = -A20_ERR_INVALID_ARGUMENT;
            goto out;
        }
        if (handles && handle_count > 0) {
            int r = ch_check_send_types(ct, handles, handle_count);
            if (r < 0) { result = r; goto out; }
        }
    }

    /* Shared transfer semantics (05-ipc.md §2.7): the sender keeps its
     * handle, the message carries one new reference per handle. */
    for (uint32_t i = 0; i < handle_count; i++)
        a20_object_ref(handles[i].object, handles[i].type);

    a20_ch_message_t *msg = ch_msg_alloc(data, data_len, handles, handle_count);
    if (!msg) {
        for (uint32_t i = 0; i < handle_count; i++)
            a20_object_release(handles[i].object, handles[i].type);
        result = -A20_ERR_NO_MEMORY;
        goto out;
    }

    for (;;) {
        int r = ch_try_enqueue(ep, msg, defer_wake);
        if (r > 0) { result = A20_OK; goto out; }
        if (r < 0) { ch_msg_free(msg, 1); result = r; goto out; }
        if (flags & A20_MSG_NONBLOCK) {
            ch_msg_free(msg, 1);
            result = -A20_ERR_WOULD_BLOCK;
            goto out;
        }

        /* Queue full: sleep on the peer's wait queue as a sender. */
        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
        if (!token.task) { ch_msg_free(msg, 1); result = -A20_ERR_WOULD_BLOCK; goto out; }

        wait_queue_entry_t entry = {0};
        spin_lock(&g_ch_lock);
        a20_channel_ep_t *peer = ep->peer;
        if (peer && !refcount_inc_not_zero(&peer->refcount))
            peer = NULL;
        if (peer)
            spin_lock(&peer->lock);
        spin_unlock(&g_ch_lock);

        if (!peer) {
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            ch_msg_free(msg, 1);
            result = -A20_ERR_CANCELED;
            goto out;
        }
        if (!peer->peer_closed && peer->msg_count >= peer->msg_cap) {
            bool linked = wait_queue_link(&peer->waiters, &entry, token,
                                          A20_CH_WAIT_SEND);
            spin_unlock(&peer->lock);
            proc_wake_reason_t reason;
            if (linked)
                reason = proc_park_commit(token);
            else {
                (void)proc_park_cancel(token);
                reason = PROC_WAKE_CANCEL;
            }
            wait_queue_unlink(&peer->waiters, &entry);
            proc_park_finish(token);
            a20_channel_ep_release(peer);
            if (proc_wake_reason_is_task_interrupt(reason)) {
                ch_msg_free(msg, 1);
                result = -A20_ERR_INTERRUPTED;
                goto out;
            }
            continue;
        }
        /* Space appeared or the peer closed while preparing: retry the
         * fast path, which re-checks both conditions under peer->lock. */
        spin_unlock(&peer->lock);
        a20_channel_ep_release(peer);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
    }

out:
    a20_channel_ep_release(ep);
    return result;
}

int64_t a20_channel_send(a20_channel_ep_t *ep, const void *data, uint32_t data_len,
                         a20_ch_handle_info_t *handles, uint32_t handle_count,
                         struct a20_ht_internal *sender_ht, uint32_t flags)
{
    return a20_channel_send_dwc(ep, data, data_len, handles, handle_count,
                                sender_ht, flags, 0);
}

/*
 * a20_channel_recv_begin — wait until a message is available and return
 * with ep->lock HELD and the head message's handle count.  The caller then
 * reserves handle-table slots (docs/native-abi/05-ipc.md §2.6: no partial
 * delivery) and calls a20_channel_recv_finish, or a20_channel_recv_abort.
 */
int64_t a20_channel_recv_begin(a20_channel_ep_t *ep, uint32_t flags,
                               uint32_t *out_msg_data_len,
                               uint32_t *out_msg_handles)
{
    if (!ep) return -A20_ERR_BAD_HANDLE;
    if (flags & ~A20_MSG_NONBLOCK) return -A20_ERR_INVALID_ARGUMENT;

    /* Reference held until recv_finish/recv_abort (see a20_channel_send). */
    refcount_inc(&ep->refcount);

    for (;;) {
        spin_lock(&ep->lock);
        if (ep->msg_count > 0) {
            a20_ch_message_t *msg = ep->msg_head;
            if (msg->handle_count > 0) {
                a20_ch_handle_info_t *hbuf =
                    (a20_ch_handle_info_t *)(msg->data + msg->data_len);
                int tr = ch_check_recv_types(ep->chan_type, hbuf,
                                             msg->handle_count);
                if (tr < 0) {
                    /* A mismatched head message can never become deliverable;
                     * discard it so the endpoint is not permanently poisoned
                     * and its handle references are released. */
                    ep->msg_head = msg->next;
                    if (!ep->msg_head) ep->msg_tail = NULL;
                    ep->msg_count--;
                    ep->total_data -= msg->data_len;
                    wait_queue_wake_one(&ep->waiters, A20_CH_WAIT_SEND,
                                        PROC_WAKE_EVENT);
                    spin_unlock(&ep->lock);
                    ch_msg_free(msg, 1);
                    a20_channel_ep_release(ep);
                    return tr;
                }
            }
            if (out_msg_data_len)
                *out_msg_data_len = msg->data_len;
            if (out_msg_handles)
                *out_msg_handles = msg->handle_count;
            return A20_OK; /* ep->lock held */
        }
        if (ep->peer_closed) {
            spin_unlock(&ep->lock);
            a20_channel_ep_release(ep);
            return -A20_ERR_CANCELED;
        }
        if (flags & A20_MSG_NONBLOCK) {
            spin_unlock(&ep->lock);
            a20_channel_ep_release(ep);
            return -A20_ERR_WOULD_BLOCK;
        }
        spin_unlock(&ep->lock);

        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
        if (!token.task) {
            a20_channel_ep_release(ep);
            return -A20_ERR_WOULD_BLOCK;
        }

        wait_queue_entry_t entry = {0};
        spin_lock(&ep->lock);
        if (ep->msg_count == 0 && !ep->peer_closed) {
            bool linked = wait_queue_link(&ep->waiters, &entry, token,
                                          A20_CH_WAIT_RECV);
            spin_unlock(&ep->lock);
            proc_wake_reason_t reason;
            if (linked)
                reason = proc_park_commit(token);
            else {
                (void)proc_park_cancel(token);
                reason = PROC_WAKE_CANCEL;
            }
            wait_queue_unlink(&ep->waiters, &entry);
            proc_park_finish(token);
            if (proc_wake_reason_is_task_interrupt(reason)) {
                a20_channel_ep_release(ep);
                return -A20_ERR_INTERRUPTED;
            }
            continue;
        }
        spin_unlock(&ep->lock);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
    }
}

void a20_channel_recv_abort(a20_channel_ep_t *ep)
{
    spin_unlock(&ep->lock);
    a20_channel_ep_release(ep);
}

/*
 * a20_channel_recv_begin_donate — recv_begin with time-slice donation
 * (docs/hybrid-kernel/02-mainstream-plan.md M1): when the caller must
 * block for a reply and the peer has a task parked on RECV, the first
 * wait hands the CPU directly to that task instead of queueing in the
 * runqueue.  Later iterations (and all ineligible cases) use the normal
 * park path, so behavior is identical under races.
 */
int64_t a20_channel_recv_begin_donate(a20_channel_ep_t *ep, uint32_t flags,
                                      uint32_t *out_msg_data_len,
                                      uint32_t *out_msg_handles)
{
    if (!ep) return -A20_ERR_BAD_HANDLE;
    if (flags & ~A20_MSG_NONBLOCK) return -A20_ERR_INVALID_ARGUMENT;

    refcount_inc(&ep->refcount);
    int donate = 1;

    for (;;) {
        spin_lock(&ep->lock);
        if (ep->msg_count > 0) {
            a20_ch_message_t *msg = ep->msg_head;
            if (msg->handle_count > 0) {
                a20_ch_handle_info_t *hbuf =
                    (a20_ch_handle_info_t *)(msg->data + msg->data_len);
                int tr = ch_check_recv_types(ep->chan_type, hbuf,
                                             msg->handle_count);
                if (tr < 0) {
                    ep->msg_head = msg->next;
                    if (!ep->msg_head) ep->msg_tail = NULL;
                    ep->msg_count--;
                    ep->total_data -= msg->data_len;
                    wait_queue_wake_one(&ep->waiters, A20_CH_WAIT_SEND,
                                        PROC_WAKE_EVENT);
                    spin_unlock(&ep->lock);
                    ch_msg_free(msg, 1);
                    a20_channel_ep_release(ep);
                    return tr;
                }
            }
            if (out_msg_data_len)
                *out_msg_data_len = msg->data_len;
            if (out_msg_handles)
                *out_msg_handles = msg->handle_count;
            return A20_OK; /* ep->lock held */
        }
        if (ep->peer_closed) {
            spin_unlock(&ep->lock);
            a20_channel_ep_release(ep);
            return -A20_ERR_CANCELED;
        }
        if (flags & A20_MSG_NONBLOCK) {
            spin_unlock(&ep->lock);
            a20_channel_ep_release(ep);
            return -A20_ERR_WOULD_BLOCK;
        }
        spin_unlock(&ep->lock);

        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
        if (!token.task) {
            a20_channel_ep_release(ep);
            return -A20_ERR_WOULD_BLOCK;
        }

        wait_queue_entry_t entry = {0};
        spin_lock(&ep->lock);
        if (ep->msg_count == 0 && !ep->peer_closed) {
            bool linked = wait_queue_link(&ep->waiters, &entry, token,
                                          A20_CH_WAIT_RECV);
            spin_unlock(&ep->lock);
            proc_wake_reason_t reason;
            if (linked) {
                task_t *target = NULL;
                if (donate) {
                    spin_lock(&g_ch_lock);
                    a20_channel_ep_t *peer = ep->peer;
                    if (peer && !peer->peer_closed)
                        target = wait_queue_peek_key(&peer->waiters,
                                                     A20_CH_WAIT_RECV);
                    spin_unlock(&g_ch_lock);
                    donate = 0;
                }
                if (target) {
                    reason = proc_park_commit_donate(token, target);
                    proc_put(target);
                } else {
                    /* Donation impossible (server running, remote CPU, or
                     * already woken): deliver the wake deferred by the
                     * channel_call send stage, then park normally. */
                    spin_lock(&g_ch_lock);
                    a20_channel_ep_t *peer = ep->peer;
                    if (peer && !peer->peer_closed)
                        wait_queue_wake_one(&peer->waiters,
                                            A20_CH_WAIT_RECV,
                                            PROC_WAKE_EVENT);
                    spin_unlock(&g_ch_lock);
                    reason = proc_park_commit(token);
                }
            } else {
                (void)proc_park_cancel(token);
                reason = PROC_WAKE_CANCEL;
            }
            wait_queue_unlink(&ep->waiters, &entry);
            proc_park_finish(token);
            if (proc_wake_reason_is_task_interrupt(reason)) {
                a20_channel_ep_release(ep);
                return -A20_ERR_INTERRUPTED;
            }
            continue;
        }
        spin_unlock(&ep->lock);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
    }
}

/*
 * a20_channel_recv_finish — dequeue the head message (ep->lock held from
 * recv_begin) and copy out data + handle infos.  On success the handle
 * references move to the receiver; the caller installs them via the
 * reserved handle-table slots.
 */
int64_t a20_channel_recv_finish(a20_channel_ep_t *ep, void *data, uint32_t *data_len,
                                a20_ch_handle_info_t *handles, uint32_t *handle_count)
{
    a20_ch_message_t *msg = ep->msg_head;
    if (!msg) {
        spin_unlock(&ep->lock);
        a20_channel_ep_release(ep); /* drop the recv_begin reference */
        return -A20_ERR_WOULD_BLOCK;
    }

    uint32_t out_len = msg->data_len;
    if (out_len > 0 && (!data || !data_len || *data_len < out_len)) {
        spin_unlock(&ep->lock);
        a20_channel_ep_release(ep);
        return -A20_ERR_NO_SPACE;
    }

    uint32_t hc = msg->handle_count;
    if (hc > 0 && (!handles || !handle_count || *handle_count < hc)) {
        spin_unlock(&ep->lock);
        a20_channel_ep_release(ep);
        return -A20_ERR_NO_SPACE;
    }

    if (hc > 0) {
        a20_ch_handle_info_t *hbuf =
            (a20_ch_handle_info_t *)(msg->data + msg->data_len);
        int tr = ch_check_recv_types(ep->chan_type, hbuf, hc);
        if (tr < 0) {
            spin_unlock(&ep->lock);
            a20_channel_ep_release(ep);
            return tr;
        }
    }

    ep->msg_head = msg->next;
    if (!ep->msg_head) ep->msg_tail = NULL;
    ep->msg_count--;
    ep->total_data -= msg->data_len;

    /* Space freed: wake one sender blocked on this endpoint's queue. */
    wait_queue_wake_one(&ep->waiters, A20_CH_WAIT_SEND, PROC_WAKE_EVENT);
    spin_unlock(&ep->lock);

    if (out_len > 0)
        memcpy(data, msg->data, out_len);
    if (data_len)
        *data_len = out_len;

    if (hc > 0) {
        a20_ch_handle_info_t *hbuf =
            (a20_ch_handle_info_t *)(msg->data + msg->data_len);
        memcpy(handles, hbuf, hc * sizeof(a20_ch_handle_info_t));
    }
    if (handle_count) *handle_count = hc;

    ch_msg_free(msg, 0); /* handle refs transfer to the receiver */
    a20_channel_ep_release(ep); /* drop the recv_begin reference */
    return A20_OK;
}

int64_t a20_channel_recv(a20_channel_ep_t *ep, void *data, uint32_t *data_len,
                         a20_ch_handle_info_t *handles, uint32_t *handle_count,
                         struct a20_ht_internal *receiver_ht, uint32_t flags)
{
    (void)receiver_ht; /* slot reservation is the syscall layer's job */
    uint32_t msg_data_len = 0;
    uint32_t msg_handles = 0;
    int64_t r = a20_channel_recv_begin(ep, flags, &msg_data_len, &msg_handles);
    if (r < 0) return r;
    if ((msg_data_len > 0 && (!data || !data_len || *data_len < msg_data_len)) ||
        (msg_handles > 0 && (!handles || !handle_count || *handle_count < msg_handles))) {
        a20_channel_recv_abort(ep);
        return -A20_ERR_NO_SPACE;
    }
    return a20_channel_recv_finish(ep, data, data_len, handles, handle_count);
}

void a20_channel_ep_release(a20_channel_ep_t *ep)
{
    if (!ep) return;
    if (!refcount_dec_and_test(&ep->refcount)) return;

    spin_lock(&g_ch_lock);
    spin_lock(&ep->lock);
    a20_channel_ep_t *peer = ep->peer;
    if (peer) {
        spin_lock(&peer->lock);
        peer->peer_closed = 1;
        spin_unlock(&peer->lock);
        if (peer->peer == ep)
            peer->peer = NULL;
        ep->peer = NULL;
        wait_queue_wake_all(&peer->waiters, 0, PROC_WAKE_EVENT);
        a20_event_notify(peer, A20_OBJ_CHANNEL_ENDPOINT,
                         A20_EVENT_PEER_CLOSED, 0, 0);
    }
    /* Watchers of the dying endpoint see one final CLOSED event, then the
     * watch entries keyed by this pointer are removed. */
    a20_event_notify(ep, A20_OBJ_CHANNEL_ENDPOINT, A20_EVENT_CLOSED, 0, 0);
    a20_eventq_on_object_destroy(ep, A20_OBJ_CHANNEL_ENDPOINT);
    spin_unlock(&ep->lock);
    spin_unlock(&g_ch_lock);

    a20_ch_message_t *msg = ep->msg_head;
    while (msg) {
        a20_ch_message_t *next = msg->next;
        ch_msg_free(msg, 1);
        msg = next;
    }

    kfree(ep);
}
