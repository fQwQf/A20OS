#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
/*
 * A20OS Native ABI — Handle table implementation.
 * Design reference: docs/zh/native-abi/03-handle.md §2
 * Design inspiration: unified kernel-object handles with a rights model and
 * capability-transfer semantics; see docs/zh/ACKNOWLEDGMENTS.md §3.
 * NATIVE_HANDLE_CAPABILITY_TEST_CONTRACT: lookup/install/remove paths are the
 * checked surface for rights downgrade, temporal limits, labels, close/dup/
 * transfer, and partial-delivery consistency gates.
 */
#include "core/types.h"
#include "uapi/a20/task_signal.h"
#include "core/string.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/klog.h"
#include "core/panic.h"
#include "core/defs.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "fs/vfs.h"
#include "fs/locks.h"
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
#include "proc/signal_core.h"
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
#include "handle_table_state.h"
_Static_assert(sizeof(a20_handle_entry_t) == (sizeof(void *) == 8 ? 48 : 40), "handle entry ABI size mismatch");
/* AAPCS32 and RV32-ILP32D align the uint64_t members to eight while pointers
 * stay four bytes wide; __alignof__(uint64_t) is the ABI property that
 * distinguishes them from ILP32 ABIs packing 64-bit aggregates to four, so
 * the C and Rust views can be compared without naming an architecture. */
_Static_assert(__alignof__(a20_handle_entry_t) == __alignof__(uint64_t), "handle entry ABI alignment mismatch");
_Static_assert(__builtin_offsetof(a20_handle_entry_t, type) == sizeof(void *), "handle entry type offset mismatch");
_Static_assert(__builtin_offsetof(a20_handle_entry_t, rights) == (sizeof(void *) == 8 ? 16 : 8), "handle entry rights offset mismatch");
_Static_assert(__builtin_offsetof(a20_handle_entry_t, expiry_tick) == (sizeof(void *) == 8 ? 24 : 16), "handle entry expiry offset mismatch");
_Static_assert(__builtin_offsetof(a20_handle_entry_t, remaining_ops) == (sizeof(void *) == 8 ? 32 : 24), "handle entry budget offset mismatch");
_Static_assert(__builtin_offsetof(a20_handle_entry_t, security_label) == (sizeof(void *) == 8 ? 40 : 32), "handle entry label offset mismatch");
_Static_assert(__builtin_offsetof(a20_handle_entry_t, state) == (sizeof(void *) == 8 ? 41 : 33), "handle entry state offset mismatch");
#endif

struct a20_ht_internal {
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    a20_rust_handle_table_state_t *rust_state;
#else
    a20_handle_entry_t *entries;
    uint32_t            capacity;
    uint32_t            count;
    uint32_t            free_hint;
    uint64_t           *free_bitmap;
    uint32_t            bitmap_size;
    uint32_t            max_handles;
#endif
    spinlock_t          lock;
    uint8_t             security_label; /* Bell-LaPadula process label: 0=L, 1=M, 2=H */
    refcount_t          refcount;       /* shared by all threads of a process */
    struct a20_ht_internal *registry_next; /* global sweeper registry */
    /* Checkpoint-based signal simulation (native ABI has no async signals).
     * bit n == signal n; SIGKILL (9) is handled immediately, never queued. */
    uint64_t            sig_pending;
    uint64_t            sig_blocked;
};

#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
static void *ht_state_alloc(size_t bytes) { return kmalloc(bytes); }
static void ht_state_free(void *ptr) { kfree(ptr); }

static inline uint32_t ht_capacity(const struct a20_ht_internal *ht)
{
    return a20_rust_handle_table_capacity(ht->rust_state);
}

#else
static inline uint32_t ht_capacity(const struct a20_ht_internal *ht) { return ht->capacity; }
#endif

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
 * a20_blp_read_ok  — No Read Up (docs/zh/native-abi/06-security.md §5.2).
 * Process label must dominate object label: ℓ(p) ≥ ℓ(o).
 */
