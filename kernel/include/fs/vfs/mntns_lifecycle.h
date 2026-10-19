#ifndef _FS_VFS_MNTNS_LIFECYCLE_H
#define _FS_VFS_MNTNS_LIFECYCLE_H

#include "core/types.h"

/*
 * Mount-namespace lifecycle state.
 *
 * The in-flight counters, the shutdown flag, the mutation/fileop admission
 * decisions, the shutdown begin/abort transitions and the teardown-pass policy
 * are owned by kernel/fs/vfs/mntns_lifecycle.rs.  mntns.c keeps the spinlock,
 * the namespace list, the shutdown snapshot array and the refcounts: it walks
 * the registry and pins the snapshot inside the `scan` callback, and collects
 * per-namespace teardown state for the retry selector.  Every entry point below
 * runs with the adapter's registry lock held.
 *
 * Keep the field order in sync with the Rust mirror.
 */

typedef struct mntns_lifecycle {
    uint32_t create_inflight;
    uint32_t mutations_inflight;
    uint32_t fileops_inflight;
    uint32_t shutdown_active;
    uint32_t shutdown_count;
    uint32_t shutdown_cap;
    uint64_t teardown_pass;
} mntns_lifecycle_t;

/* Validate the registry against `cap` and pin each namespace it reports, so a
 * 0 return means the pinned snapshot is complete. */
typedef int (*mntns_scan_fn)(void *ctx, uint32_t cap, uint32_t *out_count);

typedef struct mntns_retry_cand {
    uint32_t teardown_pending;
    uint32_t teardown_running;
    uint32_t retryable;
    uint32_t _pad;
    uint64_t attempt_pass;
} mntns_retry_cand_t;

void a20_mntns_lifecycle_init(mntns_lifecycle_t *st, uint32_t cap);
int  a20_mntns_create_enter(mntns_lifecycle_t *st);
uint32_t a20_mntns_create_leave(mntns_lifecycle_t *st);
int  a20_mntns_mutation_enter(mntns_lifecycle_t *st);
uint32_t a20_mntns_mutation_leave(mntns_lifecycle_t *st);
int  a20_mntns_fileop_enter(mntns_lifecycle_t *st);
uint32_t a20_mntns_fileop_leave(mntns_lifecycle_t *st);
int  a20_mntns_shutdown_active(const mntns_lifecycle_t *st);
int  a20_mntns_shutdown_count(const mntns_lifecycle_t *st);
int  a20_mntns_shutdown_begin(mntns_lifecycle_t *st, void *ctx, mntns_scan_fn scan);
int  a20_mntns_shutdown_abort_plan(mntns_lifecycle_t *st);
uint64_t a20_mntns_retry_pass(mntns_lifecycle_t *st);
int  a20_mntns_retry_select(const mntns_retry_cand_t *cands, size_t n,
                            uint64_t pass, int allow_busy_retry);

#endif /* _FS_VFS_MNTNS_LIFECYCLE_H */
