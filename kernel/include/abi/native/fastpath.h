/*
 * A20 syscall fastpath — inline helpers for hot-path syscall optimization.
 *
 * These inline functions avoid function-call overhead for the most
 * performance-critical operations: handle lookup and basic I/O.
 *
 * See Phase 5.1 in IMPLEMENTATION_TODO.md.
 */

#ifndef _A20_FASTPATH_H
#define _A20_FASTPATH_H

#include <kernel/include/abi/native/types.h>
#include <kernel/include/abi/native/errno.h>
#include <kernel/include/abi/native/rights.h>

/*
 * a20_fast_handle_lookup — inline handle lookup without lock acquisition.
 *
 * For read-only access (checking handle existence + type), we can do
 * a lock-free read of the handle table since:
 * 1. Handle table entries are pointer-aligned (64-bit).
 * 2. We only read, never write in this path.
 * 3. A torn read is safe — we validate the result under lock later.
 *
 * This is a speculative check; the real lookup still happens under lock
 * in the syscall handler, but this avoids the lock in the common case
 * (handle is obviously invalid).
 */
static inline int a20_fast_handle_valid(
    const struct a20_handle_entry *entries,
    uint32_t capacity,
    a20_handle_t handle)
{
    if (handle >= capacity) return 0;
    const struct a20_handle_entry *e = &entries[handle];
    if (e->type == A20_OBJ_INVALID) return 0;
    return 1;
}

/*
 * a20_fast_check_rights — inline rights subset check.
 *
 * Returns 1 if (have & required) == required, 0 otherwise.
 * This avoids a function call for the most common security check.
 */
static inline int a20_fast_check_rights(a20_rights_t have, a20_rights_t required)
{
    return (have & required) == required;
}

/*
 * a20_fast_iov_count — count total bytes in an iovec array.
 *
 * Returns -A20_ERR_INVALID_ARGUMENT if any iov has unreasonable values,
 * or the total byte count on success.
 */
static inline int64_t a20_fast_iov_count(
    const struct a20_iovec *iov,
    uint32_t count,
    uint64_t max_total)
{
    if (!iov || count == 0 || count > 256) return -A20_ERR_INVALID_ARGUMENT;
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (iov[i].len > max_total) return -A20_ERR_INVALID_ARGUMENT;
        total += iov[i].len;
        if (total > max_total) return -A20_ERR_INVALID_ARGUMENT;
    }
    return (int64_t)total;
}

/*
 * a20_fast_bitmap_scan — inline bitmap scan with free_hint optimization.
 *
 * Starts scanning from *hint, wraps around. This is the same algorithm
 * as a20_ht_alloc_slot but inlined for the fast path.
 *
 * Returns the bit index, or -1 if no free bit found.
 */
static inline int a20_fast_bitmap_scan(
    const uint64_t *bmp,
    uint32_t nbits,
    uint32_t *hint)
{
    uint32_t start = *hint;
    uint32_t idx = start;

    do {
        uint32_t word = idx >> 6;
        uint32_t bit  = idx & 63;
        if (word < (nbits >> 6) + 1) {
            if (!(bmp[word] & (1ULL << bit))) {
                *hint = idx;
                return (int)idx;
            }
        }
        idx++;
        if (idx >= nbits) idx = 0;
    } while (idx != start);

    return -1;
}

#endif /* _A20_FASTPATH_H */