static inline int a20_blp_read_ok(uint8_t proc_label, uint8_t obj_label)
{
    return proc_label >= obj_label;
}

/*
 * a20_blp_write_ok — No Write Down (docs/zh/native-abi/06-security.md §5.2).
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

#ifndef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
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
#endif

static const a20_rights_t a20_type_rights[A20_OBJ_HYP_VCPU + 1] = {
    [A20_OBJ_INVALID]          = 0,
    [A20_OBJ_HYP_VM] = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_CONTROL |
                      A20_RIGHT_STAT | A20_RIGHT_DUP | A20_RIGHT_TRANSFER,
    [A20_OBJ_HYP_VCPU] = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_CONTROL |
                        A20_RIGHT_STAT | A20_RIGHT_DUP | A20_RIGHT_TRANSFER,
    [A20_OBJ_TASK]             = A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT |
                                 A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL |
                                 A20_RIGHT_ADMIN | A20_RIGHT_READ | A20_RIGHT_WRITE,
    /* Threads are task-like for the native ABI: same default ceiling as
     * TASK so join/signal/debug work on THREAD handles (09 §8 rollout). */
    [A20_OBJ_THREAD]           = A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT |
                                 A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL |
                                 A20_RIGHT_ADMIN | A20_RIGHT_READ | A20_RIGHT_WRITE,
    [A20_OBJ_FILE]             = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                                 A20_RIGHT_SEEK | A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_EXEC | A20_RIGHT_MAP | A20_RIGHT_CONTROL,
    [A20_OBJ_DIRECTORY]        = A20_RIGHT_READ | A20_RIGHT_STAT | A20_RIGHT_DUP |
                                 A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL,
    [A20_OBJ_SOCKET]           = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                                 A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_CONNECT | A20_RIGHT_ACCEPT | A20_RIGHT_CONTROL,
    [A20_OBJ_PIPE_ENDPOINT]    = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                                 A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL,
    [A20_OBJ_CHANNEL_ENDPOINT] = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                                 A20_RIGHT_DUP | A20_RIGHT_TRANSFER,
    [A20_OBJ_EVENT_QUEUE]      = A20_RIGHT_READ | A20_RIGHT_STAT | A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_CONTROL,
    [A20_OBJ_TIMER]            = A20_RIGHT_STAT | A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_CONTROL,
    [A20_OBJ_MEMORY]           = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_EXEC |
                                 A20_RIGHT_MAP |
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
    [A20_OBJ_PAGER]            = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                                 A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL,
    [A20_OBJ_VMAR]             = A20_RIGHT_MAP | A20_RIGHT_STAT | A20_RIGHT_DUP |
                                 A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL,
    [A20_OBJ_MONITOR]          = A20_RIGHT_READ | A20_RIGHT_STAT | A20_RIGHT_DUP |
                                 A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL,
};

a20_rights_t a20_type_valid_rights(uint16_t type)
{
#ifdef CONFIG_NATIVE_TASK_HANDLES_ONLY
    if (type != A20_OBJ_TASK && type != A20_OBJ_THREAD
#ifdef CONFIG_NATIVE_FLAT_FILES
        && type != A20_OBJ_FILE
#endif
    )
        return 0;
#endif
    if (type > A20_OBJ_HYP_VCPU) return 0;
    return a20_type_rights[type];
}

struct a20_ht_internal *a20_ht_create(void)
{
    struct a20_ht_internal *ht = kmalloc(sizeof(*ht));
    if (!ht) return NULL;
    /* kmalloc may return another process's storage: initialize checkpoint
     * state as well as slots before publishing this table. */
    memset(ht, 0, sizeof(*ht));

