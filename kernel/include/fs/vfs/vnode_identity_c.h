#ifndef _FS_VFS_VNODE_IDENTITY_C_H
#define _FS_VFS_VNODE_IDENTITY_C_H

#include "fs/vfs/vnode_identity.h"

/*
 * C reference model for the canonical vnode identity index.  It is selected
 * only by the explicit UXFS_VNODE_IDENTITY_IMPL=c regression/oracle selector;
 * the Rust module (kernel/fs/vfs/vnode_identity.rs) is the default production
 * path.  Same algorithm, limits and statuses as the Rust implementation:
 * fixed-size open hash table over (sb, ino), a free-slot chain, and a rotating
 * cursor for cache-only eviction.  Header-only so the C-only regression build
 * needs no extra object; byte-identical semantics are what make the host
 * differential harness meaningful.
 *
 * As with the Rust side, every entry point requires the adapter's IRQ-save
 * spinlock; unlike the Rust static initializer this model lazily initialises
 * its state, so every entry point funnels through a20_vnode_identity_c_init()
 * (a single already-initialised check after the first call).
 */

#define A20_VNODE_IDENTITY_C_MAX_ENTRIES  16384
#define A20_VNODE_IDENTITY_C_HASH_BUCKETS 8192

typedef struct a20_vnode_identity_c_entry {
    uintptr_t sb;
    uint64_t  ino;
    uintptr_t vnode;
    int32_t   hash_next;
    int32_t   free_next;
    uint8_t   used;
} a20_vnode_identity_c_entry_t;

typedef struct a20_vnode_identity_c_state {
    a20_vnode_identity_c_entry_t entries[A20_VNODE_IDENTITY_C_MAX_ENTRIES];
    int32_t  buckets[A20_VNODE_IDENTITY_C_HASH_BUCKETS];
    int32_t  free_head;
    size_t   count;
    size_t   prune_cursor;
    int      initialized;
    a20_vnode_identity_ref_peek_fn ref_peek;
} a20_vnode_identity_c_state_t;

static inline a20_vnode_identity_c_state_t *a20_vnode_identity_c_state(void)
{
    static a20_vnode_identity_c_state_t state;
    return &state;
}

static inline uint32_t a20_vnode_identity_c_hash(uintptr_t sb, uint64_t ino)
{
    uint64_t h = (uint64_t)sb * 0x9E3779B97F4A7C15ull ^ ino;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return (uint32_t)h & (A20_VNODE_IDENTITY_C_HASH_BUCKETS - 1);
}

static inline void a20_vnode_identity_c_init(void)
{
    a20_vnode_identity_c_state_t *s = a20_vnode_identity_c_state();
    if (s->initialized)
        return;
    for (uint32_t i = 0; i < A20_VNODE_IDENTITY_C_MAX_ENTRIES; i++) {
        a20_vnode_identity_c_entry_t *e = &s->entries[i];
        e->sb = 0;
        e->ino = 0;
        e->vnode = 0;
        e->hash_next = -1;
        e->free_next = (i + 1 < A20_VNODE_IDENTITY_C_MAX_ENTRIES)
                           ? (int32_t)(i + 1)
                           : -1;
        e->used = 0;
    }
    for (uint32_t b = 0; b < A20_VNODE_IDENTITY_C_HASH_BUCKETS; b++)
        s->buckets[b] = -1;
    s->free_head = 0;
    s->count = 0;
    s->prune_cursor = 0;
    s->initialized = 1;
}

static inline int32_t
a20_vnode_identity_c_find_locked(a20_vnode_identity_c_state_t *s,
                                 uintptr_t sb, uint64_t ino)
{
    int32_t i = s->buckets[a20_vnode_identity_c_hash(sb, ino)];
    while (i >= 0) {
        const a20_vnode_identity_c_entry_t *e = &s->entries[i];
        if (e->used && e->sb == sb && e->ino == ino)
            return i;
        i = e->hash_next;
    }
    return -1;
}

static inline void
a20_vnode_identity_c_unlink_hash(a20_vnode_identity_c_state_t *s, uint32_t slot)
{
    uint32_t bucket = a20_vnode_identity_c_hash(s->entries[slot].sb,
                                                s->entries[slot].ino);
    int32_t next = s->entries[slot].hash_next;
    int32_t prev = -1;
    int32_t cur = s->buckets[bucket];
    while (cur >= 0) {
        if (cur == (int32_t)slot) {
            if (prev >= 0)
                s->entries[prev].hash_next = next;
            else
                s->buckets[bucket] = next;
            break;
        }
        prev = cur;
        cur = s->entries[cur].hash_next;
    }
    s->entries[slot].hash_next = -1;
}

