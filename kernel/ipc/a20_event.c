#include "core/types.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/sync.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "sys/usercopy.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/errno.h"

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

static void evq_hash_insert(void *object, a20_watch_entry_t *entry)
{
    evq_hash_init();
    a20_obj_watch_node_t *node = kmalloc(sizeof(*node));
    if (!node) return;
    node->object = object;
    node->entry = entry;
    node->next = NULL;

    uint32_t idx = evq_hash_ptr(object);
    uint64_t flags = spin_lock_irqsave(&g_evq_hash_lock);
    node->next = g_evq_hash[idx];
    g_evq_hash[idx] = node;
    spin_unlock_irqrestore(&g_evq_hash_lock, flags);
}

static void evq_hash_remove(void *object, a20_watch_entry_t *entry)
{
    evq_hash_init();
    uint32_t idx = evq_hash_ptr(object);
    uint64_t flags = spin_lock_irqsave(&g_evq_hash_lock);
    a20_obj_watch_node_t **pp = &g_evq_hash[idx];
    while (*pp) {
        if ((*pp)->entry == entry) {
            a20_obj_watch_node_t *del = *pp;
            *pp = del->next;
            kfree(del);
            break;
        }
        pp = &(*pp)->next;
    }
    spin_unlock_irqrestore(&g_evq_hash_lock, flags);
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
    return eq;
}

int64_t a20_eventq_watch(a20_eventq_t *eq, a20_handle_t target_h, void *target_obj,
                         uint16_t target_type, uint64_t event_mask, uint64_t user_data)
{
    if (!eq || !target_obj) return -A20_ERR_INVALID_ARGUMENT;

    uint64_t flags = spin_lock_irqsave(&eq->lock);
    a20_watch_entry_t *w = eq->watches;
    while (w) {
        if (w->target_object == target_obj) {
            w->event_mask = event_mask;
            w->user_data = user_data;
            spin_unlock_irqrestore(&eq->lock, flags);
            return A20_OK;
        }
        w = w->next;
    }

    w = kmalloc(sizeof(*w));
    if (!w) { spin_unlock_irqrestore(&eq->lock, flags); return -A20_ERR_NO_MEMORY; }
    w->target_handle = target_h;
    w->target_object = target_obj;
    w->target_type = target_type;
    w->event_mask = event_mask;
    w->user_data = user_data;
    w->owner_queue = eq;
    w->next = eq->watches;
    eq->watches = w;
    eq->watch_count++;
    spin_unlock_irqrestore(&eq->lock, flags);

    evq_hash_insert(target_obj, w);
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

int64_t a20_eventq_wait(a20_eventq_t *eq, a20_pending_event_t *out, uint64_t timeout_ns)
{
    if (!eq || !out) return -A20_ERR_FAULT;

    uint64_t flags = spin_lock_irqsave(&eq->lock);
    if (eq->ring_count > 0) {
        *out = eq->ring[eq->ring_head];
        eq->ring_head = (eq->ring_head + 1) % eq->ring_cap;
        eq->ring_count--;
        spin_unlock_irqrestore(&eq->lock, flags);
        return A20_OK;
    }
    if (timeout_ns == 0) {
        spin_unlock_irqrestore(&eq->lock, flags);
        return -A20_ERR_WOULD_BLOCK;
    }
    spin_unlock_irqrestore(&eq->lock, flags);

    (void)timeout_ns;
    return -A20_ERR_WOULD_BLOCK;
}

int64_t a20_eventq_cancel(a20_eventq_t *eq, a20_handle_t target_h)
{
    if (!eq) return -A20_ERR_BAD_HANDLE;

    uint64_t flags = spin_lock_irqsave(&eq->lock);
    a20_watch_entry_t **pp = &eq->watches;
    while (*pp) {
        if ((*pp)->target_handle == target_h) {
            a20_watch_entry_t *del = *pp;
            *pp = del->next;
            eq->watch_count--;
            spin_unlock_irqrestore(&eq->lock, flags);
            evq_hash_remove(del->target_object, del);
            kfree(del);
            return A20_OK;
        }
        pp = &(*pp)->next;
    }
    spin_unlock_irqrestore(&eq->lock, flags);
    return -A20_ERR_NOT_FOUND;
}

void a20_eventq_release(a20_eventq_t *eq)
{
    if (!eq) return;
    if (!refcount_dec_and_test(&eq->refcount)) return;

    a20_watch_entry_t *w = eq->watches;
    while (w) {
        a20_watch_entry_t *next = w->next;
        evq_hash_remove(w->target_object, w);
        kfree(w);
        w = next;
    }
    wait_queue_wake_all(&eq->waiters);
    kfree(eq->ring);
    kfree(eq);
}

void a20_event_notify(void *target_object, uint16_t target_type,
                      uint32_t event_type, uint64_t data0, uint64_t data1)
{
    evq_hash_init();
    uint32_t idx = evq_hash_ptr(target_object);

    int wake_count = 0;
    a20_eventq_t *wake_queues[32];

    uint64_t hash_flags = spin_lock_irqsave(&g_evq_hash_lock);
    a20_obj_watch_node_t *node = g_evq_hash[idx];
    while (node) {
        if (node->entry->target_object == target_object) {
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
                should_wake = 1;
            }
            spin_unlock_irqrestore(&eq->lock, eq_flags);
            if (should_wake && wake_count < (int)(sizeof(wake_queues) / sizeof(wake_queues[0])))
                wake_queues[wake_count++] = eq;
        }
        node = node->next;
    }
    spin_unlock_irqrestore(&g_evq_hash_lock, hash_flags);

    for (int i = 0; i < wake_count; i++)
        wait_queue_wake_one(&wake_queues[i]->waiters);
}

void a20_eventq_on_object_destroy(void *object)
{
    evq_hash_init();
    uint32_t idx = evq_hash_ptr(object);

    uint64_t hash_flags = spin_lock_irqsave(&g_evq_hash_lock);
    a20_obj_watch_node_t **pp = &g_evq_hash[idx];
    while (*pp) {
        a20_obj_watch_node_t *node = *pp;
        if (node->object == object) {
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