    ht->security_label = 0; /* default: L (docs/zh/native-abi/06-security.md §5.1) */
    refcount_set(&ht->refcount, 1);
    spin_init(&ht->lock);
    spin_set_debug(&ht->lock, "a20_handle_table", ht);
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    uint32_t initial_bitmap_words = (A20_HT_INITIAL_CAP + 63) / 64;
    a20_handle_entry_t *initial_entries = kmalloc(
        A20_HT_INITIAL_CAP * sizeof(a20_handle_entry_t));
    uint64_t *initial_bitmap = kmalloc(initial_bitmap_words * sizeof(uint64_t));
    size_t state_size = a20_rust_handle_table_state_size();
    size_t state_align = a20_rust_handle_table_state_align();
    ht->rust_state = state_size && state_align && (state_align & (state_align - 1)) == 0
        ? kmalloc(state_size) : NULL;
    if (!initial_entries || !initial_bitmap || !ht->rust_state ||
        ((uintptr_t)ht->rust_state & (state_align - 1)) != 0 ||
        a20_rust_handle_table_state_init(
            ht->rust_state, (uint8_t *)initial_entries,
            sizeof(a20_handle_entry_t), __alignof__(a20_handle_entry_t),
            A20_HT_INITIAL_CAP, initial_bitmap, initial_bitmap_words,
            A20_HT_MAX_CAP, A20_HT_DEFAULT_QUOTA,
            ht_state_alloc, ht_state_free) < 0) {
        kfree(initial_entries);
        kfree(initial_bitmap);
        kfree(ht->rust_state);
        kfree(ht);
        return NULL;
    }
#else
    ht->capacity = A20_HT_INITIAL_CAP;
    ht->count = 0;
    ht->free_hint = 0;
    ht->max_handles = A20_HT_DEFAULT_QUOTA;
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
#endif

    uint64_t rflags = spin_lock_irqsave(&g_a20_ht_registry_lock);
    ht->registry_next = g_a20_ht_registry;
    g_a20_ht_registry = ht;
    spin_unlock_irqrestore(&g_a20_ht_registry_lock, rflags);
    return ht;
}

#ifdef A20_HANDLE_SLOT_HOST_TEST
void a20_handle_host_set_quota(struct a20_ht_internal *ht, uint32_t quota)
{
    uint64_t flags = spin_lock_irqsave(&ht->lock);
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    (void)a20_rust_handle_table_set_quota(ht->rust_state, quota);
#else
    ht->max_handles = quota;
#endif
    spin_unlock_irqrestore(&ht->lock, flags);
}
#endif

void a20_ht_destroy(struct a20_ht_internal *ht)
{
    if (!ht) return;
#if !defined(CONFIG_MCU) && !defined(CONFIG_NATIVE_TASK_HANDLES_ONLY)
    fs_locks_release_native(ht, NULL);
#endif

#ifndef CONFIG_NATIVE_TASK_HANDLES_ONLY
    extern void a20_channel_trace_dump(void);
    extern void a20_channel_trace(uint32_t op, uint32_t len, void *p1,
                                  void *p2, uint32_t meta);
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    a20_channel_trace(9, 0, ht, ht->rust_state, 0);
    printf("[HT-DESTROY] ht=%p state=%p cap=%u refs=%d pid=%d\n",
           (void *)ht, (void *)ht->rust_state, ht_capacity(ht),
           (int)ht->refcount.value, proc_current() ? proc_current()->pid : -1);
#else
    a20_channel_trace(9, 0, ht, ht->entries, 0);
    printf("[HT-DESTROY] ht=%p entries=%p bitmap=%p cap=%u refs=%d pid=%d\n",
           (void *)ht, (void *)ht->entries, (void *)ht->free_bitmap,
           ht->capacity, (int)ht->refcount.value, proc_current() ? proc_current()->pid : -1);
#endif

#endif
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
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    uint32_t cursor = 0;
    void *destroyed_object = NULL;
    uint16_t destroyed_type = A20_OBJ_INVALID;
    while (a20_rust_handle_table_destroy_next(
               ht->rust_state, &cursor, &destroyed_object, &destroyed_type)) {
        if (destroyed_object) {
            a20_objstat_add(&g_a20_objstats.handles, -1);
            a20_object_release(destroyed_object, destroyed_type);
        }
        destroyed_object = NULL;
        destroyed_type = A20_OBJ_INVALID;
    }
    if (a20_rust_handle_table_state_fini(ht->rust_state) != 0)
        panic("handle table Rust state not empty at final teardown");
    kfree(ht->rust_state);
#else
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
#endif
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
    if (!ht || !object)
        return -A20_ERR_INVALID_ARGUMENT;

    a20_rights_t valid = a20_type_valid_rights(type);
    if ((rights & ~valid) != 0)
        rights &= valid;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    int64_t slot = a20_rust_handle_table_install(
        ht->rust_state, object, type, rights, 0, 0, 0, 0);
    if (slot >= 0)
        a20_objstat_add(&g_a20_objstats.handles, 1);
    spin_unlock_irqrestore(&ht->lock, flags);
    return slot;
#else
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
#endif
}

