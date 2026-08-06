/*
 * A20OS Native ABI — Handle table implementation.
 * Design reference: docs/native-abi/03-handle.md §2
 * Design inspiration: Windows NT kernel objects / handle + rights model and
 * Zircon (Fuchsia) handle transfer semantics; see docs/ACKNOWLEDGMENTS.md §3.
 * NATIVE_HANDLE_CAPABILITY_TEST_CONTRACT: lookup/install/remove paths are the
 * checked surface for rights downgrade, temporal limits, labels, close/dup/
 * transfer, and partial-delivery consistency gates.
 */
#include "core/types.h"
#include "core/string.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/klog.h"
#include "core/defs.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "fs/vfs.h"
#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/handle_table.h"
#include "abi/native/vmo.h"
#include "abi/native/objects.h"
#include "ipc/objstats.h"
#include "proc/proc.h"
#include "handle_table.h"

struct a20_ht_internal {
    a20_handle_entry_t *entries;
    uint32_t            capacity;
    uint32_t            count;
    uint32_t            free_hint;
    spinlock_t          lock;
    uint64_t           *free_bitmap;
    uint32_t            bitmap_size;
    uint8_t             security_label; /* Bell-LaPadula process label: 0=L, 1=M, 2=H */
    refcount_t          refcount;       /* shared by all threads of a process */
    struct a20_ht_internal *registry_next; /* global sweeper registry */
    /* Checkpoint-based signal simulation (native ABI has no async signals).
     * bit n == signal n; SIGKILL (9) is handled immediately, never queued. */
    uint64_t            sig_pending;
    uint64_t            sig_blocked;
    uint32_t            max_handles;    /* hard quota (M2); install fails above this */
};

/* ---- Global handle-table registry (sweeper) ----
 * Every live handle table is linked here so a20_temporal_sweep_all() can
 * enforce AUTO_CLOSE semantics without knowing about tasks.  Lock ordering:
 * registry lock → per-table lock (never the reverse). */
static spinlock_t g_a20_ht_registry_lock = SPINLOCK_INIT;
static struct a20_ht_internal *g_a20_ht_registry;

extern void a20_timer_object_ref(int slot);
extern void a20_timer_object_release(int slot);

/* a20_object_is_vfile_backed / a20_object_ref / a20_object_release live in
 * kernel/ipc/a20_object.c (always compiled; see that file's header comment). */

/* Software CTZ — avoids __builtin_ctzll → __ctzdi2 libgcc dependency in freestanding */
static inline int a20_ctz64(uint64_t v)
{
    if (v == 0) return 64;
    int n = 0;
    if ((v & 0xFFFFFFFF) == 0) { n += 32; v >>= 32; }
    if ((v & 0xFFFF) == 0)     { n += 16; v >>= 16; }
    if ((v & 0xFF) == 0)       { n += 8;  v >>= 8;  }
    if ((v & 0xF) == 0)        { n += 4;  v >>= 4;  }
    if ((v & 0x3) == 0)        { n += 2;  v >>= 2;  }
    if ((v & 0x1) == 0)        { n += 1;             }
    return n;
}

static uint64_t a20_current_tick(void)
{
    return timer_get_ticks();
}

static inline a20_rights_t a20_effective_rights(const a20_handle_entry_t *e)
{
    if (e->expiry_tick > 0 && a20_current_tick() >= e->expiry_tick)
        return 0;
    if ((e->temporal_flags & A20_TEMPORAL_OP_COUNT) && e->remaining_ops == 0)
        return 0;
    return e->rights;
}

/*
 * a20_blp_read_ok  — No Read Up (docs/native-abi/06-security.md §5.2).
 * Process label must dominate object label: ℓ(p) ≥ ℓ(o).
 */
static inline int a20_blp_read_ok(uint8_t proc_label, uint8_t obj_label)
{
    return proc_label >= obj_label;
}

/*
 * a20_blp_write_ok — No Write Down (docs/native-abi/06-security.md §5.2).
 * Process label must be dominated by object label: ℓ(p) ≤ ℓ(o).
 */
static inline int a20_blp_write_ok(uint8_t proc_label, uint8_t obj_label)
{
    return proc_label <= obj_label;
}

