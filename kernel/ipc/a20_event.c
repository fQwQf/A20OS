#include "core/types.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/sync.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "sys/usercopy.h"
#include "ipc/ipc.h"
#include "proc/proc.h"
#include "ipc/objstats.h"

#define A20_EVQ_HASH_BITS  8
#define A20_EVQ_HASH_SIZE  (1u << A20_EVQ_HASH_BITS)
#define A20_EVQ_HASH_MASK  (A20_EVQ_HASH_SIZE - 1)

typedef struct a20_obj_watch_node {
    void                       *object;
    a20_watch_entry_t          *entry;
    struct a20_obj_watch_node  *next;
} a20_obj_watch_node_t;

static spinlock_t              g_evq_hash_lock;
static a20_obj_watch_node_t   *g_evq_hash[A20_EVQ_HASH_SIZE];
static int                     g_evq_hash_initialized;

static void evq_hash_init(void)
{
    if (!g_evq_hash_initialized) {
        spin_init(&g_evq_hash_lock);
        memset(g_evq_hash, 0, sizeof(g_evq_hash));
        g_evq_hash_initialized = 1;
    }
}

static uint32_t evq_hash_ptr(void *ptr)
{
    uintptr_t v = (uintptr_t)ptr;
    v = ((v >> 4) ^ (v >> 16)) & A20_EVQ_HASH_MASK;
    return (uint32_t)v;
}

static void evq_hash_insert_locked(void *object, a20_watch_entry_t *entry,
                                   a20_obj_watch_node_t *node)
{
    uint32_t idx = evq_hash_ptr(object);
    node->object = object;
    node->entry = entry;
    node->next = g_evq_hash[idx];
    g_evq_hash[idx] = node;
}

static void evq_hash_remove_locked(void *object, a20_watch_entry_t *entry)
{
    uint32_t idx = evq_hash_ptr(object);
    a20_obj_watch_node_t **pp = &g_evq_hash[idx];
    while (*pp) {
        if ((*pp)->entry == entry) {
            a20_obj_watch_node_t *del = *pp;
            *pp = del->next;
            kfree(del);
            return;
        }
        pp = &(*pp)->next;
    }
}

a20_eventq_t *a20_eventq_create(uint32_t capacity_hint)
{
    if (capacity_hint == 0) capacity_hint = A20_EVQ_DEFAULT_CAP;

    a20_eventq_t *eq = kmalloc(sizeof(*eq));
    if (!eq) return NULL;
    memset(eq, 0, sizeof(*eq));

    refcount_set(&eq->refcount, 1);
    spin_init(&eq->lock);
    spin_set_debug(&eq->lock, "a20_eventq", eq);
    wait_queue_init(&eq->waiters);
    spin_set_debug(&eq->waiters.lock, "a20_eventq.waiters", eq);
    eq->ring_cap = capacity_hint;
    eq->ring = kmalloc(eq->ring_cap * sizeof(a20_pending_event_t));
    if (!eq->ring) { kfree(eq); return NULL; }
    memset(eq->ring, 0, eq->ring_cap * sizeof(a20_pending_event_t));
    a20_objstat_add(&g_a20_objstats.eventqs, 1);
    return eq;
}

int64_t a20_eventq_watch(a20_eventq_t *eq, a20_handle_t target_h, void *target_obj,
                         uint16_t target_type, uint64_t event_mask, uint64_t user_data)
{
    if (!eq || !target_obj) return -A20_ERR_INVALID_ARGUMENT;

    a20_watch_entry_t *w = kmalloc(sizeof(*w));
    if (!w) return -A20_ERR_NO_MEMORY;
    a20_obj_watch_node_t *node = kmalloc(sizeof(*node));
    if (!node) { kfree(w); return -A20_ERR_NO_MEMORY; }

    w->target_handle = target_h;
    w->target_object = target_obj;
    w->target_type = target_type;
    w->event_mask = event_mask;
    w->user_data = user_data;
    w->owner_queue = eq;
    w->next = NULL;

    evq_hash_init();
    uint64_t hash_flags = spin_lock_irqsave(&g_evq_hash_lock);
    uint64_t eq_flags = spin_lock_irqsave(&eq->lock);
    a20_watch_entry_t *old = eq->watches;
    while (old) {
        if (old->target_object == target_obj && old->target_type == target_type) {
            old->event_mask = event_mask;
            old->user_data = user_data;
            spin_unlock_irqrestore(&eq->lock, eq_flags);
            spin_unlock_irqrestore(&g_evq_hash_lock, hash_flags);
            kfree(node);
            kfree(w);
            return A20_OK;
        }
        old = old->next;
    }

    evq_hash_insert_locked(target_obj, w, node);
    w->next = eq->watches;
    eq->watches = w;
    eq->watch_count++;
    spin_unlock_irqrestore(&eq->lock, eq_flags);
    spin_unlock_irqrestore(&g_evq_hash_lock, hash_flags);
    return A20_OK;
}

