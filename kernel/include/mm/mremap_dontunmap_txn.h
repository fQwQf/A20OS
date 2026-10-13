#ifndef A20_MM_MREMAP_DONTUNMAP_TXN_H
#define A20_MM_MREMAP_DONTUNMAP_TXN_H

#include <stddef.h>
#include <stdint.h>

/* Operations supplied by the MM implementation around a single DONTUNMAP
 * destination.  The transaction owns their order and the destination's
 * rollback lifetime; callbacks are individual MM primitives, not a second
 * transaction hidden behind one C call. */
typedef struct mm_dontunmap_txn_ops {
    int (*prepare_destination)(void *ctx);
    int (*copy_backing)(void *ctx);
    int (*move_pages)(void *ctx);
    int (*reprotect)(void *ctx);
    int (*preflight_source)(void *ctx);
    void (*clear_source)(void *ctx);
    void (*release_source_lock)(void *ctx);
    int (*rollback_destination)(void *ctx, int exact_vma);
    void (*rollback_failed)(void *ctx, int primary_error,
                            int cleanup_error);
} mm_dontunmap_txn_ops_t;

/* C is the default and reference transaction.  Rust selection calls the same
 * primitive callbacks and receives the same allocated destination. */
int mm_mremap_dontunmap_txn_c(const mm_dontunmap_txn_ops_t *ops, void *ctx,
                              uintptr_t destination, size_t length,
                              uintptr_t *out_destination);
int mm_mremap_dontunmap_txn_rust(const mm_dontunmap_txn_ops_t *ops, void *ctx,
                                 uintptr_t destination, size_t length,
                                 uintptr_t *out_destination);

#endif