uint8_t a20_ht_get_label(struct a20_ht_internal *ht)
{
    return ht ? ht->security_label : 0;
}

void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label)
{
    if (ht && label <= 2)
        ht->security_label = label;
}

static int ht_alloc_slot(struct a20_ht_internal *ht)
{
    for (uint32_t i = ht->free_hint / 64; i < ht->bitmap_size; i++) {
        uint64_t word = ht->free_bitmap[i];
        if (word != UINT64_MAX) {
            int bit = a20_ctz64(~word);
            uint32_t slot = i * 64 + (uint32_t)bit;
            if (slot < ht->capacity) {
                ht->free_bitmap[i] |= (1ULL << bit);
                ht->free_hint = slot + 1;
                return (int)slot;
            }
        }
    }
    return -1;
}

static void ht_free_slot(struct a20_ht_internal *ht, uint32_t slot)
{
    uint32_t word_idx = slot / 64;
    uint32_t bit_idx  = slot % 64;
    ht->free_bitmap[word_idx] &= ~(1ULL << bit_idx);
    if (slot < ht->free_hint)
        ht->free_hint = slot;
}

static int ht_grow(struct a20_ht_internal *ht)
{
    if (ht->capacity >= A20_HT_MAX_CAP)
        return -A20_ERR_NO_SPACE;

    uint32_t new_cap = ht->capacity * A20_HT_GROWTH_FACTOR;
    if (new_cap > A20_HT_MAX_CAP)
        new_cap = A20_HT_MAX_CAP;

    uint32_t new_bm_size = (new_cap + 63) / 64;
    a20_handle_entry_t *new_entries = kmalloc(new_cap * sizeof(a20_handle_entry_t));
    uint64_t *new_bitmap = kmalloc(new_bm_size * sizeof(uint64_t));
    if (!new_entries || !new_bitmap) {
        kfree(new_entries);
        kfree(new_bitmap);
        return -A20_ERR_NO_MEMORY;
    }

    memset(new_entries, 0, new_cap * sizeof(a20_handle_entry_t));
    memcpy(new_entries, ht->entries, ht->capacity * sizeof(a20_handle_entry_t));
    memset(new_bitmap, 0, new_bm_size * sizeof(uint64_t));
    memcpy(new_bitmap, ht->free_bitmap, ht->bitmap_size * sizeof(uint64_t));

    a20_handle_entry_t *old_entries = ht->entries;
    uint64_t *old_bitmap = ht->free_bitmap;

    ht->entries = new_entries;
    ht->free_bitmap = new_bitmap;
    ht->bitmap_size = new_bm_size;
    ht->capacity = new_cap;

    kfree(old_entries);
    kfree(old_bitmap);
    return 0;
}

static const a20_rights_t a20_type_rights[A20_OBJ_EXT_PROG + 1] = {
    [A20_OBJ_INVALID]          = 0,
    [A20_OBJ_TASK]             = A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT |
                                 A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL |
                                 A20_RIGHT_ADMIN,
    [A20_OBJ_THREAD]           = A20_RIGHT_STAT | A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_CONTROL,
    [A20_OBJ_FILE]             = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                                 A20_RIGHT_SEEK | A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_EXEC | A20_RIGHT_MAP | A20_RIGHT_CONTROL,
    [A20_OBJ_DIRECTORY]        = A20_RIGHT_READ | A20_RIGHT_STAT | A20_RIGHT_DUP |
                                 A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL,
    [A20_OBJ_SOCKET]           = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                                 A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_CONNECT | A20_RIGHT_ACCEPT | A20_RIGHT_CONTROL,
    [A20_OBJ_PIPE_ENDPOINT]    = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                                 A20_RIGHT_DUP | A20_RIGHT_TRANSFER,
    [A20_OBJ_CHANNEL_ENDPOINT] = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_DUP |
                                 A20_RIGHT_TRANSFER,
    [A20_OBJ_EVENT_QUEUE]      = A20_RIGHT_READ | A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_CONTROL,
    [A20_OBJ_TIMER]            = A20_RIGHT_STAT | A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_CONTROL,
    [A20_OBJ_MEMORY]           = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_MAP |
                                 A20_RIGHT_STAT | A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_CONTROL,
    [A20_OBJ_DEVICE]           = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_MAP |
                                 A20_RIGHT_STAT | A20_RIGHT_SEEK | A20_RIGHT_DUP |
                                 A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL,
    [A20_OBJ_NAMESPACE]        = A20_RIGHT_STAT | A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_CONTROL | A20_RIGHT_ADMIN,
    [A20_OBJ_DEBUG]            = A20_RIGHT_READ | A20_RIGHT_WRITE |
                                 A20_RIGHT_WAIT | A20_RIGHT_SIGNAL |
                                 A20_RIGHT_STAT | A20_RIGHT_DUP |
                                 A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL |
                                 A20_RIGHT_ADMIN,
    [A20_OBJ_EXT_PROG]         = A20_RIGHT_READ | A20_RIGHT_DUP |
                                 A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL,
};