/*
 * a20_handle_install_temporal — install handle inheriting temporal constraints
 * from a source handle. Enforces non-refreshability (docs/zh/native-abi/06-security.md §6.4):
 * the new handle's expiry ≤ source expiry, ops ≤ source remaining_ops.
 */
int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                     uint16_t type, a20_rights_t rights,
                                     uint64_t expiry_tick, uint32_t remaining_ops,
                                     uint32_t temporal_flags, uint8_t security_label)
{
    if (!ht || !object)
        return -A20_ERR_INVALID_ARGUMENT;

    a20_rights_t valid = a20_type_valid_rights(type);
    if ((rights & ~valid) != 0)
        rights &= valid;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    int64_t slot = a20_rust_handle_table_install(
        ht->rust_state, object, type, rights, expiry_tick, remaining_ops,
        temporal_flags, security_label);
    if (slot >= 0)
        a20_objstat_add(&g_a20_objstats.handles, 1);
    spin_unlock_irqrestore(&ht->lock, flags);
    return slot;
#else
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
#endif
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
    if (!ht || !object || slot >= A20_HT_MAX_CAP)
        return -A20_ERR_INVALID_ARGUMENT;

    a20_rights_t valid = a20_type_valid_rights(type);
    if ((rights & ~valid) != 0)
        rights &= valid;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    int64_t result = a20_rust_handle_table_install_at(
        ht->rust_state, slot, object, type, rights, expiry_tick,
        remaining_ops, temporal_flags, security_label);
    if (result >= 0)
        a20_objstat_add(&g_a20_objstats.handles, 1);
    spin_unlock_irqrestore(&ht->lock, flags);
    return result;
#else
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
#endif
}

static int64_t a20_handle_lookup_ref_mode(struct a20_ht_internal *ht,
                                          a20_handle_t h,
                                          uint16_t expected_type,
                                          a20_rights_t required_rights,
                                          a20_handle_entry_t *out,
                                          int take_ref)
{
    uint64_t flags = spin_lock_irqsave(&ht->lock);
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    if (h >= ht_capacity(ht)) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_BAD_HANDLE;
    }
    int64_t rust_result = a20_rust_handle_table_lookup(
        ht->rust_state, h, expected_type, required_rights, a20_current_tick(), out);
    if (rust_result == A20_OK && take_ref)
        a20_object_ref(out->object, out->type);
    spin_unlock_irqrestore(&ht->lock, flags);
    return rust_result;
#else
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
#endif
}

/* Task-like lookup: accept TASK or THREAD handles.  Threads became a
 * distinct object type (09-native-abi-deepening §8); every syscall that
 * operates on "some runnable thing" must take both. */
int64_t a20_handle_lookup_task_like(struct a20_ht_internal *ht,
                                    a20_handle_t h,
                                    a20_rights_t required_rights,
                                    a20_handle_entry_t *out)
{
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_TASK,
                                           required_rights, out);
    if (r < 0)
        r = a20_handle_lookup_internal(ht, h, A20_OBJ_THREAD,
                                       required_rights, out);
    return r;
}

