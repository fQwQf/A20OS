#ifndef A20_EXT4_INODE_FREE_TXN_H
#define A20_EXT4_INODE_FREE_TXN_H

#include <stdint.h>

/* Shared production policy for the final step of freeing an inode. The
 * filesystem-specific callbacks below remain the narrow bitmap/descriptor
 * operations. A no-journal descriptor error must restore the allocation bit
 * while alloc_lock is still held; otherwise an orphan retry can relink an
 * inode number already handed to another allocator. */
typedef struct {
    void *context;
    void (*restore_free_inode_count)(void *, uint32_t);
    int (*mark_inode_allocated)(void *);
    int (*write_group_descriptor)(void *);
    void (*poison_journal)(void *);
} ext4_inode_free_rollback_ops_t;

/* ABI shared with ext4_extent_txn.rs. C owns the filesystem lock and raw
 * metadata primitives; Rust owns the bounded extent parse/free/rollback and
 * resulting inode-image decision. */
typedef struct {
    void *context;
    int (*free_block)(void *, uint64_t);
    int (*reserve_block)(void *, uint64_t);
    int (*write_inode)(void *, uint8_t *);
    int (*poison_journal)(void *);
    int32_t journaled;
    uint64_t first_data_block;
    uint64_t blocks_count;
} ext4_extent_txn_ops_t;

static inline int ext4_inode_free_rollback(
    const ext4_inode_free_rollback_ops_t *ops, uint32_t old_free_count,
    int write_error, int journaled)
{
    if (!ops || !ops->restore_free_inode_count ||
        !ops->mark_inode_allocated || !ops->write_group_descriptor)
        return -117; /* EUCLEAN: allocation state cannot be established */
    if (journaled) {
        if (ops->poison_journal) ops->poison_journal(ops->context);
        return write_error;
    }

    ops->restore_free_inode_count(ops->context, old_free_count);
    int bit_error = ops->mark_inode_allocated(ops->context);
    int descriptor_error = ops->write_group_descriptor(ops->context);
    if (bit_error < 0 || descriptor_error < 0)
        return -117; /* do not relink an inode with uncertain bitmap state */
    return write_error;
}

#endif