a20_rights_t a20_type_valid_rights(uint16_t type)
{
    if (type > A20_OBJ_EXT_PROG) return 0;
    return a20_type_rights[type];
}

struct a20_ht_internal *a20_ht_create(void)
{
    struct a20_ht_internal *ht = kmalloc(sizeof(*ht));
    if (!ht) return NULL;

    ht->capacity = A20_HT_INITIAL_CAP;
    ht->count = 0;
    ht->free_hint = 0;
    ht->max_handles = A20_HT_DEFAULT_QUOTA;
    ht->security_label = 0; /* default: L (docs/native-abi/06-security.md §5.1) */
    refcount_set(&ht->refcount, 1);
    spin_init(&ht->lock);
    spin_set_debug(&ht->lock, "a20_handle_table", ht);
    ht->bitmap_size = (ht->capacity + 63) / 64;

    ht->entries = kmalloc(ht->capacity * sizeof(a20_handle_entry_t));
    ht->free_bitmap = kmalloc(ht->bitmap_size * sizeof(uint64_t));
    if (!ht->entries || !ht->free_bitmap) {
        kfree(ht->entries);
        kfree(ht->free_bitmap);
        kfree(ht);
        return NULL;
    }
    memset(ht->entries, 0, ht->capacity * sizeof(a20_handle_entry_t));
    memset(ht->free_bitmap, 0, ht->bitmap_size * sizeof(uint64_t));

    uint64_t rflags = spin_lock_irqsave(&g_a20_ht_registry_lock);
    ht->registry_next = g_a20_ht_registry;
    g_a20_ht_registry = ht;
    spin_unlock_irqrestore(&g_a20_ht_registry_lock, rflags);
    return ht;
}

void a20_ht_destroy(struct a20_ht_internal *ht)
{
    if (!ht) return;

    extern void a20_channel_trace_dump(void);
    extern void a20_channel_trace(uint32_t op, uint32_t len, void *p1,
                                  void *p2, uint32_t meta);
    a20_channel_trace(9, 0, ht, ht->entries, 0);
    printf("[HT-DESTROY] ht=%p entries=%p bitmap=%p cap=%u refs=%d pid=%d\n",
           (void *)ht, (void *)ht->entries, (void *)ht->free_bitmap,
           ht->capacity, (int)ht->refcount.value, proc_current() ? proc_current()->pid : -1);

    uint64_t rflags = spin_lock_irqsave(&g_a20_ht_registry_lock);
    struct a20_ht_internal **pp = &g_a20_ht_registry;
    while (*pp) {
        if (*pp == ht) {
            *pp = ht->registry_next;
            break;
        }
        pp = &(*pp)->registry_next;
    }
    ht->registry_next = NULL;
    spin_unlock_irqrestore(&g_a20_ht_registry_lock, rflags);

    /* The table is dead (refcount hit zero): no concurrent lookups remain.
     * Drop every entry's object reference so channels see peer_closed,
     * event queues are released and vfile fds are closed on process exit. */
    for (uint32_t i = 0; i < ht->capacity; i++) {
        if (ht->entries[i].object != NULL && ht->entries[i].state != A20_HS_FREE) {
            void *obj = ht->entries[i].object;
            uint16_t type = ht->entries[i].type;
            ht->entries[i].object = NULL;
            ht->entries[i].state = A20_HS_FREE;
            a20_objstat_add(&g_a20_objstats.handles, -1);
            a20_object_release(obj, type);
        }
    }
    kfree(ht->entries);
    kfree(ht->free_bitmap);
    kfree(ht);
}

