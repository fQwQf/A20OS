#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../../kernel/fs/diskfs/ext4_inode_free_txn.h"

typedef struct {
    uint32_t free_inodes;
    int allocated;
    int fail_bit_restore;
    int fail_descriptor_restore;
    int poison_count;
    int descriptor_writes;
} fixture_t;

static void restore_count(void *opaque, uint32_t count)
{
    ((fixture_t *)opaque)->free_inodes = count;
}

static int mark_allocated(void *opaque)
{
    fixture_t *f = opaque;
    if (f->fail_bit_restore) return -5;
    f->allocated = 1;
    return 0;
}

static int write_descriptor(void *opaque)
{
    fixture_t *f = opaque;
    f->descriptor_writes++;
    return f->fail_descriptor_restore ? -5 : 0;
}

static void poison(void *opaque)
{
    ((fixture_t *)opaque)->poison_count++;
}

static ext4_inode_free_rollback_ops_t ops_for(fixture_t *f)
{
    return (ext4_inode_free_rollback_ops_t) {
        .context = f,
        .restore_free_inode_count = restore_count,
        .mark_inode_allocated = mark_allocated,
        .write_group_descriptor = write_descriptor,
        .poison_journal = poison,
    };
}

static void test_nojournal_descriptor_failure_is_rolled_back(void)
{
    fixture_t f = { .free_inodes = 5, .allocated = 0 };
    ext4_inode_free_rollback_ops_t ops = ops_for(&f);
    assert(ext4_inode_free_rollback(&ops, 4, -5, 0) == -5);
    assert(f.free_inodes == 4 && f.allocated == 1);
    assert(f.descriptor_writes == 1 && f.poison_count == 0);
}

static void test_uncertain_bitmap_or_descriptor_is_fatal(void)
{
    fixture_t bit = { .free_inodes = 5, .allocated = 0,
                      .fail_bit_restore = 1 };
    ext4_inode_free_rollback_ops_t bit_ops = ops_for(&bit);
    assert(ext4_inode_free_rollback(&bit_ops, 4, -5, 0) == -117);
    assert(bit.free_inodes == 4 && bit.allocated == 0);

    fixture_t desc = { .free_inodes = 5, .allocated = 0,
                       .fail_descriptor_restore = 1 };
    ext4_inode_free_rollback_ops_t desc_ops = ops_for(&desc);
    assert(ext4_inode_free_rollback(&desc_ops, 4, -5, 0) == -117);
    assert(desc.free_inodes == 4 && desc.allocated == 1);
}

static void test_journal_error_poisoned_without_nojournal_rollback(void)
{
    fixture_t f = { .free_inodes = 5, .allocated = 0 };
    ext4_inode_free_rollback_ops_t ops = ops_for(&f);
    assert(ext4_inode_free_rollback(&ops, 4, -5, 1) == -5);
    assert(f.free_inodes == 5 && f.allocated == 0);
    assert(f.descriptor_writes == 0 && f.poison_count == 1);
}

int main(void)
{
    test_nojournal_descriptor_failure_is_rolled_back();
    test_uncertain_bitmap_or_descriptor_is_fatal();
    test_journal_error_poisoned_without_nojournal_rollback();
    puts("ext4 inode-free transaction oracle: 3/3 PASS");
    return 0;
}