static inline uintptr_t
a20_vnode_identity_c_detach(a20_vnode_identity_c_state_t *s, uint32_t slot)
{
    a20_vnode_identity_c_unlink_hash(s, slot);
    a20_vnode_identity_c_entry_t *e = &s->entries[slot];
    uintptr_t vnode = e->vnode;
    e->used = 0;
    e->sb = 0;
    e->ino = 0;
    e->vnode = 0;
    e->free_next = s->free_head;
    s->free_head = (int32_t)slot;
    s->count -= 1;
    return vnode;
}

static inline uintptr_t a20_vnode_identity_lookup_c(uintptr_t sb, uint64_t ino)
{
    a20_vnode_identity_c_init();
    a20_vnode_identity_c_state_t *s = a20_vnode_identity_c_state();
    int32_t slot = a20_vnode_identity_c_find_locked(s, sb, ino);
    return slot < 0 ? 0 : s->entries[slot].vnode;
}

static inline void
a20_vnode_identity_insert_c(uintptr_t sb, uint64_t ino, uintptr_t vnode,
                            a20_vnode_identity_insert_out_t *out)
{
    if (!out)
        return;
    a20_vnode_identity_c_init();
    a20_vnode_identity_c_state_t *s = a20_vnode_identity_c_state();
    int32_t dup = a20_vnode_identity_c_find_locked(s, sb, ino);
    if (dup >= 0) {
        out->status = A20_VNODE_IDENTITY_DUPLICATE;
        out->existing = s->entries[dup].vnode;
        return;
    }
    int32_t slot = s->free_head;
    if (slot < 0) {
        out->status = A20_VNODE_IDENTITY_FULL;
        out->existing = 0;
        return;
    }
    s->free_head = s->entries[slot].free_next;
    uint32_t bucket = a20_vnode_identity_c_hash(sb, ino);
    a20_vnode_identity_c_entry_t *e = &s->entries[slot];
    e->used = 1;
    e->sb = sb;
    e->ino = ino;
    e->vnode = vnode;
    e->hash_next = s->buckets[bucket];
    e->free_next = -1;
    s->buckets[bucket] = slot;
    s->count += 1;
    out->status = A20_VNODE_IDENTITY_INSERTED;
    out->existing = vnode;
}

static inline uintptr_t a20_vnode_identity_remove_c(uintptr_t sb, uint64_t ino)
{
    a20_vnode_identity_c_init();
    a20_vnode_identity_c_state_t *s = a20_vnode_identity_c_state();
    int32_t slot = a20_vnode_identity_c_find_locked(s, sb, ino);
    return slot < 0 ? 0 : a20_vnode_identity_c_detach(s, (uint32_t)slot);
}

static inline size_t a20_vnode_identity_clear_sb_c(uintptr_t sb, uintptr_t *out,
                                                   size_t cap)
{
    a20_vnode_identity_c_init();
    a20_vnode_identity_c_state_t *s = a20_vnode_identity_c_state();
    if (!out || cap == 0)
        return 0;
    size_t n = 0;
    for (uint32_t i = 0; i < A20_VNODE_IDENTITY_C_MAX_ENTRIES && n < cap; i++) {
        if (s->entries[i].used && s->entries[i].sb == sb)
            out[n++] = a20_vnode_identity_c_detach(s, i);
    }
    return n;
}

static inline size_t a20_vnode_identity_prune_batch_c(uintptr_t *out, size_t cap)
{
    a20_vnode_identity_c_init();
    a20_vnode_identity_c_state_t *s = a20_vnode_identity_c_state();
    if (!out || cap == 0 || !s->ref_peek)
        return 0;
    size_t n = 0;
    size_t scanned = 0;
    while (scanned < A20_VNODE_IDENTITY_C_MAX_ENTRIES && n < cap) {
        size_t i = s->prune_cursor;
        s->prune_cursor = (i + 1 == A20_VNODE_IDENTITY_C_MAX_ENTRIES) ? 0
                                                                     : i + 1;
        scanned++;
        if (!s->entries[i].used)
            continue;
        uintptr_t vnode = s->entries[i].vnode;
        /* Cache-only means the entry owns the vnode's only reference. */
        if (s->ref_peek(vnode) <= 1)
            out[n++] = a20_vnode_identity_c_detach(s, (uint32_t)i);
    }
    return n;
}

static inline size_t a20_vnode_identity_count_c(void)
{
    a20_vnode_identity_c_state_t *s = a20_vnode_identity_c_state();
    return s->initialized ? s->count : 0;
}

static inline void
a20_vnode_identity_set_ref_peek_c(a20_vnode_identity_ref_peek_fn peek)
{
    a20_vnode_identity_c_init();
    a20_vnode_identity_c_state()->ref_peek = peek;
}

#endif /* _FS_VFS_VNODE_IDENTITY_C_H */