/*
 * The handle table is process-local: every thread created via
 * proc_create_thread (CLONE_THREAD) holds one reference.  The finalizer
 * runs when the last thread of the process drops its reference.
 */
struct a20_ht_internal *a20_ht_get_ref(struct a20_ht_internal *ht)
{
    if (!ht) return NULL;
    refcount_inc(&ht->refcount);
    return ht;
}

void a20_ht_put_ref(struct a20_ht_internal *ht)
{
    if (!ht) return;
    if (refcount_dec_and_test(&ht->refcount))
        a20_ht_destroy(ht);
}

int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, a20_rights_t rights)
{
    a20_rights_t valid = a20_type_valid_rights(type);
    if ((rights & ~valid) != 0)
        rights &= valid;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
    int slot = -1;
    if (ht->count < ht->max_handles)
        slot = ht_alloc_slot(ht);
    if (slot < 0 && ht->count < ht->max_handles) {
        if (ht_grow(ht) == 0)
            slot = ht_alloc_slot(ht);
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_NO_SPACE;
    }
    ht->entries[slot].object = object;
    ht->entries[slot].type = type;
    ht->entries[slot].rights = rights;
    ht->entries[slot].expiry_tick = 0;
    ht->entries[slot].remaining_ops = 0;
    ht->entries[slot].temporal_flags = 0;
    ht->entries[slot].security_label = 0;
    ht->entries[slot].state = A20_HS_ACTIVE;
    ht->count++;
    a20_objstat_add(&g_a20_objstats.handles, 1);
    spin_unlock_irqrestore(&ht->lock, flags);
    return (int64_t)slot;
}

/*
 * a20_handle_install_temporal — install handle inheriting temporal constraints
 * from a source handle. Enforces non-refreshability (docs/native-abi/06-security.md §6.4):
 * the new handle's expiry ≤ source expiry, ops ≤ source remaining_ops.
 */
int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                     uint16_t type, a20_rights_t rights,
                                     uint64_t expiry_tick, uint32_t remaining_ops,
                                     uint32_t temporal_flags, uint8_t security_label)
{
    a20_rights_t valid = a20_type_valid_rights(type);
    if ((rights & ~valid) != 0)
        rights &= valid;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
    int slot = -1;
    if (ht->count < ht->max_handles)
        slot = ht_alloc_slot(ht);
    if (slot < 0 && ht->count < ht->max_handles) {
        if (ht_grow(ht) == 0)
            slot = ht_alloc_slot(ht);
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_NO_SPACE;
    }
    ht->entries[slot].object = object;
    ht->entries[slot].type = type;
    ht->entries[slot].rights = rights;
    ht->entries[slot].expiry_tick = expiry_tick;
    ht->entries[slot].remaining_ops = remaining_ops;
    ht->entries[slot].temporal_flags = temporal_flags;
    ht->entries[slot].security_label = security_label;
    ht->entries[slot].state = A20_HS_ACTIVE;
    ht->count++;
    a20_objstat_add(&g_a20_objstats.handles, 1);
    spin_unlock_irqrestore(&ht->lock, flags);
    return (int64_t)slot;
}

/* Install at a caller-selected slot for Native fd inheritance.  The reserved
 * range is kept separate from ordinary allocation so libc can reconstruct
 * fd-to-handle mapping from the startup count. */