int64_t a20_handle_lookup_ref_task_like(struct a20_ht_internal *ht,
                                        a20_handle_t h,
                                        a20_rights_t required_rights,
                                        a20_handle_entry_t *out)
{
    int64_t r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_TASK,
                                               required_rights, out);
    if (r < 0)
        r = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_THREAD,
                                           required_rights, out);
    return r;
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
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    if (a20_rust_handle_table_remove(ht->rust_state, h, &obj, &type)) {
        a20_objstat_add(&g_a20_objstats.handles, -1);
    }
#else
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
#endif
    spin_unlock_irqrestore(&ht->lock, flags);

    if (!obj)
        return -A20_ERR_BAD_HANDLE;
#if !defined(CONFIG_MCU) && !defined(CONFIG_NATIVE_TASK_HANDLES_ONLY)
    if (type == A20_OBJ_FILE) fs_locks_release_native(ht, obj);
#endif
    a20_object_release(obj, type);
    return A20_OK;
}

/* ---- Reserve/abort/commit (docs/zh/native-abi/03-handle.md §2.5) ----
 * channel_recv uses this to guarantee atomic delivery: slots are reserved
 * before the message is dequeued, so a full table yields NO_SPACE with the
 * message still queued instead of a partial delivery.
 */
int64_t a20_handle_reserve_many(struct a20_ht_internal *ht,
                                a20_handle_t *handles, uint32_t count)
{
    if (count == 0) return A20_OK;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    int32_t rust_result = a20_rust_handle_table_reserve_many(
        ht->rust_state, count, (uint32_t *)handles);
    spin_unlock_irqrestore(&ht->lock, flags);
    return rust_result;
#else
    if (count > ht->max_handles || ht->count > ht->max_handles - count) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_NO_SPACE;
    }
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
#endif
}

void a20_handle_abort_reserved(struct a20_ht_internal *ht,
                               a20_handle_t *handles, uint32_t count)
{
    uint64_t flags = spin_lock_irqsave(&ht->lock);
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    (void)a20_rust_handle_table_abort_many(
        ht->rust_state, (const uint32_t *)handles, count);
    spin_unlock_irqrestore(&ht->lock, flags);
#else
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
#endif
}