static int evq_ring_put(a20_eventq_t *eq, const a20_pending_event_t *ev)
{
    if (eq->ring_count >= eq->ring_cap) return -A20_ERR_NO_SPACE;
    eq->ring[eq->ring_tail] = *ev;
    eq->ring_tail = (eq->ring_tail + 1) % eq->ring_cap;
    eq->ring_count++;
    return A20_OK;
}

static uint64_t evq_ns_to_deadline(uint64_t timeout_ns)
{
    if (timeout_ns == A20_TIMEOUT_INFINITE) return 0; /* no deadline */
    uint64_t sec  = timeout_ns / 1000000000ULL;
    uint64_t nsec = timeout_ns % 1000000000ULL;
    uint64_t ticks;
    if (sec > UINT64_MAX / TICKS_PER_SEC) {
        ticks = UINT64_MAX / 2;
    } else {
        ticks = sec * TICKS_PER_SEC + nsec * TICKS_PER_SEC / 1000000000ULL;
    }
    uint64_t now = timer_get_ticks();
    if (ticks > UINT64_MAX - now) return UINT64_MAX / 2;
    uint64_t deadline = now + ticks;
    return deadline ? deadline : 1; /* 0 is reserved for "no deadline" */
}

/*
 * a20_eventq_wait — dequeue up to max_events events, blocking while the
 * ring is empty (docs/native-abi/05-ipc.md §3.4).
 * timeout_ns == 0 polls; A20_TIMEOUT_INFINITE waits without a deadline;
 * any other value is a relative timeout in nanoseconds.
 * The queue is referenced for the duration of the call so a concurrent
 * handle_close cannot free it under a blocked waiter.
 */
int64_t a20_eventq_wait(a20_eventq_t *eq, a20_pending_event_t *out,
                        uint32_t max_events, uint64_t timeout_ns)
{
    if (!eq || !out || max_events == 0) return -A20_ERR_INVALID_ARGUMENT;

    refcount_inc(&eq->refcount);
    uint64_t deadline = evq_ns_to_deadline(timeout_ns);
    int64_t result;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&eq->lock);
        if (eq->ring_count > 0) {
            uint32_t n = 0;
            while (eq->ring_count > 0 && n < max_events) {
                out[n++] = eq->ring[eq->ring_head];
                eq->ring_head = (eq->ring_head + 1) % eq->ring_cap;
                eq->ring_count--;
            }
            spin_unlock_irqrestore(&eq->lock, flags);
            result = (int64_t)n;
            break;
        }
        if (timeout_ns == 0) {
            spin_unlock_irqrestore(&eq->lock, flags);
            result = -A20_ERR_WOULD_BLOCK;
            break;
        }
        spin_unlock_irqrestore(&eq->lock, flags);

        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE,
                                                    deadline);
        if (!token.task) { result = -A20_ERR_WOULD_BLOCK; break; }

        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&eq->lock);
        if (eq->ring_count == 0) {
            bool linked = wait_queue_link(&eq->waiters, &entry, token, 0);
            spin_unlock_irqrestore(&eq->lock, flags);
            proc_wake_reason_t reason;
            if (linked)
                reason = proc_park_commit(token);
            else {
                (void)proc_park_cancel(token);
                reason = PROC_WAKE_CANCEL;
            }
            wait_queue_unlink(&eq->waiters, &entry);
            proc_park_finish(token);
            if (reason == PROC_WAKE_TIMEOUT ||
                reason == PROC_WAKE_TIMEOUT_CAPACITY) {
                result = -A20_ERR_TIMED_OUT;
                break;
            }
            if (proc_wake_reason_is_task_interrupt(reason)) {
                result = -A20_ERR_INTERRUPTED;
                break;
            }
            continue;
        }
        spin_unlock_irqrestore(&eq->lock, flags);
        (void)proc_park_cancel(token);
        proc_park_finish(token);
    }

    a20_eventq_release(eq);
    return result;
}

int64_t a20_eventq_cancel(a20_eventq_t *eq, a20_handle_t target_h)
{
    if (!eq) return -A20_ERR_BAD_HANDLE;

    evq_hash_init();
    uint64_t hash_flags = spin_lock_irqsave(&g_evq_hash_lock);
    uint64_t eq_flags = spin_lock_irqsave(&eq->lock);
    a20_watch_entry_t **pp = &eq->watches;
    while (*pp) {
        if ((*pp)->target_handle == target_h) {
            a20_watch_entry_t *del = *pp;
            *pp = del->next;
            eq->watch_count--;
            evq_hash_remove_locked(del->target_object, del);
            spin_unlock_irqrestore(&eq->lock, eq_flags);
            spin_unlock_irqrestore(&g_evq_hash_lock, hash_flags);
            kfree(del);
            return A20_OK;
        }
        pp = &(*pp)->next;
    }
    spin_unlock_irqrestore(&eq->lock, eq_flags);
    spin_unlock_irqrestore(&g_evq_hash_lock, hash_flags);
    return -A20_ERR_NOT_FOUND;
}

