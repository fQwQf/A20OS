/*
 * A20OS Native ABI — Channel endpoints.
 * Design reference: docs/zh/native-abi/05-ipc.md §2
 * Design inspiration: Zircon (Fuchsia) Channel handle-transfer semantics
 * and seL4 Endpoint capability transfer; see docs/zh/ACKNOWLEDGMENTS.md §3.
 *
 * CH_PEER_TEARDOWN_PROTOCOL:
 * Both endpoints are freed independently once their refcount hits zero, so
 * all peer dereferences are serialized by g_ch_lock: an endpoint reads its
 * peer pointer under g_ch_lock, then either takes peer->lock before dropping
 * g_ch_lock, or -- on the paths that only read peer state -- takes a peer
 * reference under g_ch_lock and uses peer->lock after dropping it, which the
 * send and park paths already do.  Release takes g_ch_lock and then peer->lock
 * before kfree, which guarantees a sender holding peer->lock never touches
 * freed memory.
 */
#include "core/types.h"
#include "core/bootargs.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/sync.h"
#include "mm/slab.h"
#include "sys/usercopy.h"
#include "ipc/ipc.h"
#include "proc/proc.h"
#include "core/panic.h"
#include "core/timer.h"
#include "core/cpu.h"
#include "ipc/objstats.h"

extern int64_t a20_rust_channel_recv_finish(a20_channel_ep_t *ep, void *data,
                                             uint32_t *data_len,
                                             a20_ch_handle_info_t *handles,
                                             uint32_t *handle_count);
extern void a20_rust_channel_recv_abort(a20_channel_ep_t *ep);
extern int64_t a20_rust_channel_recv_begin(a20_channel_ep_t *ep, uint32_t flags,
                                            uint32_t *out_msg_data_len,
                                            uint32_t *out_msg_handles);
extern int64_t a20_rust_channel_recv_probe_locked(a20_channel_ep_t *ep,
                                                   uint32_t *out_msg_data_len,
                                                   uint32_t *out_msg_handles);
extern void a20_rust_channel_peer_shutdown(a20_channel_ep_t *ep);
extern void a20_rust_channel_ep_finalize(a20_channel_ep_t *ep);

extern void a20_object_ref(void *object, uint16_t type);
extern void a20_object_release(void *object, uint16_t type);

static spinlock_t g_ch_lock = SPINLOCK_INIT;

/* ---- crash trace ring (diagnostics) ---- */
#define CH_TRACE_ENTRIES 64
struct ch_trace_ent {
    uint64_t tick;
    int      pid;
    uint32_t op;      /* 1=alloc 2=enqueue 3=send-ok 4=send-fail 5=recv-begin
                       6=recv-finish 7=recv-fail 8=free 9=ht-destroy */
    uint32_t len;
    void    *p1;      /* msg or ht */
    void    *p2;      /* kdata or entries */
    uint32_t meta;
};
static struct ch_trace_ent g_ch_trace[CH_TRACE_ENTRIES];
static uint32_t g_ch_trace_idx;
static int g_ch_trace_ready;
static int g_ch_trace_on;

/*
 * CHTRACE_BOOTARG
 *
 * The ring exists to explain a crash, and a20_channel_trace_dump() is not
 * reached from a hot path, so nothing on the send or receive side ever reads
 * what it collects.  Recording unconditionally costs every send and every
 * receive an atomic bump on one global, a timer read and two proc_current()
 * lookups.  Recording is therefore opt-in through "chtrace" on the kernel
 * command line, the way the syscall trace is gated on "trace=".
 */
static void ch_trace_init(void)
{
    const char *args = bootargs_get();
    g_ch_trace_ready = 1;
    if (!args || !strstr(args, "chtrace="))
        return;
    g_ch_trace_on = 1;
    printf("[CHTRACE] enabled\n");
}