int64_t a20_handle_install_at_temporal(struct a20_ht_internal *ht,
                                       a20_handle_t slot, void *object,
                                       uint16_t type, a20_rights_t rights,
                                       uint64_t expiry_tick,
                                       uint32_t remaining_ops,
                                       uint32_t temporal_flags,
                                       uint8_t security_label)
{
    if (!ht || slot >= A20_HT_MAX_CAP)
        return -A20_ERR_INVALID_ARGUMENT;

    a20_rights_t valid = a20_type_valid_rights(type);
    if ((rights & ~valid) != 0)
        rights &= valid;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
    while (slot >= ht->capacity) {
        if (ht_grow(ht) < 0) {
            spin_unlock_irqrestore(&ht->lock, flags);
            return -A20_ERR_NO_SPACE;
        }
    }

    uint32_t word_idx = slot / 64;
    uint32_t bit_idx = slot % 64;
    if (ht->free_bitmap[word_idx] & (1ULL << bit_idx)) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_EXISTS;
    }

    if (ht->count >= ht->max_handles) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_NO_SPACE;
    }
    ht->free_bitmap[word_idx] |= (1ULL << bit_idx);
    if (ht->free_hint <= slot)
        ht->free_hint = slot + 1;
    ht->entries[slot].object = object;
    ht->entries[slot].type = type;
    ht->entries[slot].rights = rights;
    ht->entries[slot].expiry_tick = expiry_tick;
    ht->entries[slot].remaining_ops = remaining_ops;
    ht->entries[slot].temporal_flags = temporal_flags;
    ht->entries[slot].security_label = security_label;
    ht->entries[slot].state = A20_HS_ACTIVE;
    a20_objstat_add(&g_a20_objstats.handles, 1);
    ht->count++;
    spin_unlock_irqrestore(&ht->lock, flags);
    return (int64_t)slot;
}

static int64_t a20_handle_lookup_ref_mode(struct a20_ht_internal *ht,
                                          a20_handle_t h,
                                          uint16_t expected_type,
                                          a20_rights_t required_rights,
                                          a20_handle_entry_t *out,
                                          int take_ref)
{
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    if (h >= ht->capacity) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_BAD_HANDLE;
    }
    a20_handle_entry_t *e = &ht->entries[h];
    if (e->object == NULL || e->state == A20_HS_FREE) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_BAD_HANDLE;
    }
    if (e->state == A20_HS_CLOSING) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_BAD_HANDLE;
    }
    if (e->state == A20_HS_EXPIRED) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_EXPIRED;
    }
    if (expected_type != A20_OBJ_INVALID && e->type != expected_type) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_BAD_HANDLE;
    }
    a20_rights_t effective = a20_effective_rights(e);
    if ((effective & required_rights) != required_rights) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_ACCESS;
    }
    if ((e->temporal_flags & A20_TEMPORAL_OP_COUNT) && e->remaining_ops > 0)
        e->remaining_ops--;
    *out = *e;
    out->rights = effective;
    if (take_ref)
        a20_object_ref(out->object, out->type);
    spin_unlock_irqrestore(&ht->lock, flags);
    return A20_OK;
}

int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                    uint16_t expected_type, a20_rights_t required_rights,
                                    a20_handle_entry_t *out)
{
    return a20_handle_lookup_ref_mode(ht, h, expected_type, required_rights, out, 0);
}

int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                       a20_handle_t h,
                                       uint16_t expected_type,
                                       a20_rights_t required_rights,
                                       a20_handle_entry_t *out)
{
    return a20_handle_lookup_ref_mode(ht, h, expected_type, required_rights, out, 1);
}

/*
 * a20_handle_remove — detach the entry and drop its object reference.
 * The object release runs after the table lock is dropped (the release path
 * may wake waiters or notify event queues).  Returns 0 on success or
 * -A20_ERR_BAD_HANDLE when the handle is not live.
 */
int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h)
{
    void *obj = NULL;
    uint16_t type = A20_OBJ_INVALID;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
    if (h < ht->capacity && ht->entries[h].object != NULL &&
        ht->entries[h].state != A20_HS_FREE && ht->entries[h].state != A20_HS_CLOSING) {
        obj = ht->entries[h].object;
        type = ht->entries[h].type;
        ht->entries[h].object = NULL;
        ht->entries[h].type = A20_OBJ_INVALID;
        ht->entries[h].rights = 0;
        ht->entries[h].state = A20_HS_FREE;
        ht_free_slot(ht, h);
        ht->count--;
        a20_objstat_add(&g_a20_objstats.handles, -1);
    }
    spin_unlock_irqrestore(&ht->lock, flags);

    if (!obj)
        return -A20_ERR_BAD_HANDLE;
    a20_object_release(obj, type);
    return A20_OK;
}

/* ---- Reserve/abort/commit (docs/native-abi/03-handle.md §2.5) ----
 * channel_recv uses this to guarantee atomic delivery: slots are reserved
 * before the message is dequeued, so a full table yields NO_SPACE with the
 * message still queued instead of a partial delivery.
 */
