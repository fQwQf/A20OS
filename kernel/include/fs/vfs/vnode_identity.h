#ifndef _FS_VFS_VNODE_IDENTITY_H
#define _FS_VFS_VNODE_IDENTITY_H

#include "core/types.h"

/*
 * Canonical (superblock, inode) -> vnode identity index.
 *
 * One live vnode per (sb, ino) pair.  The index stores opaque handle tokens
 * and owns exactly one vnode reference per live entry; it never dereferences
 * a vnode.  The adapter that owns the mount (currently uxfs) serialises every
 * entry point with its own IRQ-save spinlock and owns vnode allocation,
 * release, and the caller-side references described below.
 *
 * Reference contract (adapter side), all under the adapter's spinlock:
 *  - lookup/insert report a vnode whose entry owns one reference; the adapter
 *    takes its own caller reference with vnode_get() before unlocking;
 *  - on INSERTED the adapter vnode_get()s the freshly created vnode once so
 *    the entry owns exactly one reference and the caller owns one;
 *  - on DUPLICATE the adapter must use `existing` and dispose the candidate
 *    object it wanted to insert;
 *  - remove/clear_sb/prune_batch hand the entry's reference to the caller,
 *    which must vnode_put() it *outside* the lock (put may take mutexes and
 *    free the vnode);
 *  - on FULL nothing changed and the caller decides whether to retry.
 *
 * This header declares the Rust implementation ABI
 * (kernel/fs/vfs/vnode_identity.rs, selected by
 * CONFIG_UXFS_VNODE_IDENTITY_RUST).  The C reference model with identical
 * semantics lives in vnode_identity_c.h and is selected otherwise.
 */

enum {
    A20_VNODE_IDENTITY_INSERTED  = 0,
    A20_VNODE_IDENTITY_DUPLICATE = 1,
    A20_VNODE_IDENTITY_FULL      = 2,
};

typedef struct a20_vnode_identity_insert_out {
    int32_t   status;
    uintptr_t existing;
} a20_vnode_identity_insert_out_t;

_Static_assert(__builtin_offsetof(a20_vnode_identity_insert_out_t, existing)
                   == sizeof(uintptr_t),
               "vnode identity insert-out layout");
_Static_assert(sizeof(a20_vnode_identity_insert_out_t) == 2 * sizeof(uintptr_t),
               "vnode identity insert-out size");

/*
 * Non-blocking total-reference-count reader for the cache-only eviction
 * decision.  It must be callable with the adapter's spinlock held, must not
 * sleep and must not re-enter this module; it returns 0 for a null handle.
 */
typedef uintptr_t (*a20_vnode_identity_ref_peek_fn)(uintptr_t vnode);

#ifdef CONFIG_UXFS_VNODE_IDENTITY_RUST
/* Provided by the Rust object; every entry point requires the adapter's
 * spinlock to be held.  The registration is idempotent and may be repeated. */
void a20_vnode_identity_set_ref_peek(a20_vnode_identity_ref_peek_fn peek);
/* Returns the vnode whose entry owns a reference, or 0. */
uintptr_t a20_vnode_identity_lookup(uintptr_t sb, uint64_t ino);
/* See the status contract above; `out` must be writable. */
void a20_vnode_identity_insert(uintptr_t sb, uint64_t ino, uintptr_t vnode,
                               a20_vnode_identity_insert_out_t *out);
/* Returns the removed entry's vnode, or 0 when the key was absent. */
uintptr_t a20_vnode_identity_remove(uintptr_t sb, uint64_t ino);
/* Removes up to `cap` entries of `sb`; returns the count written to `out`.
 * Call repeatedly until it returns 0. */
size_t a20_vnode_identity_clear_sb(uintptr_t sb, uintptr_t *out, size_t cap);
/* Collects up to `cap` cache-only entries (refcount <= 1) from a rotating
 * cursor; returns the count written to `out`. */
size_t a20_vnode_identity_prune_batch(uintptr_t *out, size_t cap);
/* Live entry count (diagnostics/tests). */
size_t a20_vnode_identity_count(void);
#endif

#endif /* _FS_VFS_VNODE_IDENTITY_H */