void a20_channel_trace(uint32_t op, uint32_t len, void *p1, void *p2, uint32_t meta)
{
    if (!g_ch_trace_ready)
        ch_trace_init();
    if (!g_ch_trace_on)
        return;
    uint32_t i = __atomic_fetch_add(&g_ch_trace_idx, 1, __ATOMIC_RELAXED) %
                 CH_TRACE_ENTRIES;
    g_ch_trace[i].tick = timer_get_ticks();
    g_ch_trace[i].pid  = proc_current() ? proc_current()->pid : -1;
    g_ch_trace[i].op   = op;
    g_ch_trace[i].len  = len;
    g_ch_trace[i].p1   = p1;
    g_ch_trace[i].p2   = p2;
    g_ch_trace[i].meta = meta;
}
void a20_channel_trace_dump(void)
{
    if (!g_ch_trace_ready)
        ch_trace_init();
    if (!g_ch_trace_on)
        return;
    uint32_t n = g_ch_trace_idx;
    uint32_t start = n > CH_TRACE_ENTRIES ? n - CH_TRACE_ENTRIES : 0;
    for (uint32_t i = start; i < n; i++) {
        struct ch_trace_ent *e = &g_ch_trace[i % CH_TRACE_ENTRIES];
        printf("[CHTRACE] tick=%lu pid=%d op=%u len=%u p1=%p p2=%p meta=%u\n",
               (unsigned long)e->tick, e->pid, e->op, e->len,
               e->p1, e->p2, e->meta);
    }
}

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
    a20_channel_trace(8, release_refs, msg, NULL, 0);
    if (msg->data_len == 0xDEADBEEF) {
        printf("[CH-MSG-DOUBLE-FREE] msg=%p release_refs=%d cpu=%u\n",
               (void *)msg, release_refs, cpu_current_id());
        panic("channel message double free");
    }    if (release_refs && msg->handle_count > 0) {
        a20_ch_handle_info_t *hbuf =
            (a20_ch_handle_info_t *)(msg->data + msg->data_len);
        for (uint32_t i = 0; i < msg->handle_count; i++)
            a20_object_release(hbuf[i].object, hbuf[i].type);
    }
    msg->data_len = 0xDEADBEEF;
    kfree(msg);
}

/*
 * ch_check_send_types — verify handles are allowed by the channel's
 * send_handle_types bitmask (docs/zh/native-abi/05-ipc.md §2.3).
 */

/*
 * ch_check_recv_types — verify received handles are allowed by the
 * channel's recv_handle_types bitmask (docs/zh/native-abi/05-ipc.md §2.3).
 * Without this check a receiver could be forced to accept handle types
 * it did not agree to handle, breaking the typed-channel contract.
 */

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

    /* Typed channel (docs/zh/native-abi/05-ipc.md §2.3): each endpoint keeps
     * its own copy of the type signature so there is no shared-lifetime
     * problem between the two endpoints. */
    if (type) {
        ep0->chan_type_storage = *type;
        ep1->chan_type_storage = *type;
        ep0->chan_type = &ep0->chan_type_storage;
        ep1->chan_type = &ep1->chan_type_storage;
    }

    a20_objstat_add(&g_a20_objstats.channel_eps, 2);
    return ep0;
}

/*
 * ch_try_enqueue — one non-blocking enqueue attempt.
 * Returns 1 on success, 0 when the peer queue is full, < 0 on error.
 * defer_wake skips the RECV wait-queue wake (channel_call donation path);
 * the caller then owns delivering the wake or a direct handoff.
 */

/* Mechanism bridges for channel_send.rs. */
void a20_channel_send_limits(a20_channel_ep_t *ep, uint32_t *max_data,
                             uint32_t *max_handles, uint32_t *send_types,
                             int *typed)
{
    const a20_channel_type_t *ct = ep->chan_type;
    *typed = ct != NULL;
    *max_data = ct ? ct->max_data_size : 0;
    *max_handles = ct ? ct->max_handles : 0;
    *send_types = ct ? ct->send_handle_types : 0;
}
void a20_channel_send_hold(a20_channel_ep_t *ep) { refcount_inc(&ep->refcount); }
a20_ch_message_t *a20_channel_send_alloc(const void *data, uint32_t data_len,
                                         const a20_ch_handle_info_t *handles,
                                         uint32_t handle_count)
{ return ch_msg_alloc(data, data_len, (a20_ch_handle_info_t *)handles, handle_count); }
/* Queue locking/ref pins and wake primitives are mechanisms; Rust owns the
 * capacity decision and FIFO link/count/byte mutations. */
typedef struct a20_channel_queue_view {
    a20_ch_message_t **head;
    a20_ch_message_t **tail;
    uint32_t *count;
    const uint32_t *capacity;
    uint32_t *total_data;
    const int *peer_closed;
} a20_channel_queue_view_t;