int64_t a20_handle_reserve_many(struct a20_ht_internal *ht,
                                a20_handle_t *handles, uint32_t count)
{
    if (count == 0) return A20_OK;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
    uint32_t free_slots = ht->capacity - ht->count;
    if (free_slots < count) {
        if (ht_grow(ht) == 0)
            free_slots = ht->capacity - ht->count;
        if (free_slots < count) {
            spin_unlock_irqrestore(&ht->lock, flags);
            return -A20_ERR_NO_SPACE;
        }
    }

    uint32_t done = 0;
    for (uint32_t i = 0; i < count; i++) {
        int slot = ht_alloc_slot(ht);
        if (slot < 0) break;
        ht->entries[slot].state = A20_HS_CLOSING; /* reserved: invisible to lookup */
        ht->count++;
        handles[i] = (a20_handle_t)slot;
        done++;
    }
    spin_unlock_irqrestore(&ht->lock, flags);

    if (done < count) {
        a20_handle_abort_reserved(ht, handles, done);
        return -A20_ERR_NO_SPACE;
    }
    return A20_OK;
}

void a20_handle_abort_reserved(struct a20_ht_internal *ht,
                               a20_handle_t *handles, uint32_t count)
{
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    for (uint32_t i = 0; i < count; i++) {
        a20_handle_t h = handles[i];
        if (h >= ht->capacity) continue;
        a20_handle_entry_t *e = &ht->entries[h];
        if (e->state != A20_HS_CLOSING || e->object != NULL) continue;
        e->state = A20_HS_FREE;
        ht_free_slot(ht, h);
        ht->count--;
    }
    spin_unlock_irqrestore(&ht->lock, flags);
}

int64_t a20_handle_commit_reserved_temporal(struct a20_ht_internal *ht,
                                            a20_handle_t h, void *object,
                                            uint16_t type, a20_rights_t rights,
                                            uint64_t expiry_tick,
                                            uint32_t remaining_ops,
                                            uint32_t temporal_flags,
                                            uint8_t security_label)
{
    a20_rights_t valid = a20_type_valid_rights(type);
    if ((rights & ~valid) != 0)
        rights &= valid;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
    if (h >= ht->capacity ||
        ht->entries[h].state != A20_HS_CLOSING || ht->entries[h].object != NULL) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_BAD_HANDLE;
    }
    a20_handle_entry_t *e = &ht->entries[h];
    e->object = object;
    e->type = type;
    e->rights = rights;
    e->expiry_tick = expiry_tick;
    e->remaining_ops = remaining_ops;
    e->temporal_flags = temporal_flags;
    e->security_label = security_label;
    e->state = A20_HS_ACTIVE;
    spin_unlock_irqrestore(&ht->lock, flags);
    return A20_OK;
}

struct a20_ht_internal *task_get_a20_ht(task_t *t)
{
    return t ? (struct a20_ht_internal *)
        __atomic_load_n(&t->a20_ht, __ATOMIC_ACQUIRE) : NULL;
}

/* Obtain a stable table reference for another task. The registry lock
 * serializes against a20_ht_destroy() between loading a20_ht and
 * incrementing the table refcount. */
struct a20_ht_internal *task_get_a20_ht_ref(task_t *t)
{
    if (!t) return NULL;
    uint64_t flags = spin_lock_irqsave(&g_a20_ht_registry_lock);
    struct a20_ht_internal *ht = (struct a20_ht_internal *)
        __atomic_load_n(&t->a20_ht, __ATOMIC_ACQUIRE);
    if (ht && !refcount_inc_not_zero(&ht->refcount))
        ht = NULL;
    spin_unlock_irqrestore(&g_a20_ht_registry_lock, flags);
    return ht;
}

/* ---- Temporal constraint control (docs/native-abi/06-security.md §6.4) ----
 * SET is strengthening-only (non-refreshability): flags can only be added,
 * an existing absolute expiry can only be lowered, and an existing
 * operation budget can only be decreased.
 */
#define A20_TEMPORAL_KNOWN_FLAGS \
    (A20_TEMPORAL_EXPIRY_ABSOLUTE | A20_TEMPORAL_OP_COUNT | A20_TEMPORAL_AUTO_CLOSE)