int64_t a20_handle_commit_reserved_temporal(struct a20_ht_internal *ht,
                                            a20_handle_t h, void *object,
                                            uint16_t type, a20_rights_t rights,
                                            uint64_t expiry_tick,
                                            uint32_t remaining_ops,
                                            uint32_t temporal_flags,
                                            uint8_t security_label)
{
    if (!ht || !object)
        return -A20_ERR_INVALID_ARGUMENT;

    a20_rights_t valid = a20_type_valid_rights(type);
    if ((rights & ~valid) != 0)
        rights &= valid;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    if (a20_rust_handle_table_commit(ht->rust_state, h, object, type, rights,
                                     expiry_tick, remaining_ops,
                                     temporal_flags, security_label) != 0) {
        spin_unlock_irqrestore(&ht->lock, flags);
        return -A20_ERR_BAD_HANDLE;
    }
#else
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
#endif
    a20_objstat_add(&g_a20_objstats.handles, 1);
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

/* ---- Temporal constraint control (docs/zh/native-abi/06-security.md §6.4) ----
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
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    int64_t rust_result = a20_rust_handle_table_set_temporal(
        ht->rust_state, h, expiry_tick, remaining_ops, temporal_flags);
    spin_unlock_irqrestore(&ht->lock, flags);
    if (rust_result < 0)
        return rust_result;
#else
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
#endif

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
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    int64_t rust_result = a20_rust_handle_table_get_temporal(
        ht->rust_state, h, expiry_tick, remaining_ops, temporal_flags);
    spin_unlock_irqrestore(&ht->lock, flags);
    return rust_result;
#else
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
#endif
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
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
    int64_t rust_result = a20_rust_handle_table_set_label(
        ht->rust_state, h, label);
    spin_unlock_irqrestore(&ht->lock, flags);
    return rust_result;
#else
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
#endif
}

/* ---- Temporal sweeper docs/zh/native-abi/03-handle.md §3.1, §5) ---- */

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
        for (uint32_t i = 0; i < ht_capacity(ht) && n < A20_SWEEP_BATCH; i++) {
#ifdef CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
            void *detached_object = NULL;
            uint16_t detached_type = A20_OBJ_INVALID;
            if (a20_rust_handle_table_expire(ht->rust_state, i, now,
                                              &detached_object, &detached_type)) {
                objs[n] = detached_object;
                types[n] = detached_type;
                n++;
                a20_objstat_add(&g_a20_objstats.handles, -1);
            }
#else
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
                a20_objstat_add(&g_a20_objstats.handles, -1);
            } else {
                e->state = A20_HS_EXPIRED;
                e->rights = 0;
            }
#endif
        }
        spin_unlock_irqrestore(&ht->lock, flags);

        for (uint32_t i = 0; i < n; i++) {
#if !defined(CONFIG_MCU) && !defined(CONFIG_NATIVE_TASK_HANDLES_ONLY)
            if (types[i] == A20_OBJ_FILE) fs_locks_release_native(ht, objs[i]);
#endif
            a20_object_release(objs[i], types[i]);
        }

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
    a20_signal_native_ht_pend_locked(&ht->sig_pending, sig);
    spin_unlock_irqrestore(&ht->lock, flags);
}

/* Return the deliverable signals (pending & ~blocked) and clear them.
 * Blocked signals stay pending until unblocked. */
uint64_t a20_ht_sig_take(struct a20_ht_internal *ht)
{
    if (!ht)
        return 0;
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    uint64_t deliver = a20_signal_native_ht_take_locked(&ht->sig_pending,
                                                        ht->sig_blocked);
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
    ht->sig_blocked = a20_signal_native_ht_mask(mask);
    spin_unlock_irqrestore(&ht->lock, flags);
    return old;
}

/* Thread checkpoints deliberately share the table lock, so taking a process
 * signal and a thread signal is one transaction. No scheduler lock is taken. */
void a20_thread_sig_pend(task_t *task, int sig) {
    struct a20_ht_internal *ht = task_get_a20_ht_ref(task);
    if (!ht)
        return;
    if (sig <= 0 || sig >= 64) {
        a20_ht_put_ref(ht);
        return;
    }
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    a20_signal_native_thread_pend_locked(&task->a20_signal_pending, sig);
    spin_unlock_irqrestore(&ht->lock, flags);
    a20_ht_put_ref(ht);
}

uint64_t a20_thread_sig_check(task_t *task, uint32_t mode, uint64_t selected) {
    struct a20_ht_internal *ht = task_get_a20_ht(task);
    if (!ht)
        return 0;
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    /* Rust owns the checkpoint select+consume transaction (thread
     * occurrence preferred, lowest-signum SELECT consume). */
    uint64_t deliver = a20_signal_native_thread_check_locked(
        &ht->sig_pending, &task->a20_signal_pending, task->a20_signal_blocked,
        mode, selected);
    spin_unlock_irqrestore(&ht->lock, flags);
    return deliver;
}

uint64_t a20_thread_sig_mask(task_t *task, uint64_t mask, int query) {
    struct a20_ht_internal *ht = task_get_a20_ht(task);
    if (!ht)
        return 0;
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    uint64_t old = task->a20_signal_blocked;
    if (!query) {
        task->a20_signal_blocked = a20_signal_native_thread_mask(mask);
    }
    spin_unlock_irqrestore(&ht->lock, flags);
    return old;
}

#endif