a20_channel_ep_t *a20_channel_send_lock_peer(a20_channel_ep_t *ep)
{
    spin_lock(&g_ch_lock);
    a20_channel_ep_t *peer = ep->peer;
    if (peer && !refcount_inc_not_zero(&peer->refcount))
        peer = NULL;
    if (peer)
        spin_lock(&peer->lock);
    spin_unlock(&g_ch_lock);
    return peer;
}
void a20_channel_send_unlock_peer(a20_channel_ep_t *peer)
{
    spin_unlock(&peer->lock);
    a20_channel_ep_release(peer);
}
void a20_channel_queue_view(a20_channel_ep_t *ep, a20_channel_queue_view_t *view)
{
    view->head = &ep->msg_head;
    view->tail = &ep->msg_tail;
    view->count = &ep->msg_count;
    view->capacity = &ep->msg_cap;
    view->total_data = &ep->total_data;
    view->peer_closed = &ep->peer_closed;
}
void a20_channel_send_notify(a20_channel_ep_t *peer, int defer_wake)
{
    if (!defer_wake)
        wait_queue_wake_one(&peer->waiters, A20_CH_WAIT_RECV, PROC_WAKE_EVENT);
    a20_event_notify(peer, A20_OBJ_CHANNEL_ENDPOINT,
                     A20_EVENT_MESSAGE_READY, 0, 0);
}
void a20_channel_send_free(a20_ch_message_t *msg, int release_refs)
{ ch_msg_free(msg, release_refs); }

/* Queue-space parking is a wait-queue mechanism; Rust owns the retry and
 * message/ref rollback policy around its result. */
int64_t a20_channel_send_wait_space(a20_channel_ep_t *ep)
{
    proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
    if (!token.task)
        return -A20_ERR_WOULD_BLOCK;

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
        return -A20_ERR_CANCELED;
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
        if (proc_wake_reason_is_task_interrupt(reason))
            return -A20_ERR_INTERRUPTED;
        return A20_OK;
    }

    spin_unlock(&peer->lock);
    a20_channel_ep_release(peer);
    (void)proc_park_cancel(token);
    proc_park_finish(token);
    return A20_OK;
}

extern int64_t a20_rust_channel_send_dwc(a20_channel_ep_t *ep,
                                         const void *data, uint32_t data_len,
                                         a20_ch_handle_info_t *handles,
                                         uint32_t handle_count,
                                         struct a20_ht_internal *sender_ht,
                                         uint32_t flags, int defer_wake);
int64_t a20_channel_send_dwc(a20_channel_ep_t *ep, const void *data, uint32_t data_len,
                             a20_ch_handle_info_t *handles, uint32_t handle_count,
                             struct a20_ht_internal *sender_ht, uint32_t flags,
                             int defer_wake)
{
    return a20_rust_channel_send_dwc(ep, data, data_len, handles,
                                     handle_count, sender_ht, flags,
                                     defer_wake);
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
 * reserves handle-table slots (docs/zh/native-abi/05-ipc.md §2.6: no partial
 * delivery) and calls a20_channel_recv_finish, or a20_channel_recv_abort.
 */
int64_t a20_channel_recv_begin(a20_channel_ep_t *ep, uint32_t flags,
                               uint32_t *out_msg_data_len,
                               uint32_t *out_msg_handles)
{ return a20_rust_channel_recv_begin(ep, flags, out_msg_data_len, out_msg_handles); }

void a20_channel_recv_abort(a20_channel_ep_t *ep)
{ a20_rust_channel_recv_abort(ep); }

/* The same locked view is used by Rust receive dequeue policy. */
void a20_channel_recv_queue_view(a20_channel_ep_t *ep, a20_channel_queue_view_t *view)
{ a20_channel_queue_view(ep, view); }
void a20_channel_recv_lock(a20_channel_ep_t *ep) { spin_lock(&ep->lock); }
void a20_channel_recv_hold(a20_channel_ep_t *ep) { refcount_inc(&ep->refcount); }
void a20_channel_recv_free_msg_refs(a20_ch_message_t *msg) { ch_msg_free(msg, 1); }
void a20_channel_recv_trace_begin(uint32_t len, a20_ch_message_t *msg,
                                  uint32_t count)
{ a20_channel_trace(5, len, msg, NULL, count); }
int64_t a20_channel_recv_wait(a20_channel_ep_t *ep)
{
    proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
    if (!token.task) return -A20_ERR_WOULD_BLOCK;
    wait_queue_entry_t entry = {0};
    spin_lock(&ep->lock);
    if (ep->msg_count == 0 && !ep->peer_closed) {
        bool linked = wait_queue_link(&ep->waiters, &entry, token, A20_CH_WAIT_RECV);
        spin_unlock(&ep->lock);
        proc_wake_reason_t reason;
        if (linked) reason = proc_park_commit(token);
        else { (void)proc_park_cancel(token); reason = PROC_WAKE_CANCEL; }
        wait_queue_unlink(&ep->waiters, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason))
            return -A20_ERR_INTERRUPTED;
        return A20_OK;
    }
    spin_unlock(&ep->lock);
    (void)proc_park_cancel(token);
    proc_park_finish(token);
    return A20_OK;
}
void a20_channel_recv_contract(a20_channel_ep_t *ep, uint32_t *types, int *typed)
{
    *typed = ep->chan_type != NULL;
    *types = ep->chan_type ? ep->chan_type->recv_handle_types : 0;
}
/* The scheduler donation loop probes the queue on every rendezvous. Keep the
 * common empty/no-handles cases in this C lock context: they need only direct
 * fields and tracing, while the typed-handle validation and rollback-sensitive
 * receive policy remain in Rust. */