int64_t a20_handle_set_temporal(struct a20_ht_internal *ht, a20_handle_t h,
                                uint64_t expiry_tick, uint32_t remaining_ops,
                                uint32_t temporal_flags)
{
    if (temporal_flags & ~A20_TEMPORAL_KNOWN_FLAGS)
        return -A20_ERR_INVALID_ARGUMENT;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
    if (h >= ht->capacity) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_BAD_HANDLE;
    }
    a20_handle_entry_t *e = &ht->entries[h];
    if (e->object == NULL || e->state != A20_HS_ACTIVE) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_BAD_HANDLE;
    }

    /* No flag may be cleared once set. */
    if ((e->temporal_flags & ~temporal_flags) != 0) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_ACCESS;
    }
    /* An existing expiry can only be lowered (never extended or removed). */
    if ((e->temporal_flags & A20_TEMPORAL_EXPIRY_ABSOLUTE) && e->expiry_tick > 0 &&
        (expiry_tick == 0 || expiry_tick > e->expiry_tick)) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_ACCESS;
    }
    /* An existing operation budget can only be decreased. */
    if ((e->temporal_flags & A20_TEMPORAL_OP_COUNT) &&
        remaining_ops > e->remaining_ops) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_ACCESS;
    }

    if (temporal_flags & A20_TEMPORAL_EXPIRY_ABSOLUTE)
        e->expiry_tick = expiry_tick;
    if (temporal_flags & A20_TEMPORAL_OP_COUNT)
        e->remaining_ops = remaining_ops;
    e->temporal_flags = temporal_flags;
    spin_unlock_irqrestore(&ht->lock, flags);

    /* Wake the sweeper at the relevant tick: at the expiry itself, or one
     * cadence ahead for op-count-only constraints. */
    if ((temporal_flags & A20_TEMPORAL_EXPIRY_ABSOLUTE) && expiry_tick > 0)
        sched_note_timer_deadline(expiry_tick);
    else if (temporal_flags != 0)
        sched_note_timer_deadline(timer_get_ticks() + A20_SWEEP_INTERVAL_TICKS);
    return A20_OK;
}

int64_t a20_handle_get_temporal(struct a20_ht_internal *ht, a20_handle_t h,
                                uint64_t *expiry_tick, uint32_t *remaining_ops,
                                uint32_t *temporal_flags)
{
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    if (h >= ht->capacity) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_BAD_HANDLE;
    }
    a20_handle_entry_t *e = &ht->entries[h];
    if (e->object == NULL || e->state == A20_HS_FREE || e->state == A20_HS_CLOSING) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_BAD_HANDLE;
    }
    *expiry_tick = e->expiry_tick;
    *remaining_ops = e->remaining_ops;
    *temporal_flags = e->temporal_flags;
    spin_unlock_irqrestore(&ht->lock, flags);
    return A20_OK;
}

/*
 * a20_handle_set_label — raise a handle entry's Bell-LaPadula label.
 * Raising is the restricting direction (it can only remove readability /
 * writability for lower-label processes), so labels are raise-only.
 */
int64_t a20_handle_set_label(struct a20_ht_internal *ht, a20_handle_t h,
                             uint8_t label)
{
    if (label > 2) return -A20_ERR_INVALID_ARGUMENT;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
    if (h >= ht->capacity) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_BAD_HANDLE;
    }
    a20_handle_entry_t *e = &ht->entries[h];
    if (e->object == NULL || e->state != A20_HS_ACTIVE) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_BAD_HANDLE;
    }
    if (label < e->security_label) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_ACCESS;
    }
    e->security_label = label;
    spin_unlock_irqrestore(&ht->lock, flags);
    return A20_OK;
}

/* ---- Temporal sweeper docs/native-abi/03-handle.md §3.1, §5) ---- */

#define A20_SWEEP_BATCH 16

