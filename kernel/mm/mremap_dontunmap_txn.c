#include "mm/mremap_dontunmap_txn.h"
#include "core/errno.h"

static int txn_fail(const mm_dontunmap_txn_ops_t *ops, void *ctx,
                    int primary_error, int exact_vma)
{
    int cleanup_error = ops->rollback_destination(ctx, exact_vma);
    if (cleanup_error < 0) {
        ops->rollback_failed(ctx, primary_error, cleanup_error);
        return cleanup_error;
    }
    return primary_error;
}

int mm_mremap_dontunmap_txn_c(const mm_dontunmap_txn_ops_t *ops, void *ctx,
                              uintptr_t destination, size_t length,
                              uintptr_t *out_destination)
{
    if (!ops || !ctx || !length || !out_destination ||
        !ops->prepare_destination || !ops->copy_backing || !ops->move_pages ||
        !ops->reprotect || !ops->preflight_source || !ops->clear_source ||
        !ops->release_source_lock || !ops->rollback_destination ||
        !ops->rollback_failed)
        return -EINVAL;

    /* Until exact VMA boundaries are established, rollback may need the
     * ordinary partial-unmap path. Keep it explicit and fallible. */
    int r = ops->prepare_destination(ctx);
    if (r < 0)
        return txn_fail(ops, ctx, r, 0);

    r = ops->copy_backing(ctx);
    if (r < 0)
        return txn_fail(ops, ctx, r, 1);
    r = ops->move_pages(ctx);
    if (r < 0)
        return txn_fail(ops, ctx, r, 1);
    r = ops->reprotect(ctx);
    if (r < 0)
        return txn_fail(ops, ctx, r, 1);
    r = ops->preflight_source(ctx);
    if (r < 0)
        return txn_fail(ops, ctx, r, 1);

    /* Source retirement is the commit point. The caller reserved the TLB
     * holds before allocating the destination, so these operations cannot
     * report a recoverable error after preflight. */
    ops->clear_source(ctx);
    ops->release_source_lock(ctx);
    *out_destination = destination;
    return 0;
}