int a20_channel_recv_probe_simple_locked(a20_channel_ep_t *ep,
                                         uint32_t *out_msg_data_len,
                                         uint32_t *out_msg_handles,
                                         int64_t *out_probe)
{
    if (ep->msg_count == 0) {
        *out_probe = 1;
        return 1;
    }
    a20_ch_message_t *msg = ep->msg_head;
    if (msg->handle_count != 0)
        return 0;
    if (out_msg_data_len)
        *out_msg_data_len = msg->data_len;
    if (out_msg_handles)
        *out_msg_handles = 0;
    a20_channel_trace(5, msg->data_len, msg, NULL, ep->msg_count);
    *out_probe = A20_OK;
    return 1;
}
void a20_channel_recv_copy(a20_ch_message_t *msg, void *data,
                           a20_ch_handle_info_t *handles)
{
    if (msg->data_len) memcpy(data, msg->data, msg->data_len);
    if (msg->handle_count)
        memcpy(handles, msg->data + msg->data_len,
               msg->handle_count * sizeof(a20_ch_handle_info_t));
}
void a20_channel_recv_wake_sender(a20_channel_ep_t *ep)
{ wait_queue_wake_one(&ep->waiters, A20_CH_WAIT_SEND, PROC_WAKE_EVENT); }
void a20_channel_recv_unlock(a20_channel_ep_t *ep) { spin_unlock(&ep->lock); }
void a20_channel_recv_release_ep(a20_channel_ep_t *ep) { a20_channel_ep_release(ep); }
void a20_channel_recv_free_msg(a20_ch_message_t *msg) { ch_msg_free(msg, 0); }
void a20_channel_recv_trace(uint32_t len, a20_ch_message_t *msg, void *data,
                            uint32_t handles)
{ a20_channel_trace(6, len, msg, data, handles); }

#ifdef CONFIG_CHANNEL_DONATE_TEST
static int g_ch_donate_test_injected;
static int g_ch_donate_test_fallback_armed;
static int g_ch_donate_test_queued_reply_armed;
#define CH_DONATE_TEST_MAGIC UINT64_C(0x31454b41574f4e44) /* "DNOWAKE1" */
#define CH_DONATE_TEST_QUEUED_REPLY_MAGIC UINT64_C(0x3130594c50455251) /* "QREPLY01" */

/* Is a receiver parked on @peer?  wait_queue_peek_key() hands back a
 * referenced task which stays valid without the queue lock; this check only
 * needs the answer, so it drops that reference immediately instead of keeping
 * a task pointer alive across the rest of the fixture. */
static bool ch_peer_has_receiver(a20_channel_ep_t *peer)
{
    task_t *task = wait_queue_peek_key(&peer->waiters, A20_CH_WAIT_RECV);
    if (!task)
        return false;
    proc_put(task);
    return true;
}

static int ch_donate_test_request_matches(a20_channel_ep_t *ep)
{
    int matches = 0;
    spin_lock(&g_ch_lock);
    a20_channel_ep_t *peer = ep->peer;
    if (peer) {
        spin_lock(&peer->lock);
        a20_ch_message_t *msg = peer->msg_head;
        if (msg && msg->data_len >= sizeof(uint64_t)) {
            uint64_t magic;
            memcpy(&magic, msg->data, sizeof(magic));
            matches = magic == CH_DONATE_TEST_MAGIC;
        }
        spin_unlock(&peer->lock);
    }
    spin_unlock(&g_ch_lock);
    return matches;
}