void a20_temporal_sweep(struct a20_ht_internal *ht)
{
    if (!ht) return;

    /*
     * Expired entries are collected in batches so the object release
     * (channel peer_closed, eventq teardown, vfs_close) never runs under
     * the table lock.  AUTO_CLOSE entries are detached and freed; plain
     * expiry transitions to EXPIRED and is reaped by handle_close or
     * table destroy.
     */
    for (;;) {
        void *objs[A20_SWEEP_BATCH];
        uint16_t types[A20_SWEEP_BATCH];
        uint32_t n = 0;
        uint64_t now = timer_get_ticks();

        uint64_t flags = spin_lock_irqsave(&ht->lock);
        for (uint32_t i = 0; i < ht->capacity && n < A20_SWEEP_BATCH; i++) {
            a20_handle_entry_t *e = &ht->entries[i];
            if (e->object == NULL || e->state != A20_HS_ACTIVE)
                continue;

            int expired = 0;

            if ((e->temporal_flags & A20_TEMPORAL_EXPIRY_ABSOLUTE) &&
                e->expiry_tick > 0 && now >= e->expiry_tick)
                expired = 1;

            if ((e->temporal_flags & A20_TEMPORAL_OP_COUNT) &&
                e->remaining_ops == 0)
                expired = 1;

            if (!expired) continue;

            if (e->temporal_flags & A20_TEMPORAL_AUTO_CLOSE) {
                objs[n] = e->object;
                types[n] = e->type;
                n++;
                e->object = NULL;
                e->type = A20_OBJ_INVALID;
                e->rights = 0;
                e->state = A20_HS_FREE;
                ht_free_slot(ht, i);
                ht->count--;
            } else {
                e->state = A20_HS_EXPIRED;
                e->rights = 0;
            }
        }
        spin_unlock_irqrestore(&ht->lock, flags);

        for (uint32_t i = 0; i < n; i++)
            a20_object_release(objs[i], types[i]);

        if (n < A20_SWEEP_BATCH)
            break;
    }
}

/*
 * a20_temporal_sweep_all — periodic entry point, called from the scheduler
 * tick path (sched(), process context).  Registry is scanned with the
 * registry lock held; per-table sweeps take the per-table lock, which is
 * safe because the lock order registry → table is respected everywhere.
 */
void a20_temporal_sweep_all(void)
{
    uint64_t flags = spin_lock_irqsave(&g_a20_ht_registry_lock);
    struct a20_ht_internal *ht = g_a20_ht_registry;
    if (ht && !refcount_inc_not_zero(&ht->refcount))
        ht = NULL;
    spin_unlock_irqrestore(&g_a20_ht_registry_lock, flags);

    while (ht) {
        flags = spin_lock_irqsave(&g_a20_ht_registry_lock);
        struct a20_ht_internal *next = ht->registry_next;
        if (next && !refcount_inc_not_zero(&next->refcount))
            next = NULL;
        spin_unlock_irqrestore(&g_a20_ht_registry_lock, flags);

        a20_temporal_sweep(ht);
        a20_ht_put_ref(ht);
        ht = next;
    }
}

/* ------------------------------------------------------------------ */
/* Checkpoint-based signal simulation                                  */
/* ------------------------------------------------------------------ */

/* Queue signal @sig for a process (bitmap in its handle table). */
void a20_ht_sig_pend(struct a20_ht_internal *ht, int sig)
{
    if (!ht || sig <= 0 || sig >= 64)
        return;
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    ht->sig_pending |= (1ULL << sig);
    spin_unlock_irqrestore(&ht->lock, flags);
}

/* Return the deliverable signals (pending & ~blocked) and clear them.
 * Blocked signals stay pending until unblocked. */
uint64_t a20_ht_sig_take(struct a20_ht_internal *ht)
{
    if (!ht)
        return 0;
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    uint64_t deliver = ht->sig_pending & ~ht->sig_blocked;
    ht->sig_pending &= ht->sig_blocked;
    spin_unlock_irqrestore(&ht->lock, flags);
    return deliver;
}

uint64_t a20_ht_sig_blocked(struct a20_ht_internal *ht)
{
    if (!ht)
        return 0;
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    uint64_t m = ht->sig_blocked;
    spin_unlock_irqrestore(&ht->lock, flags);
    return m;
}

uint64_t a20_ht_sig_set_blocked(struct a20_ht_internal *ht, uint64_t mask)
{
    if (!ht)
        return 0;
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    uint64_t old = ht->sig_blocked;
    ht->sig_blocked = mask & ~(1ULL << 9);   /* SIGKILL cannot be blocked */
    spin_unlock_irqrestore(&ht->lock, flags);
    return old;
}
