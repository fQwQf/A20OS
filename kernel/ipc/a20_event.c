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
#include "fs/readiness.h"
#include "core/poll.h"

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
            wait_queue_wake_all(&eq->waiters, 0, PROC_WAKE_EVENT);
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
    wait_queue_wake_all(&eq->waiters, 0, PROC_WAKE_EVENT);
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

typedef struct evq_readiness_watch {
    a20_handle_t handle;
    uint16_t type;
    uint64_t event_mask;
    uint64_t user_data;
} evq_readiness_watch_t;

static uint64_t evq_readiness_mask(void)
{
    return A20_EVENT_MASK(A20_EVENT_READABLE) |
           A20_EVENT_MASK(A20_EVENT_WRITABLE) |
           A20_EVENT_MASK(A20_EVENT_ERROR) |
           A20_EVENT_MASK(A20_EVENT_CLOSED) |
           A20_EVENT_MASK(A20_EVENT_ACCEPT_READY) |
           A20_EVENT_MASK(A20_EVENT_MESSAGE_READY);
}

static short evq_mask_to_poll(uint64_t mask)
{
    short events = 0;
    if (mask & (A20_EVENT_MASK(A20_EVENT_READABLE) |
                A20_EVENT_MASK(A20_EVENT_ACCEPT_READY) |
                A20_EVENT_MASK(A20_EVENT_MESSAGE_READY)))
        events |= POLLIN;
    if (mask & A20_EVENT_MASK(A20_EVENT_WRITABLE))
        events |= POLLOUT;
    if (!events)
        events = POLLIN | POLLOUT;
    return events;
}

static uint64_t evq_poll_to_mask(short events)
{
    uint64_t mask = 0;
    if (events & (POLLIN | POLLPRI))
        mask |= A20_EVENT_MASK(A20_EVENT_READABLE) |
                A20_EVENT_MASK(A20_EVENT_ACCEPT_READY) |
                A20_EVENT_MASK(A20_EVENT_MESSAGE_READY);
    if (events & POLLOUT)
        mask |= A20_EVENT_MASK(A20_EVENT_WRITABLE);
    if (events & POLLERR)
        mask |= A20_EVENT_MASK(A20_EVENT_ERROR);
    if (events & (POLLHUP | POLLNVAL))
        mask |= A20_EVENT_MASK(A20_EVENT_CLOSED);
    return mask;
}

typedef struct evq_wait_probe {
    a20_eventq_t *eq;
    uint64_t generation;
} evq_wait_probe_t;

static bool evq_wait_changed(void *arg)
{
    evq_wait_probe_t *probe = arg;
    a20_eventq_t *eq = probe->eq;
    uint64_t flags = spin_lock_irqsave(&eq->lock);
    bool ready = eq->ring_count != 0;
    spin_unlock_irqrestore(&eq->lock, flags);
    return ready || wait_queue_generation(&eq->waiters) != probe->generation;
}

static uint32_t evq_first_event(uint64_t events)
{
    for (uint32_t event = 0; event < 64; event++)
        if (events & (1ULL << event))
            return event;
    return 0;
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
        uint32_t watch_cap = eq->watch_count;
        spin_unlock_irqrestore(&eq->lock, flags);

        readiness_interest_t *interests = watch_cap ?
            kcalloc(watch_cap, sizeof(*interests)) : NULL;
        evq_readiness_watch_t *watches = watch_cap ?
            kcalloc(watch_cap, sizeof(*watches)) : NULL;
        if (watch_cap && (!interests || !watches)) {
            kfree(interests);
            kfree(watches);
            result = -A20_ERR_NO_MEMORY;
            break;
        }

        size_t count = 0;
        evq_wait_probe_t probe = { .eq = eq };
        flags = spin_lock_irqsave(&eq->lock);
        if (eq->ring_count) {
            spin_unlock_irqrestore(&eq->lock, flags);
            kfree(interests);
            kfree(watches);
            continue;
        }
        for (a20_watch_entry_t *watch = eq->watches;
             watch && count < watch_cap; watch = watch->next) {
            uint64_t mask = watch->event_mask & evq_readiness_mask();
            if (!mask || !a20_object_is_vfile_backed(watch->target_type))
                continue;
            interests[count] = (readiness_interest_t){
                .fd = (int)(uintptr_t)watch->target_object,
                .events = evq_mask_to_poll(mask),
                .flags = READINESS_F_GLOBAL_FD,
                .cookie = count,
            };
            watches[count] = (evq_readiness_watch_t){
                .handle = watch->target_handle,
                .type = watch->target_type,
                .event_mask = watch->event_mask,
                .user_data = watch->user_data,
            };
            count++;
        }
        probe.generation = wait_queue_generation(&eq->waiters);
        spin_unlock_irqrestore(&eq->lock, flags);

        readiness_extra_t queue_event = {
            .source = { &eq->waiters, 0, 0 },
            .ready = evq_wait_changed,
            .arg = &probe,
        };
        int wait_result = readiness_wait_once(
            interests, count, &queue_event, 1, max_events,
            deadline, timeout_ns != A20_TIMEOUT_INFINITE);
        if (wait_result > 0) {
            uint32_t n = 0;
            for (size_t i = 0; i < count && n < max_events; i++) {
                uint64_t active = evq_poll_to_mask(interests[i].revents) &
                                  watches[i].event_mask;
                if (!active)
                    continue;
                memset(&out[n], 0, sizeof(out[n]));
                out[n].source = watches[i].handle;
                out[n].type = evq_first_event(active);
                out[n].events = active;
                out[n].user_data = watches[i].user_data;
                n++;
            }
            kfree(interests);
            kfree(watches);
            if (n) {
                result = n;
                break;
            }
            continue;
        }
        kfree(interests);
        kfree(watches);
        if (wait_result == READINESS_RETRY)
            continue;
        if (wait_result == -EINTR) {
            result = -A20_ERR_INTERRUPTED;
            break;
        }
        if (wait_result < 0) {
            result = wait_result == -ENOMEM ?
                     -A20_ERR_NO_MEMORY : -A20_ERR_WOULD_BLOCK;
            break;
        }
        result = timeout_ns == 0 ?
                 -A20_ERR_WOULD_BLOCK : -A20_ERR_TIMED_OUT;
        break;
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
            wait_queue_wake_all(&eq->waiters, 0, PROC_WAKE_EVENT);
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
            wait_queue_wake_all(&eq->waiters, 0, PROC_WAKE_EVENT);
            kfree(we);
            continue;
        }
        pp = &node->next;
    }
    spin_unlock_irqrestore(&g_evq_hash_lock, hash_flags);
}