/* The dedicated queued-reply fixture sends QREPLY01 while the receiver is
 * already parked, then channel_call observes an older reply on its own
 * endpoint. Require the actual peer waiter here so the test cannot pass from
 * a scheduling race where the receiver simply runs later. */
static int ch_donate_test_arm_queued_reply(a20_channel_ep_t *ep)
{
    int matches = 0;
    int has_waiter = 0;
    spin_lock(&g_ch_lock);
    a20_channel_ep_t *peer = ep->peer;
    if (peer && !peer->peer_closed) {
        spin_lock(&peer->lock);
        a20_ch_message_t *msg = peer->msg_head;
        if (msg && msg->data_len >= sizeof(uint64_t)) {
            uint64_t magic;
            memcpy(&magic, msg->data, sizeof(magic));
            matches = magic == CH_DONATE_TEST_QUEUED_REPLY_MAGIC;
        }
        spin_unlock(&peer->lock);
        if (matches)
            has_waiter = ch_peer_has_receiver(peer);
    }
    spin_unlock(&g_ch_lock);
    if (matches && !has_waiter)
        panic("channel queued-reply test receiver was not parked");
    if (matches)
        __atomic_store_n(&g_ch_donate_test_queued_reply_armed, 1,
                         __ATOMIC_RELEASE);
    return matches;
}
#endif

/* Wake the peer whose request/reply wake was deferred for a possible direct
 * handoff. Called by proc_park_commit_donate only on a no-handoff path, with
 * scheduler locks released and before the caller can block in sched(). The
 * recv_begin reference keeps ep alive while this callback reacquires the
 * channel lock and resolves its current peer. */
static void ch_donate_fallback_wake(void *context)
{
    a20_channel_ep_t *ep = (a20_channel_ep_t *)context;
    if (!ep)
        return;

    spin_lock(&g_ch_lock);
    a20_channel_ep_t *peer = ep->peer;
#ifdef CONFIG_CHANNEL_DONATE_TEST
    unsigned woken = 0;
    if (peer && !peer->peer_closed)
        woken = wait_queue_wake_one(&peer->waiters, A20_CH_WAIT_RECV,
                                    PROC_WAKE_EVENT);
#else
    if (peer && !peer->peer_closed)
        wait_queue_wake_one(&peer->waiters, A20_CH_WAIT_RECV,
                            PROC_WAKE_EVENT);
#endif
    spin_unlock(&g_ch_lock);
#ifdef CONFIG_CHANNEL_DONATE_TEST
    if (__atomic_exchange_n(&g_ch_donate_test_queued_reply_armed, 0,
                            __ATOMIC_ACQ_REL)) {
        if (woken == 0)
            panic("channel queued-reply test wake did not reach receiver");
        printf("[CH_DONATE_TEST] queued-reply-fallback\n");
    }
    if (__atomic_exchange_n(&g_ch_donate_test_fallback_armed, 0,
                            __ATOMIC_ACQ_REL))
        printf("[CH_DONATE_TEST] fallback-wake\n");
#endif
}