void a20_eventq_release(a20_eventq_t *eq)
{
    if (!eq) return;
    if (!refcount_dec_and_test(&eq->refcount)) return;
    a20_objstat_add(&g_a20_objstats.eventqs, -1);

    /* Remove watches that use this queue as their target before tearing down
     * the queue-owned watch list. */
    a20_eventq_on_object_destroy(eq, A20_OBJ_EVENT_QUEUE);

    /* Remove the queue-owned watches and their reverse-index nodes under the
     * global lock order hash -> eq.  This leaves no window in which cancel
     * or object-destroy can free the same entry. */
    evq_hash_init();
    uint64_t hash_flags = spin_lock_irqsave(&g_evq_hash_lock);
    uint64_t eq_flags = spin_lock_irqsave(&eq->lock);
    a20_watch_entry_t *w = eq->watches;
    eq->watches = NULL;
    eq->watch_count = 0;
    while (w) {
        a20_watch_entry_t *next = w->next;
        evq_hash_remove_locked(w->target_object, w);
        kfree(w);
        w = next;
    }
    spin_unlock_irqrestore(&eq->lock, eq_flags);
    spin_unlock_irqrestore(&g_evq_hash_lock, hash_flags);
    wait_queue_wake_all(&eq->waiters, 0, PROC_WAKE_EVENT);
    kfree(eq->ring);
    kfree(eq);
}

void a20_event_notify(void *target_object, uint16_t target_type,
                      uint32_t event_type, uint64_t data0, uint64_t data1)
{
    evq_hash_init();
    uint32_t idx = evq_hash_ptr(target_object);

    uint64_t hash_flags = spin_lock_irqsave(&g_evq_hash_lock);
    a20_obj_watch_node_t *node = g_evq_hash[idx];
    while (node) {
        if (node->entry->target_object == target_object &&
            node->entry->target_type == target_type) {
            a20_watch_entry_t *we = node->entry;
            if (!(we->event_mask & ((uint64_t)1u << event_type))) {
                node = node->next;
                continue;
            }
            a20_eventq_t *eq = we->owner_queue;
            int should_wake = 0;
            uint64_t eq_flags = spin_lock_irqsave(&eq->lock);
            if (eq->ring_count < eq->ring_cap) {
                a20_pending_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.source = we->target_handle;
                ev.type = event_type;
                ev.events = (uint64_t)1u << event_type;
                ev.user_data = we->user_data;
                ev.data0 = data0;
                ev.data1 = data1;
                evq_ring_put(eq, &ev);
                should_wake = 1;
            } else {
                /* Ring full: wake anyway so waiters re-read; oldest events
                 * stay queued (wake-then-keep policy). */
                should_wake = 1;
            }
            spin_unlock_irqrestore(&eq->lock, eq_flags);
            /* Wake while the hash lock still pins the watch/owner queue:
             * final eventq_release must remove this node under the same lock
             * before freeing eq, so no refcount resurrection is needed. */
            if (should_wake)
                wait_queue_wake_one(&eq->waiters, 0, PROC_WAKE_EVENT);
        }
        node = node->next;
    }
    spin_unlock_irqrestore(&g_evq_hash_lock, hash_flags);
}

void a20_eventq_on_object_destroy(void *object, uint16_t object_type)
{
    evq_hash_init();
    uint32_t idx = evq_hash_ptr(object);

    uint64_t hash_flags = spin_lock_irqsave(&g_evq_hash_lock);
    a20_obj_watch_node_t **pp = &g_evq_hash[idx];
    while (*pp) {
        a20_obj_watch_node_t *node = *pp;
        if (node->object == object && node->entry->target_type == object_type) {
            a20_watch_entry_t *we = node->entry;
            a20_eventq_t *eq = we->owner_queue;
            *pp = node->next;
            kfree(node);

            uint64_t eq_flags = spin_lock_irqsave(&eq->lock);
            a20_watch_entry_t **wpp = &eq->watches;
            while (*wpp) {
                if (*wpp == we) {
                    *wpp = we->next;
                    eq->watch_count--;
                    break;
                }
                wpp = &(*wpp)->next;
            }
            spin_unlock_irqrestore(&eq->lock, eq_flags);
            kfree(we);
            continue;
        }
        pp = &node->next;
    }
    spin_unlock_irqrestore(&g_evq_hash_lock, hash_flags);
}
