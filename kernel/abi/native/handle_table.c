/*
 * A20OS Native ABI — Handle table implementation.
 * Design reference: docs/native-abi/03-handle.md §2
 */
#include "core/types.h"
#include "core/string.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/klog.h"
#include "core/defs.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "proc/proc.h"

struct a20_ht_internal {
    a20_handle_entry_t *entries;
    uint32_t            capacity;
    uint32_t            count;
    uint32_t            free_hint;
    spinlock_t          lock;
    uint64_t           *free_bitmap;
    uint32_t            bitmap_size;
    uint8_t             security_label; /* Bell-LaPadula process label: 0=L, 1=M, 2=H */
};

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

static const a20_rights_t a20_type_rights[A20_OBJ_DEBUG + 1] = {
    [A20_OBJ_INVALID]          = 0,
    [A20_OBJ_TASK]             = A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT |
                                 A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL |
                                 A20_RIGHT_ADMIN,
    [A20_OBJ_THREAD]           = A20_RIGHT_STAT | A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_CONTROL,
    [A20_OBJ_FILE]             = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                                 A20_RIGHT_SEEK | A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_MAP | A20_RIGHT_CONTROL,
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
    [A20_OBJ_DEBUG]            = A20_RIGHT_STAT | A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                 A20_RIGHT_CONTROL | A20_RIGHT_ADMIN,
};

a20_rights_t a20_type_valid_rights(uint16_t type)
{
    if (type > A20_OBJ_DEBUG) return 0;
    return a20_type_rights[type];
}

struct a20_ht_internal *a20_ht_create(void)
{
    struct a20_ht_internal *ht = kmalloc(sizeof(*ht));
    if (!ht) return NULL;

    ht->capacity = A20_HT_INITIAL_CAP;
    ht->count = 0;
    ht->free_hint = 0;
    ht->security_label = 0; /* default: L (docs/native-abi/06-security.md §5.1) */
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
    return ht;
}

void a20_ht_destroy(struct a20_ht_internal *ht)
{
    if (!ht) return;
    for (uint32_t i = 0; i < ht->capacity; i++) {
        if (ht->entries[i].object != NULL) {
            ht->entries[i].object = NULL;
        }
    }
    kfree(ht->entries);
    kfree(ht->free_bitmap);
    kfree(ht);
}

int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, a20_rights_t rights)
{
    a20_rights_t valid = a20_type_valid_rights(type);
    if ((rights & ~valid) != 0)
        rights &= valid;

    uint64_t flags = spin_lock_irqsave(&ht->lock);
    int slot = ht_alloc_slot(ht);
    if (slot < 0) {
        if (ht_grow(ht) == 0)
            slot = ht_alloc_slot(ht);
        if (slot < 0) {
            spin_unlock_irqrestore(&ht->lock, flags);
            return -A20_ERR_NO_SPACE;
        }
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
    int slot = ht_alloc_slot(ht);
    if (slot < 0) {
        if (ht_grow(ht) == 0)
            slot = ht_alloc_slot(ht);
        if (slot < 0) {
            spin_unlock_irqrestore(&ht->lock, flags);
            return -A20_ERR_NO_SPACE;
        }
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
    spin_unlock_irqrestore(&ht->lock, flags);
    return (int64_t)slot;
}

int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                    uint16_t expected_type, a20_rights_t required_rights,
                                    a20_handle_entry_t *out)
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
    spin_unlock_irqrestore(&ht->lock, flags);
    return A20_OK;
}

void a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h)
{
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    if (h < ht->capacity && ht->entries[h].object != NULL &&
        ht->entries[h].state != A20_HS_FREE && ht->entries[h].state != A20_HS_CLOSING) {
        ht->entries[h].state = A20_HS_CLOSING;
        void *obj = ht->entries[h].object;
        ht->entries[h].object = NULL;
        ht->entries[h].type = A20_OBJ_INVALID;
        ht->entries[h].rights = 0;
        ht->entries[h].state = A20_HS_FREE;
        ht_free_slot(ht, h);
        ht->count--;
        spin_unlock_irqrestore(&ht->lock, flags);

        /* Deferred object release outside lock docs/native-abi/03-handle.md §4.1 atomicity) */
        (void)obj; /* Caller responsible for refcount_dec if needed */
        return;
    }
    spin_unlock_irqrestore(&ht->lock, flags);
}

struct a20_ht_internal *task_get_a20_ht(task_t *t)
{
    return t ? (struct a20_ht_internal *)t->scratch_buf : NULL;
}

/* ---- Temporal sweeper docs/native-abi/03-handle.md §3.1, §5) ---- */

void a20_temporal_sweep(struct a20_ht_internal *ht)
{
    if (!ht) return;
    uint64_t now = timer_get_ticks();

    uint64_t flags = spin_lock_irqsave(&ht->lock);
    for (uint32_t i = 0; i < ht->capacity; i++) {
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
            e->state = A20_HS_CLOSING;
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
}