/*
 * a20_channel_recv_begin_donate — recv_begin with time-slice donation
 * (docs/zh/hybrid-kernel/02-mainstream-plan.md M1): when the caller must
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
    /* Try time-slice donation on every wait; proc_park_commit_donate() only
     * donates when the target is fully parked on this CPU and falls back to
     * the normal park path otherwise. */
    int donate = 1;
    /* channel_call deferred the request wake when entering this path. Own
     * that wake obligation until we either attempt commit_donate (which calls
     * the fallback callback on every no-handoff path) or resolve it here. */
    int deferred_wake_pending = donate;

    for (;;) {
        spin_lock(&ep->lock);
        /* Rust inspects/dequeues the head under this lock. The donated wait
         * path keeps its scheduler handoff policy here. Empty/simple messages
         * avoid an FFI round trip; handle-bearing messages use Rust's full
         * validation path. */
        /* Preserve the C path's queued-reply ordering: wake the parked peer
         * before probing the queued message. A Rust probe error drops the
         * lease's endpoint reference, so no fallback callback may use ep
         * after that probe has returned an error. */
        if (ep->msg_count > 0 && deferred_wake_pending) {
            spin_unlock(&ep->lock);
#ifdef CONFIG_CHANNEL_DONATE_TEST
            (void)ch_donate_test_arm_queued_reply(ep);
#endif
            ch_donate_fallback_wake(ep);
            deferred_wake_pending = 0;
            continue;
        }
        int64_t probe;
        if (!a20_channel_recv_probe_simple_locked(
                ep, out_msg_data_len, out_msg_handles, &probe))
            probe = a20_rust_channel_recv_probe_locked(
                ep, out_msg_data_len, out_msg_handles);
        if (probe == A20_OK) {
            if (deferred_wake_pending) {
                spin_unlock(&ep->lock);
#ifdef CONFIG_CHANNEL_DONATE_TEST
                (void)ch_donate_test_arm_queued_reply(ep);
#endif
                ch_donate_fallback_wake(ep);
                deferred_wake_pending = 0;
                continue;
            }
            return A20_OK; /* ep->lock held */
        }
        if (probe < 0) {
            if (deferred_wake_pending)
                ch_donate_fallback_wake(ep);
            return probe;
        }
        if (ep->peer_closed) {
            spin_unlock(&ep->lock);
            if (deferred_wake_pending) {
                ch_donate_fallback_wake(ep);
                deferred_wake_pending = 0;
            }
            a20_channel_ep_release(ep);
            return -A20_ERR_CANCELED;
        }
        if (flags & A20_MSG_NONBLOCK) {
            spin_unlock(&ep->lock);
            if (deferred_wake_pending) {
                ch_donate_fallback_wake(ep);
                deferred_wake_pending = 0;
            }
            a20_channel_ep_release(ep);
            return -A20_ERR_WOULD_BLOCK;
        }
        spin_unlock(&ep->lock);

        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
        if (!token.task) {
            if (deferred_wake_pending) {
                ch_donate_fallback_wake(ep);
                deferred_wake_pending = 0;
            }
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
                int donate_local_parked = 0;
                if (target) {
                    /* A deferred channel_call wake may see a receiver parked
                     * on another CPU. Direct donation only handles a parked
                     * task on this CPU; otherwise proc_park_commit_donate()
                     * falls back to sched() without waking the remote waiter.
                     * Verify the exact direct-switch preconditions under the
                     * target lock, and use the normal wait-queue wake path for
                     * PREPARING, remote, or otherwise ineligible receivers. */
                    uint64_t target_flags =
                        spin_lock_irqsave(&target->park_lock);
                    donate_local_parked =
                        target->park_state == PROC_PARK_PARKED &&
                        target->state == PROC_BLOCKED && !target->on_cpu &&
                        !__atomic_load_n(&target->on_rq, __ATOMIC_RELAXED) &&
                        !__atomic_load_n(&target->dispatching,
                                         __ATOMIC_RELAXED) &&
                        __atomic_load_n(&target->cpu_id, __ATOMIC_RELAXED) ==
                            cpu_current_id();
                    spin_unlock_irqrestore(&target->park_lock, target_flags);
                }
                if (target && donate_local_parked) {
#ifdef CONFIG_CHANNEL_DONATE_TEST
                    int expected = 0;
                    if (ch_donate_test_request_matches(ep) &&
                        __atomic_compare_exchange_n(
                            &g_ch_donate_test_injected, &expected, 1, 0,
                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                        if (!proc_try_wake(proc_current(), token.seq,
                                           PROC_WAKE_SIGNAL))
                            panic("channel donate test failed to pre-wake donor");
                        __atomic_store_n(&g_ch_donate_test_fallback_armed, 1,
                                         __ATOMIC_RELEASE);
                        printf("[CH_DONATE_TEST] donor-prewoken\n");
                    }
#endif
                    deferred_wake_pending = 0;
                    reason = proc_park_commit_donate(
                        token, target, ch_donate_fallback_wake, ep);
                    proc_put(target);
                } else {
                    /* The send stage deferred its receiver wake for possible
                     * same-CPU donation. Deliver it before ordinary commit. */
                    if (target)
                        proc_put(target);
                    if (deferred_wake_pending) {
                        ch_donate_fallback_wake(ep);
                        deferred_wake_pending = 0;
                    }
                    reason = proc_park_commit(token);
                }
            } else {
                if (deferred_wake_pending) {
                    ch_donate_fallback_wake(ep);
                    deferred_wake_pending = 0;
                }
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
int64_t a20_channel_recv_finish(a20_channel_ep_t *ep, void *data,
                                uint32_t *data_len,
                                a20_ch_handle_info_t *handles,
                                uint32_t *handle_count)
{
    a20_ch_message_t *msg = ep->msg_head;
    if (msg && msg->handle_count == 0 &&
        (msg->data_len == 0 ||
         (data && data_len && *data_len >= msg->data_len))) {
        uint32_t out_len = msg->data_len;
        ep->msg_head = msg->next;
        if (!ep->msg_head) ep->msg_tail = NULL;
        ep->msg_count--;
        ep->total_data -= out_len;
        wait_queue_wake_one(&ep->waiters, A20_CH_WAIT_SEND, PROC_WAKE_EVENT);
        spin_unlock(&ep->lock);
        if (out_len > 0)
            memcpy(data, msg->data, out_len);
        if (data_len)
            *data_len = out_len;
        if (handle_count)
            *handle_count = 0;
        a20_channel_trace(6, out_len, msg, data, 0);
        ch_msg_free(msg, 0);
        a20_channel_ep_release(ep);
        return A20_OK;
    }
    /* Keep empty/NO_SPACE/handle-bearing and all typed/error semantics in the
     * Rust receive transaction. This direct C path only consumes a simple
     * message after Rust has already transferred the lock and endpoint pin. */
    return a20_rust_channel_recv_finish(ep, data, data_len, handles, handle_count);
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

int a20_channel_readable(a20_channel_ep_t *ep)
{
    if (!ep) return 0;
    spin_lock(&ep->lock);
    int r = ep->msg_count > 0 || ep->peer_closed;
    spin_unlock(&ep->lock);
    return r;
}

int a20_channel_writable(a20_channel_ep_t *ep)
{
    if (!ep)
        return 0;

    /* Sending on an endpoint enqueues into its peer.  Readiness therefore
     * depends on the peer queue, not this endpoint's inbound queue.  Only a
     * reference is taken under g_ch_lock; the queue is inspected after the
     * global lock is dropped, so a poll on one endpoint does not serialise
     * against a send on any other. */
    spin_lock(&g_ch_lock);
    a20_channel_ep_t *peer = ep->peer;
    if (peer && !refcount_inc_not_zero(&peer->refcount))
        peer = NULL;
    spin_unlock(&g_ch_lock);

    if (!peer)
        return 0;
    spin_lock(&peer->lock);
    int r = !peer->peer_closed && peer->msg_count < peer->msg_cap;
    spin_unlock(&peer->lock);
    a20_channel_ep_release(peer);
    return r;
}

int a20_channel_peer_closed(a20_channel_ep_t *ep)
{
    if (!ep) return 0;
    spin_lock(&ep->lock);
    int r = ep->peer_closed;
    spin_unlock(&ep->lock);
    return r;
}

int a20_channel_ep_set_peer_close_hook(a20_channel_ep_t *ep,
                                       void (*hook)(void *),
                                       void (*ctx_ref)(void *),
                                       void (*ctx_put)(void *), void *ctx)
{
    if (!ep || !hook || !ctx_ref || !ctx_put || !ctx)
        return -A20_ERR_INVALID_ARGUMENT;
    spin_lock(&g_ch_lock);
    spin_lock(&ep->lock);
    if (ep->peer_close_hook || ep->peer_closed) {
        spin_unlock(&ep->lock);
        spin_unlock(&g_ch_lock);
        return -A20_ERR_BUSY;
    }
    ctx_ref(ctx); /* endpoint-owned reference */
    ep->peer_close_hook = hook;
    ep->peer_close_ref = ctx_ref;
    ep->peer_close_put = ctx_put;
    ep->peer_close_ctx = ctx;
    spin_unlock(&ep->lock);
    spin_unlock(&g_ch_lock);
    return A20_OK;
}

void a20_channel_ep_clear_peer_close_hook(a20_channel_ep_t *ep)
{
    if (!ep) return;
    void (*ctx_put)(void *) = NULL;
    void *ctx = NULL;
    spin_lock(&g_ch_lock);
    spin_lock(&ep->lock);
    ctx_put = ep->peer_close_put;
    ctx = ep->peer_close_ctx;
    ep->peer_close_hook = NULL;
    ep->peer_close_ref = NULL;
    ep->peer_close_put = NULL;
    ep->peer_close_ctx = NULL;
    spin_unlock(&ep->lock);
    spin_unlock(&g_ch_lock);
    if (ctx_put) ctx_put(ctx);
}

typedef struct a20_channel_close_locks {
    a20_channel_ep_t *ep;
    a20_channel_ep_t *peer;
    int peer_locked;
} a20_channel_close_locks_t;
typedef struct a20_channel_close_view {
    a20_channel_ep_t *peer;
    a20_channel_ep_t **peer_link;
    a20_channel_ep_t **peer_backlink;
    int *peer_closed;
    void (**hook)(void *);
    void (**ctx_ref)(void *);
    void (**ctx_put)(void *);
    void **hook_ctx;
    a20_ch_message_t **message_head;
} a20_channel_close_view_t;
void a20_channel_close_lock(a20_channel_ep_t *ep,
                            a20_channel_close_locks_t *locks)
{
    spin_lock(&g_ch_lock);
    spin_lock(&ep->lock);
    locks->ep = ep;
    locks->peer = ep->peer;
    locks->peer_locked = 0;
    if (locks->peer) {
        spin_lock(&locks->peer->lock);
        locks->peer_locked = 1;
    }
}
void a20_channel_close_unlock_peer(a20_channel_close_locks_t *locks)
{
    if (locks->peer_locked) {
        spin_unlock(&locks->peer->lock);
        locks->peer_locked = 0;
    }
}
void a20_channel_close_unlock(a20_channel_close_locks_t *locks)
{
    a20_channel_close_unlock_peer(locks);
    spin_unlock(&locks->ep->lock);
    spin_unlock(&g_ch_lock);
}
void a20_channel_close_view(a20_channel_close_locks_t *locks,
                            a20_channel_close_view_t *view)
{
    a20_channel_ep_t *ep = locks->ep;
    a20_channel_ep_t *peer = locks->peer;
    view->peer = peer;
    view->peer_link = &ep->peer;
    view->peer_backlink = peer ? &peer->peer : NULL;
    view->peer_closed = peer ? &peer->peer_closed : NULL;
    view->hook = peer ? &peer->peer_close_hook : NULL;
    view->ctx_ref = peer ? &peer->peer_close_ref : NULL;
    view->ctx_put = peer ? &peer->peer_close_put : NULL;
    view->hook_ctx = peer ? &peer->peer_close_ctx : NULL;
    view->message_head = &ep->msg_head;
}
void a20_channel_close_stats_dec(void)
{ a20_objstat_add(&g_a20_objstats.channel_eps, -1); }
void a20_channel_close_wake(a20_channel_ep_t *ep)
{ wait_queue_wake_all(&ep->waiters, 0, PROC_WAKE_EVENT); }
void a20_channel_close_event(a20_channel_ep_t *ep, uint32_t event)
{ a20_event_notify(ep, A20_OBJ_CHANNEL_ENDPOINT, event, 0, 0); }
void a20_channel_close_destroy_watches(a20_channel_ep_t *ep)
{ a20_eventq_on_object_destroy(ep, A20_OBJ_CHANNEL_ENDPOINT); }
void a20_channel_close_free_msg(a20_ch_message_t *msg)
{ ch_msg_free(msg, 1); }
void a20_channel_close_free_ep(a20_channel_ep_t *ep) { kfree(ep); }

void a20_channel_ep_peer_shutdown(a20_channel_ep_t *ep)
{
    a20_rust_channel_peer_shutdown(ep);
}

/*
 * a20_channel_ep_peer_ref — referenced peer lookup for the cluster proxy
 * plumbing (kernel/cluster/remote_ep.c, docs/zh/cluster/03-kernel-impl.md §1).
 *
 * Minimal registered patch: connect() must own the proxy half of a fresh
 * pair and export() the service half's peer, and the peer pointer may only
 * be read under g_ch_lock (see CH_PEER_TEARDOWN_PROTOCOL above).  This is
 * that read plus refcount_inc_not_zero, exactly the pattern the internal
 * send/recv paths use.  Fast paths are untouched.  Returns NULL when the
 * peer is gone.
 */
a20_channel_ep_t *a20_channel_ep_peer_ref(a20_channel_ep_t *ep)
{
    a20_channel_ep_t *peer;

    if (!ep)
        return NULL;
    spin_lock(&g_ch_lock);
    peer = ep->peer;
    if (peer && !refcount_inc_not_zero(&peer->refcount))
        peer = NULL;
    spin_unlock(&g_ch_lock);
    return peer;
}

void a20_channel_ep_release(a20_channel_ep_t *ep)
{
    /* Keep the common non-final decrement in C. Only the last owner enters
     * Rust's multi-resource endpoint teardown transaction. */
    if (!ep || !refcount_dec_and_test(&ep->refcount)) return;
    a20_rust_channel_ep_finalize(ep);
}
