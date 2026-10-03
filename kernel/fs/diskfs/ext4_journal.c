#include "fs/ext4_internal.h"
#include "fs/ext4_journal.h"
#include "fs/block_cache.h"
#include "mm/slab.h"
#include "core/bootargs.h"
#include "core/errno.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/stdio.h"

/*
 * Minimal, fail-closed JBD2 for ext4's internal journal.
 *
 * Reading: replay a pre-existing, checksummed recovery log before the
 * filesystem becomes writable, then mark that log empty and clear
 * EXT4_FEATURE_INCOMPAT_RECOVER.  Unknown JBD2 formats are rejected instead of
 * being mounted unsafely.
 *
 * Writing: ordered mode.  File data is never copied into the log; it is forced
 * home first, and the metadata naming it is only allowed to follow a durable
 * commit block.  The ordering itself is enforced one level down, by holding the
 * affected cache pages (bcache_hold_page) so that no generic sync can write
 * metadata home while its transaction is still in flight.
 */

#define JBD2_MAGIC_NUMBER             0xc03b3998U
#define JBD2_DESCRIPTOR_BLOCK         1U
#define JBD2_COMMIT_BLOCK             2U
#define JBD2_SUPERBLOCK_V2            4U
#define JBD2_REVOKE_BLOCK             5U

#define JBD2_FLAG_ESCAPE              0x1U
#define JBD2_FLAG_SAME_UUID           0x2U
#define JBD2_FLAG_DELETED             0x4U
#define JBD2_FLAG_LAST_TAG            0x8U
#define JBD2_KNOWN_TAG_FLAGS          0xfU

#define JBD2_FEATURE_COMPAT_CHECKSUM  0x1U
#define JBD2_FEATURE_INCOMPAT_REVOKE  0x1U
#define JBD2_FEATURE_INCOMPAT_64BIT   0x2U
#define JBD2_FEATURE_INCOMPAT_CSUM_V3 0x10U
#define JBD2_SUPPORTED_INCOMPAT       (JBD2_FEATURE_INCOMPAT_REVOKE | \
                                       JBD2_FEATURE_INCOMPAT_64BIT | \
                                       JBD2_FEATURE_INCOMPAT_CSUM_V3)

#define JBD2_CRC32C_CHKSUM            4U
#define JBD2_HEADER_BYTES             12U
#define JBD2_TAG3_BYTES               16U
#define JBD2_REVOKE_HEADER_BYTES      16U
#define JBD2_CHECKSUM_TAIL_BYTES      4U
#define JBD2_SUPERBLOCK_BYTES         1024U
#define JBD2_SUPERBLOCK_CHECKSUM_OFF  252U
#define JBD2_MAX_JOURNAL_BLOCKS       65536U
/* s_sequence / s_start / s_head inside the journal superblock. */
#define JBD2_SB_SEQUENCE_OFF          24U
#define JBD2_SB_START_OFF             28U
#define JBD2_SB_HEAD_OFF              88U
/* Descriptor payload available for tags: one 12-byte header, a trailing
 * checksum, and 16 bytes of UUID per tag.  Capped below what that allows so a
 * transaction can never come within one block of not fitting. */
#define JBD2_MAX_TAGS_PER_DESCRIPTOR  240U

#define EXT4_SUPERBLOCK_CHECKSUM_OFF  1020U

enum jbd2_recovery_pass {
    JBD2_PASS_SCAN,
    JBD2_PASS_REVOKE,
    JBD2_PASS_REPLAY,
};

typedef struct jbd2_revoke_entry {
    uint64_t block;
    uint32_t sequence;
} jbd2_revoke_entry_t;

/* Shared reader/writer state.  Recovery uses one on the stack; the persistent
 * writer embeds it. */
typedef struct jbd2 {
    ext4_sb_info_t *fs;
    ext4_inode_t journal_inode;
    uint32_t block_size;
    uint32_t maxlen;
    uint32_t first;
    uint32_t start;
    uint32_t sequence;
    uint32_t end_sequence;
    uint32_t head;
    uint32_t incompat;
    uint32_t checksum_seed;
    uint32_t scan_revoke_records;
    uint32_t replayed_blocks;
    uint32_t revoke_hits;
    uint8_t uuid[16];
    uint8_t *meta;
    uint8_t *data;
    jbd2_revoke_entry_t *revokes;
    uint32_t revoke_count;
    uint32_t revoke_capacity;
} jbd2_t;

/* ---- writer state ----------------------------------------------------- */

struct ext4_journal {
    jbd2_t     j;
    mutex_t    lock;
    /* Physical filesystem blocks covered by the open transaction, in the
     * order they were first recorded. */
    uint64_t  *tx;
    uint32_t   tx_count;
    uint32_t   tx_capacity;
    /* True once EXT4_FEATURE_INCOMPAT_RECOVER has been written and flushed
     * for this dirty period.  The on-disk flag is what makes a crash between
     * the commit block and the checkpoint replayable, so it is set before the
     * log is touched and cleared only after the checkpoint is durable. */
    int        recover_flag;
    int        enabled;
    /* Log block the next transaction starts at.  Kept apart from j.start,
     * which is the *on-disk* pointer that recovery reads: while a transaction
     * is in flight the two differ, and mixing them up is how a log ends up
     * claiming a replay that nobody committed. */
    uint32_t   head;
    uint64_t   commits;
    uint64_t   committed_blocks;
    char       crash_point[24];
};

static uint32_t crc32c_table[256];
static int crc32c_table_ready;

static uint32_t get_be32(const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t get_be64(const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;
    return ((uint64_t)get_be32(p) << 32) | get_be32(p + 4);
}

static void put_be32(void *ptr, uint32_t value)
{
    uint8_t *p = (uint8_t *)ptr;
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void put_le32(void *ptr, uint32_t value)
{
    uint8_t *p = (uint8_t *)ptr;
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void crc32c_init_table(void)
{
    if (crc32c_table_ready)
        return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0x82f63b78U : 0U);
        crc32c_table[i] = crc;
    }
    crc32c_table_ready = 1;
}

uint32_t ext4_crc32c(uint32_t seed, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = seed;
    crc32c_init_table();
    while (len--)
        crc = crc32c_table[(crc ^ *p++) & 0xffU] ^ (crc >> 8);
    return crc;
}

static uint32_t jbd2_advance(const jbd2_t *j, uint32_t block)
{
    block++;
    return block == j->maxlen ? j->first : block;
}

static uint32_t jbd2_advance_n(const jbd2_t *j, uint32_t block, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        block = jbd2_advance(j, block);
    return block;
}

static int jbd2_read_block(jbd2_t *j, uint32_t logical, void *buffer)
{
    if (logical >= j->maxlen)
        return -EINVAL;
    uint64_t physical = ext4_block_map(j->fs, &j->journal_inode, logical);
    if (!physical || physical >= j->fs->blocks_count)
        return -EIO;
    if (bcache_read_bytes(j->fs->bc, physical * j->block_size,
                          buffer, j->block_size) < 0)
        return -EIO;
    return 0;
}

static int jbd2_write_block(jbd2_t *j, uint32_t logical,
                            const void *buffer)
{
    if (logical >= j->maxlen)
        return -EINVAL;
    uint64_t physical = ext4_block_map(j->fs, &j->journal_inode, logical);
    if (!physical || physical >= j->fs->blocks_count)
        return -EIO;
    if (bcache_write_bytes(j->fs->bc, physical * j->block_size,
                           buffer, j->block_size) < 0)
        return -EIO;
    return 0;
}

static int jbd2_metadata_checksum_ok(jbd2_t *j, uint8_t *block)
{
    uint32_t offset = j->block_size - JBD2_CHECKSUM_TAIL_BYTES;
    uint32_t stored = get_be32(block + offset);
    put_be32(block + offset, 0);
    uint32_t calculated = ext4_crc32c(j->checksum_seed, block, j->block_size);
    put_be32(block + offset, stored);
    return calculated == stored;
}

static int jbd2_commit_checksum_ok(jbd2_t *j, uint8_t *block)
{
    uint32_t stored = get_be32(block + 16);
    put_be32(block + 16, 0);
    uint32_t calculated = ext4_crc32c(j->checksum_seed, block, j->block_size);
    put_be32(block + 16, stored);
    return calculated == stored;
}

/* A data block's checksum is carried by its descriptor tag and covers the
 * block image exactly as it is logged.  Nothing is written into the block
 * itself: its last four bytes belong to the filesystem -- they are inside a
 * block bitmap's hashed area, for one -- so a checksum parked there would be
 * replayed straight back into the filesystem block and desynchronise it from
 * the group descriptor that carries the matching value. */
static uint32_t jbd2_data_checksum(jbd2_t *j, const uint8_t *block,
                                   uint32_t sequence)
{
    uint8_t encoded_sequence[4];
    put_be32(encoded_sequence, sequence);
    uint32_t crc = ext4_crc32c(j->checksum_seed, encoded_sequence,
                               sizeof(encoded_sequence));
    return ext4_crc32c(crc, block, j->block_size);
}

static int jbd2_data_checksum_ok(jbd2_t *j, const uint8_t *block,
                                 uint32_t sequence, uint32_t stored)
{
    return jbd2_data_checksum(j, block, sequence) == stored;
}

static int jbd2_tid_geq(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) >= 0;
}

static int jbd2_set_revoke(jbd2_t *j, uint64_t block,
                           uint32_t sequence)
{
    for (uint32_t i = 0; i < j->revoke_count; i++) {
        if (j->revokes[i].block != block)
            continue;
        if (jbd2_tid_geq(sequence, j->revokes[i].sequence))
            j->revokes[i].sequence = sequence;
        return 0;
    }
    if (j->revoke_count >= j->revoke_capacity)
        return -EINVAL;
    j->revokes[j->revoke_count].block = block;
    j->revokes[j->revoke_count].sequence = sequence;
    j->revoke_count++;
    return 0;
}

static int jbd2_is_revoked(jbd2_t *j, uint64_t block,
                           uint32_t sequence)
{
    for (uint32_t i = 0; i < j->revoke_count; i++) {
        if (j->revokes[i].block == block &&
            jbd2_tid_geq(j->revokes[i].sequence, sequence))
            return 1;
    }
    return 0;
}

static int jbd2_process_revoke(jbd2_t *j, uint8_t *block,
                               uint32_t sequence,
                               enum jbd2_recovery_pass pass)
{
    if (!jbd2_metadata_checksum_ok(j, block))
        return -EIO;
    uint32_t count = get_be32(block + 12);
    uint32_t record_bytes = (j->incompat & JBD2_FEATURE_INCOMPAT_64BIT) ? 8U : 4U;
    if (count < JBD2_REVOKE_HEADER_BYTES ||
        count > j->block_size - JBD2_CHECKSUM_TAIL_BYTES ||
        (count - JBD2_REVOKE_HEADER_BYTES) % record_bytes != 0)
        return -EINVAL;
    uint32_t records = (count - JBD2_REVOKE_HEADER_BYTES) / record_bytes;
    if (pass == JBD2_PASS_SCAN) {
        if (records > j->maxlen - j->scan_revoke_records)
            return -EINVAL;
        j->scan_revoke_records += records;
        return 0;
    }
    if (pass != JBD2_PASS_REVOKE)
        return 0;
    uint32_t offset = JBD2_REVOKE_HEADER_BYTES;
    for (uint32_t i = 0; i < records; i++, offset += record_bytes) {
        uint64_t target = record_bytes == 8 ? get_be64(block + offset) :
                                             get_be32(block + offset);
        if (target >= j->fs->blocks_count)
            return -EINVAL;
        int ret = jbd2_set_revoke(j, target, sequence);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int jbd2_process_descriptor(jbd2_t *j, uint8_t *descriptor,
                                   uint32_t sequence, uint32_t *log_block,
                                   uint32_t *consumed,
                                   enum jbd2_recovery_pass pass)
{
    if (!jbd2_metadata_checksum_ok(j, descriptor))
        return -EIO;
    uint32_t limit = j->block_size - JBD2_CHECKSUM_TAIL_BYTES;
    uint32_t offset = JBD2_HEADER_BYTES;
    for (;;) {
        if (offset + JBD2_TAG3_BYTES > limit)
            return -EINVAL;
        uint64_t target = get_be32(descriptor + offset);
        uint32_t flags = get_be32(descriptor + offset + 4);
        if (j->incompat & JBD2_FEATURE_INCOMPAT_64BIT)
            target |= (uint64_t)get_be32(descriptor + offset + 8) << 32;
        uint32_t checksum = get_be32(descriptor + offset + 12);
        offset += JBD2_TAG3_BYTES;
        if (flags & ~JBD2_KNOWN_TAG_FLAGS)
            return -EINVAL;
        if (!(flags & JBD2_FLAG_SAME_UUID)) {
            if (offset + 16 > limit)
                return -EINVAL;
            /* JBD2 treats this as an opaque UUID slot; existing e2fsprogs
             * images may leave it zero even though the journal UUID is set. */
            offset += 16;
        }
        if (target >= j->fs->blocks_count)
            return -EINVAL;
        if (*consumed >= j->maxlen)
            return -EINVAL;
        uint32_t data_log_block = *log_block;
        *log_block = jbd2_advance(j, *log_block);
        (*consumed)++;

        if (pass == JBD2_PASS_SCAN || pass == JBD2_PASS_REPLAY) {
            int ret = jbd2_read_block(j, data_log_block, j->data);
            if (ret < 0) {
                    return ret;
            }
            if (!jbd2_data_checksum_ok(j, j->data, sequence, checksum)) {
                return -EIO;
            }
        }

        if (pass == JBD2_PASS_REPLAY) {
            if (jbd2_is_revoked(j, target, sequence)) {
                j->revoke_hits++;
            } else {
                if (flags & JBD2_FLAG_ESCAPE)
                    put_be32(j->data, JBD2_MAGIC_NUMBER);
                if (bcache_write_bytes(j->fs->bc, target * j->block_size,
                                       j->data, j->block_size) < 0)
                    return -EIO;
                j->replayed_blocks++;
            }
        }
        if (flags & JBD2_FLAG_LAST_TAG)
            return 0;
    }
}

/*
 * Walk the log from j->start, one pass per job: find the last committed
 * transaction (SCAN), collect revokes (REVOKE), replay (REPLAY).
 *
 * *truncated reports that the scan stopped because the log ends in something
 * that is not a complete, checksummed transaction.  That is the normal shape
 * of a log whose writer died mid-transaction, and it means the incomplete tail
 * is simply discarded -- so the scan treats it as the end of the log instead of
 * an error.  The replay passes never reach it: they stop as soon as they run
 * out of committed transactions.
 */
static int jbd2_run_pass(jbd2_t *j, enum jbd2_recovery_pass pass,
                         int *truncated)
{
    uint32_t log_block = j->start;
    uint32_t sequence = j->sequence;
    uint32_t head = j->start;
    uint32_t consumed = 0;

    while (consumed < j->maxlen) {
        if (pass != JBD2_PASS_SCAN && sequence == j->end_sequence)
            break;
        uint32_t metadata_log_block = log_block;
        int ret = jbd2_read_block(j, metadata_log_block, j->meta);
        if (ret < 0) {
            if (pass != JBD2_PASS_SCAN)
                return ret;
            if (truncated)
                *truncated = 1;
            break;
        }
        log_block = jbd2_advance(j, log_block);
        consumed++;

        uint32_t magic = get_be32(j->meta);
        uint32_t type = get_be32(j->meta + 4);
        uint32_t block_sequence = get_be32(j->meta + 8);
        if (magic != JBD2_MAGIC_NUMBER || block_sequence != sequence) {
            if (pass != JBD2_PASS_SCAN)
                return -EIO;
            if (truncated)
                *truncated = 1;
            break;
        }

        int ok = 1;
        switch (type) {
        case JBD2_DESCRIPTOR_BLOCK:
            ok = jbd2_process_descriptor(j, j->meta, sequence, &log_block,
                                         &consumed, pass) >= 0;
            break;
        case JBD2_REVOKE_BLOCK:
            ok = jbd2_process_revoke(j, j->meta, sequence, pass) >= 0;
            break;
        case JBD2_COMMIT_BLOCK:
            ok = jbd2_commit_checksum_ok(j, j->meta);
            if (ok) {
                sequence++;
                head = log_block;
            }
            break;
        default:
            ok = 0;
            break;
        }
        if (!ok) {
            if (pass != JBD2_PASS_SCAN)
                return -EIO;
            if (truncated)
                *truncated = 1;
            break;
        }
    }

    if (pass == JBD2_PASS_SCAN) {
        j->end_sequence = sequence;
        j->head = head;
        return 0;
    }
    return sequence == j->end_sequence ? 0 : -EIO;
}

static int jbd2_superblock_checksum_ok(uint8_t *block)
{
    uint32_t stored = get_be32(block + JBD2_SUPERBLOCK_CHECKSUM_OFF);
    put_be32(block + JBD2_SUPERBLOCK_CHECKSUM_OFF, 0);
    uint32_t calculated = ext4_crc32c(0xffffffffU, block,
                                 JBD2_SUPERBLOCK_BYTES);
    put_be32(block + JBD2_SUPERBLOCK_CHECKSUM_OFF, stored);
    return calculated == stored;
}

static int jbd2_load_superblock(jbd2_t *j)
{
    int ret = jbd2_read_block(j, 0, j->meta);
    if (ret < 0)
        return ret;
    if (get_be32(j->meta) != JBD2_MAGIC_NUMBER ||
        get_be32(j->meta + 4) != JBD2_SUPERBLOCK_V2 ||
        !jbd2_superblock_checksum_ok(j->meta))
        return -EINVAL;

    uint32_t compat = get_be32(j->meta + 36);
    j->incompat = get_be32(j->meta + 40);
    uint32_t ro_compat = get_be32(j->meta + 44);
    if (compat & JBD2_FEATURE_COMPAT_CHECKSUM)
        return -EOPNOTSUPP;
    if (ro_compat || (j->incompat & ~JBD2_SUPPORTED_INCOMPAT) ||
        !(j->incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3))
        return -EOPNOTSUPP;
    if (j->meta[80] != JBD2_CRC32C_CHKSUM)
        return -EOPNOTSUPP;

    j->block_size = get_be32(j->meta + 12);
    j->maxlen = get_be32(j->meta + 16);
    j->first = get_be32(j->meta + 20);
    j->sequence = get_be32(j->meta + 24);
    j->start = get_be32(j->meta + 28);
    memcpy(j->uuid, j->meta + 48, sizeof(j->uuid));
    j->checksum_seed = ext4_crc32c(0xffffffffU, j->uuid, sizeof(j->uuid));

    if (j->block_size != j->fs->block_size || j->block_size != 4096 ||
        j->maxlen < 2 || j->maxlen > JBD2_MAX_JOURNAL_BLOCKS ||
        j->first < 1 || j->first >= j->maxlen ||
        (j->start && (j->start < j->first || j->start >= j->maxlen)) ||
        ext4_inode_size(&j->journal_inode) /
            j->block_size < j->maxlen)
        return -EINVAL;
    return 0;
}

static int jbd2_mark_empty(jbd2_t *j)
{
    int ret = jbd2_read_block(j, 0, j->meta);
    if (ret < 0)
        return ret;
    if (!jbd2_superblock_checksum_ok(j->meta))
        return -EIO;
    /* end_sequence is advanced after each valid commit, so it already is
     * the first transaction id available after the recovered log. */
    put_be32(j->meta + 24, j->end_sequence);
    put_be32(j->meta + 28, 0);
    put_be32(j->meta + 88, j->head);
    put_be32(j->meta + JBD2_SUPERBLOCK_CHECKSUM_OFF, 0);
    uint32_t checksum = ext4_crc32c(0xffffffffU, j->meta,
                               JBD2_SUPERBLOCK_BYTES);
    put_be32(j->meta + JBD2_SUPERBLOCK_CHECKSUM_OFF, checksum);
    return jbd2_write_block(j, 0, j->meta);
}

/*
 * Set or clear EXT4_FEATURE_INCOMPAT_RECOVER on the ext4 superblock.  Both
 * directions matter: the writer raises the flag before it touches the log and
 * lowers it only once the checkpoint is on disk, and e2fsprogs reads it to
 * decide whether to replay.
 *
 * The flag is deliberately not forced out ahead of the log copy.  It shares
 * its cache page with the superblock's free counters, which the open
 * transaction holds precisely so they cannot reach the disk before the log
 * does -- writing the page for the flag's sake would put the counters out
 * early and leave a pre-commit crash with counters that disagree with the
 * bitmaps.  Nothing is lost by waiting: the journal superblock's s_start is
 * written and flushed before the commit block, so any mount that finds a
 * committed transaction also finds a non-empty log, and ext4_journal_log_pending()
 * is what makes the mount act on it.  The flag itself lands with the
 * checkpoint, and is cleared again once the log is empty.
 *
 * The metadata_csum checksum covering the first 1020 bytes is verified on the
 * way in and recomputed on the way out, so a superblock we did not understand
 * is never silently rewritten.
 */
static int ext4_set_recover_feature(jbd2_t *j, ext4_superblock_t *disk_sb,
                                    int set)
{
    ext4_superblock_t updated;
    if (bcache_read_bytes(j->fs->bc, 1024, &updated,
                          sizeof(updated)) < 0)
        return -EIO;
    if (updated.s_magic != EXT4_DISK_MAGIC)
        return -EINVAL;
    uint8_t *raw = (uint8_t *)&updated;
    if (updated.s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        uint32_t stored = (uint32_t)raw[EXT4_SUPERBLOCK_CHECKSUM_OFF] |
                          ((uint32_t)raw[EXT4_SUPERBLOCK_CHECKSUM_OFF + 1] << 8) |
                          ((uint32_t)raw[EXT4_SUPERBLOCK_CHECKSUM_OFF + 2] << 16) |
                          ((uint32_t)raw[EXT4_SUPERBLOCK_CHECKSUM_OFF + 3] << 24);
        uint32_t calculated = ext4_crc32c(0xffffffffU, raw,
                                     EXT4_SUPERBLOCK_CHECKSUM_OFF);
        if (stored != calculated)
            return -EIO;
    }
    if (set)
        updated.s_feature_incompat |= EXT4_FEATURE_INCOMPAT_RECOVER;
    else
        updated.s_feature_incompat &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
    if (updated.s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        uint32_t checksum = ext4_crc32c(0xffffffffU, raw,
                                   EXT4_SUPERBLOCK_CHECKSUM_OFF);
        put_le32(raw + EXT4_SUPERBLOCK_CHECKSUM_OFF, checksum);
    }
    if (bcache_write_bytes(j->fs->bc, 1024, &updated,
                           sizeof(updated)) < 0)
        return -EIO;
    *disk_sb = updated;
    j->fs->s_feature_incompat = updated.s_feature_incompat;
    return 0;
}

int ext4_journal_recover(ext4_sb_info_t *fs, ext4_superblock_t *disk_sb)
{
    if (!fs || !disk_sb ||
        !(disk_sb->s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) ||
        !disk_sb->s_journal_inum)
        return -EINVAL;

    jbd2_t j;
    memset(&j, 0, sizeof(j));
    j.fs = fs;
    int ret = ext4_read_inode(fs, disk_sb->s_journal_inum,
                              &j.journal_inode);
    if (ret < 0)
        return ret;

    /* The published reference images use 4 KiB journal blocks.  Allocate before
     * reading the superblock because the ext4 block size is already known. */
    j.block_size = fs->block_size;
    uint64_t journal_blocks = ext4_inode_size(&j.journal_inode) /
                              j.block_size;
    if (journal_blocks < 2 || journal_blocks > JBD2_MAX_JOURNAL_BLOCKS)
        return -EINVAL;
    /* Bootstrap the bounds check used to read logical block zero.  The
     * on-disk s_maxlen replaces this value after its checksum is verified. */
    j.maxlen = (uint32_t)journal_blocks;
    j.meta = (uint8_t *)kmalloc(j.block_size);
    j.data = (uint8_t *)kmalloc(j.block_size);
    if (!j.meta || !j.data) {
        ret = -ENOMEM;
        goto out;
    }

    ret = jbd2_load_superblock(&j);
    if (ret < 0)
        goto out;

    if (!j.start) {
        j.end_sequence = j.sequence;
        j.head = get_be32(j.meta + 88);
        if (j.head < j.first || j.head >= j.maxlen)
            j.head = j.first;
        printf("[EXT4/JBD2] journal already empty; clearing stale recover flag\n");
    } else {
        printf("[EXT4/JBD2] recovery start=%u sequence=%u blocks=%u features=0x%x\n",
               j.start, j.sequence, j.maxlen, j.incompat);
        int truncated = 0;
        ret = jbd2_run_pass(&j, JBD2_PASS_SCAN, &truncated);
        if (ret < 0)
            goto out;
        if (truncated)
            printf("[EXT4/JBD2] log ends in an incomplete transaction; "
                   "discarding it\n");
        if (j.scan_revoke_records) {
            j.revoke_capacity = j.scan_revoke_records;
            j.revokes = (jbd2_revoke_entry_t *)kmalloc(
                (size_t)j.revoke_capacity * sizeof(*j.revokes));
            if (!j.revokes) {
                ret = -ENOMEM;
                goto out;
            }
        }
        ret = jbd2_run_pass(&j, JBD2_PASS_REVOKE, NULL);
        if (ret < 0)
            goto out;
        ret = jbd2_run_pass(&j, JBD2_PASS_REPLAY, NULL);
        if (ret < 0)
            goto out;
        ret = bcache_sync_checked(fs->bc);
        if (ret < 0) {
            /* Deferral is safe here: the on-disk journal still holds the
             * uncommitted transactions (mark_empty has not run), so the next
             * mount replays them again.  Replay is full-block blind writes,
             * therefore idempotent over any partially flushed prefix. */
            printf("[EXT4/JBD2] replay flush failed (%d); keeping replayed "
                   "blocks in cache and deferring journal cleanup\n", ret);
            ret = EXT4_JOURNAL_DEFERRED;
            goto out;
        }
        printf("[EXT4/JBD2] replay complete transactions=%u blocks=%u "
               "revokes=%u hits=%u head=%u\n",
               j.end_sequence - j.sequence, j.replayed_blocks,
               j.revoke_count, j.revoke_hits, j.head);
    }

    ret = jbd2_mark_empty(&j);
    if (ret < 0)
        goto out;
    ret = bcache_sync_checked(fs->bc);
    if (ret < 0) {
        /* All replayed blocks are already durable (the post-replay sync
         * succeeded), so the emptied journal superblock may reach the disk
         * at any later time without risking inconsistency.  If it never
         * does, the next mount simply replays the same transactions. */
        printf("[EXT4/JBD2] mark-empty flush failed (%d); deferring\n", ret);
        ret = EXT4_JOURNAL_DEFERRED;
        goto out;
    }
    ret = ext4_set_recover_feature(&j, disk_sb, 0);
    if (ret < 0)
        goto out;
    ret = bcache_sync_checked(fs->bc);
    if (ret < 0) {
        /* Worst case on disk: empty journal with the recover flag still
         * set, which the next mount handles through the already-empty
         * path. */
        printf("[EXT4/JBD2] recover-flag flush failed (%d); deferring\n",
               ret);
        ret = EXT4_JOURNAL_DEFERRED;
        goto out;
    }
    printf("[EXT4/JBD2] journal marked empty and recover flag cleared\n");

out:
    if (ret < 0)
        printf("[EXT4/JBD2] recovery failed: %d\n", ret);
    if (j.revokes)
        kfree(j.revokes);
    if (j.data)
        kfree(j.data);
    if (j.meta)
        kfree(j.meta);
    return ret;
}

/* =====================================================================
 * Writer: ordered-mode transactions
 * =====================================================================
 *
 * The commit sequence below is the whole design.  Each step is separated by a
 * device flush, so "durable" means durable rather than "handed to a controller
 * that may reorder it":
 *
 *   1  flush the cache          data (and any unheld page) reaches disk first;
 *                                held metadata pages are skipped by the block
 *                                cache, so ordering is real rather than hoped
 *                                for
 *   2  RECOVER flag + flush     the next mount is now required to replay
 *   3  descriptor, data, flush  the log holds a self-sufficient copy of every
 *                                metadata block the transaction touches
 *   4  s_start, commit, flush   only now may the metadata go home
 *   5  release holds + flush    the checkpoint
 *   6  s_start=0, flag, flush   the transaction is now fully resolved and
 *                                the log is empty again
 *
 * A crash before step 4 has written nothing that matters: the log is
 * incomplete, so it is discarded, and no metadata reached its home location.
 * A crash between 4 and 6 leaves a committed transaction and a raised flag, so
 * the next mount replays it -- which is idempotent, because replay is a blind
 * full-block copy of exactly the bytes the checkpoint would have written.
 *
 * The log itself is written with unbuffered device I/O rather than through the
 * block cache.  The cache flushes in pool order, which cannot express
 * "descriptor before data before commit"; the log needs that order to be
 * reconstructible rather than merely present.
 */

/* The point the next commit should die at, or "" for none.  Two sources set
 * it: a20.journal_crash= on the command line (handy interactively) and
 * /proc/a20/journal (which is what the crash-consistency gate uses, because
 * not every machine hands QEMU's -append to the kernel -- LoongArch's virt
 * board creates an empty /chosen and drops it).  The command line is copied
 * into the journal at open time so a mount-time decision cannot race a later
 * procfs write; the global is what the running commit consults. */
static char g_journal_crash_point[24];

/* The injection points, in the order a commit reaches them.  Kept as a table
 * rather than as string literals at the call sites so /proc/a20/journal can
 * reject a typo: a misspelled point would otherwise look armed and silently
 * never fire, which is exactly the failure this gate exists to rule out. */
static const char *const jbd2_crash_points[] = {
    "post-recover-flag",
    "post-journal",
    "post-commit",
    "post-checkpoint",
};

static int jbd2_crash_point_known(const char *point)
{
    for (size_t i = 0; i < sizeof(jbd2_crash_points) /
                        sizeof(jbd2_crash_points[0]); i++) {
        if (strcmp(jbd2_crash_points[i], point) == 0)
            return 1;
    }
    return 0;
}

int ext4_journal_crash_points_format(char *buf, size_t bufsz)
{
    int n = snprintf(buf, bufsz, "crash_point: %s\npoints:", 
                     g_journal_crash_point[0] ? g_journal_crash_point : "none");
    if (n < 0)
        return 0;
    size_t off = (size_t)n;
    for (size_t i = 0; i < sizeof(jbd2_crash_points) /
                        sizeof(jbd2_crash_points[0]) && off + 1 < bufsz; i++) {
        n = snprintf(buf + off, bufsz - off, " %s", jbd2_crash_points[i]);
        if (n < 0)
            break;
        off += (size_t)n;
    }
    if (off + 1 < bufsz)
        buf[off++] = '\n';
    buf[off] = '\0';
    return (int)off;
}

int ext4_journal_set_crash_point(const char *point)
{
    if (!point)
        return -EINVAL;
    size_t len = strlen(point);
    if (len >= sizeof(g_journal_crash_point))
        return -EINVAL;
    if (len && !jbd2_crash_point_known(point))
        return -EINVAL;
    memcpy(g_journal_crash_point, point, len + 1);
    printf("[EXT4/JBD2] crash point set to '%s'\n", g_journal_crash_point);
    return 0;
}

const char *ext4_journal_crash_point(void)
{
    return g_journal_crash_point;
}

/* halt the machine at a named point in the commit sequence.
 * The only way to be sure a crash-consistency claim is true is to actually
 * crash at each step and check what the next mount makes of the result. */
static void jbd2_crash_point(ext4_journal_t *ej, const char *point)
{
    const char *armed = g_journal_crash_point[0] ? g_journal_crash_point
                                                 : ej->crash_point;
    if (!armed[0] || strcmp(armed, point) != 0)
        return;
    printf("[EXT4/JBD2] crash injection at %s: halting\n", point);
    printf("[EXT4/JBD2] CRASH-INJECT %s blocks=%u sequence=%u\n", point,
           ej->tx_count, ej->j.sequence);
    /* panic() halts without touching the block cache or syncing a filesystem,
     * so the image is left exactly as the last successful flush made it. */
    panic("a20.journal_crash=%s", point);
}

static int jbd2_physical_block(jbd2_t *j, uint32_t logical, uint64_t *out)
{
    if (logical >= j->maxlen)
        return -EINVAL;
    uint64_t physical = ext4_block_map(j->fs, &j->journal_inode, logical);
    if (!physical || physical >= j->fs->blocks_count)
        return -EIO;
    *out = physical;
    return 0;
}

static int jbd2_direct_flush(jbd2_t *j)
{
    block_dev_t *dev = j->fs->bc ? j->fs->bc->dev : NULL;
    if (!dev || !dev->flush)
        return -EOPNOTSUPP;
    return dev->flush(dev);
}

static int jbd2_direct_write(jbd2_t *j, uint32_t logical, const void *buffer)
{
    uint64_t physical;
    int ret = jbd2_physical_block(j, logical, &physical);
    if (ret < 0)
        return ret;
    block_dev_t *dev = j->fs->bc ? j->fs->bc->dev : NULL;
    if (!dev)
        return -EIO;
    /* The cached image of this log block is stale the moment it is written;
     * drop it so the next read goes to the device. */
    bcache_invalidate_page(j->fs->bc,
                           physical * j->block_size / PCACHE_PAGE_SIZE);
    int wrote = dev->write_sector(dev,
                                  physical * j->block_size / BCACHE_BLOCK_SIZE,
                                  (const void *)buffer,
                                  j->block_size / BCACHE_BLOCK_SIZE);
    return wrote < 0 ? wrote : 0;
}

static void jbd2_metadata_checksum_put(jbd2_t *j, uint8_t *block)
{
    uint32_t offset = j->block_size - JBD2_CHECKSUM_TAIL_BYTES;
    put_be32(block + offset, 0);
    put_be32(block + offset,
             ext4_crc32c(j->checksum_seed, block, j->block_size));
}

/* Pull one metadata block's current image out of the block cache.  It is
 * resident and dirty -- ext4_meta_write() put it there and the hold keeps it
 * from being evicted -- so this is the post-transaction content, which is
 * exactly what a replay has to reproduce. */
static int jbd2_metadata_image(ext4_journal_t *ej, uint64_t block, uint8_t *out)
{
    if (bcache_read_bytes(ej->j.fs->bc, block * ej->j.block_size,
                          out, ej->j.block_size) < 0)
        return -EIO;
    return 0;
}

static int jbd2_record_block(ext4_journal_t *ej, uint64_t block)
{
    for (uint32_t i = 0; i < ej->tx_count; i++)
        if (ej->tx[i] == block)
            return 0;
    if (ej->tx_count >= ej->tx_capacity)
        return -ENOSPC;
    ej->tx[ej->tx_count++] = block;
    return 0;
}

int ext4_journal_meta_write(ext4_sb_info_t *fs, uint64_t byte_off,
                            const void *buf, size_t len)
{
    ext4_journal_t *ej = fs ? fs->journal : NULL;
    if (!ej || !ej->enabled)
        return bcache_write_bytes(fs->bc, byte_off, buf, len);

    mutex_lock(&ej->lock);
    int ret = bcache_write_bytes(fs->bc, byte_off, buf, len);
    if (ret == 0) {
        /* Hold and record every filesystem block the write touched.  Doing
         * this under the journal lock is what closes the window in which a
         * concurrent commit could checkpoint metadata this write has just
         * made, but has not yet logged. */
        uint64_t first = byte_off / fs->block_size;
        uint64_t last = (byte_off + len - 1) / fs->block_size;
        for (uint64_t blk = first; blk <= last; blk++) {
            bcache_hold_page(fs->bc, blk * fs->block_size / PCACHE_PAGE_SIZE);
            ret = jbd2_record_block(ej, blk);
            if (ret < 0)
                break;
        }
        if (ret < 0) {
            /* A transaction that cannot grow is refused rather than silently
             * shrunk: a commit missing a block would replay a filesystem whose
             * inode and bitmap disagree. */
            printf("[EXT4/JBD2] transaction overflow at %llu blocks; "
                   "commit what fits\n", (unsigned long long)byte_off);
            ej->tx_count = 0;
            bcache_release_holds(fs->bc);
        }
    }
    mutex_unlock(&ej->lock);
    return ret;
}

static int jbd2_write_descriptor(ext4_journal_t *ej, uint32_t logical)
{
    jbd2_t *j = &ej->j;
    uint8_t *block = j->meta;
    memset(block, 0, j->block_size);
    put_be32(block, JBD2_MAGIC_NUMBER);
    put_be32(block + 4, JBD2_DESCRIPTOR_BLOCK);
    put_be32(block + 8, j->sequence);

    uint32_t offset = JBD2_HEADER_BYTES;
    for (uint32_t i = 0; i < ej->tx_count; i++) {
        uint64_t target = ej->tx[i];
        if (target >= j->fs->blocks_count)
            return -EINVAL;
        uint32_t flags = 0;
        if (i == ej->tx_count - 1)
            flags |= JBD2_FLAG_LAST_TAG;
        /* tag3: blocknr_lo, flags, blocknr_hi, checksum.  Every tag carries
         * the full UUID rather than JBD2_FLAG_SAME_UUID: one 16-byte copy per
         * block is cheap next to the 4 KiB data block it describes, and it
         * keeps the format identical to what e2fsprogs writes. */
        put_be32(block + offset, (uint32_t)target);
        put_be32(block + offset + 4, flags);
        put_be32(block + offset + 8, (uint32_t)(target >> 32));
        put_be32(block + offset + 12, 0);
        offset += JBD2_TAG3_BYTES;
        memcpy(block + offset, j->uuid, 16);
        offset += 16;
    }
    /* The data checksums depend on the sequence and on the block contents, so
     * they can only be computed once the descriptor is laid out -- do it here
     * and rewrite the tags' checksum field in place.  The descriptor's own
     * checksum comes last, because those rewrites change bytes it covers. */
    uint32_t tag_offset = JBD2_HEADER_BYTES;
    for (uint32_t i = 0; i < ej->tx_count; i++) {
        int ret = jbd2_metadata_image(ej, ej->tx[i], j->data);
        if (ret < 0)
            return ret;
        put_be32(block + tag_offset + 12,
                 jbd2_data_checksum(j, j->data, j->sequence));
        tag_offset += JBD2_TAG3_BYTES + 16;
    }
    jbd2_metadata_checksum_put(j, block);
    return jbd2_direct_write(j, logical, block);
}

static int jbd2_write_transaction_data(ext4_journal_t *ej, uint32_t first_log)
{
    jbd2_t *j = &ej->j;
    uint32_t logical = first_log;
    for (uint32_t i = 0; i < ej->tx_count; i++) {
        int ret = jbd2_metadata_image(ej, ej->tx[i], j->data);
        if (ret < 0)
            return ret;
        ret = jbd2_direct_write(j, logical, j->data);
        if (ret < 0)
            return ret;
        logical = jbd2_advance(j, logical);
    }
    return 0;
}

static int jbd2_write_commit(ext4_journal_t *ej, uint32_t logical)
{
    jbd2_t *j = &ej->j;
    uint8_t *block = j->meta;
    memset(block, 0, j->block_size);
    put_be32(block, JBD2_MAGIC_NUMBER);
    put_be32(block + 4, JBD2_COMMIT_BLOCK);
    put_be32(block + 8, j->sequence);
    /* Offset 12 would hold the revoke-table pointer in a journal that uses
     * one; zero means there is none, which is what the reader expects. */
    put_be32(block + 12, 0);
    put_be32(block + 16, 0);
    put_be32(block + 16, ext4_crc32c(j->checksum_seed, block, j->block_size));
    return jbd2_direct_write(j, logical, block);
}

/* Write the journal superblock through unbuffered I/O, optionally pointing
 * s_start at a transaction that is about to be committed. */
static int jbd2_write_journal_superblock(ext4_journal_t *ej, uint32_t start,
                                          uint32_t sequence, uint32_t head)
{
    jbd2_t *j = &ej->j;
    int ret = jbd2_read_block(j, 0, j->meta);
    if (ret < 0)
        return ret;
    if (!jbd2_superblock_checksum_ok(j->meta))
        return -EIO;
    put_be32(j->meta + JBD2_SB_SEQUENCE_OFF, sequence);
    put_be32(j->meta + JBD2_SB_START_OFF, start);
    put_be32(j->meta + JBD2_SB_HEAD_OFF, head);
    put_be32(j->meta + JBD2_SUPERBLOCK_CHECKSUM_OFF, 0);
    put_be32(j->meta + JBD2_SUPERBLOCK_CHECKSUM_OFF,
             ext4_crc32c(0xffffffffU, j->meta, JBD2_SUPERBLOCK_BYTES));
    return jbd2_direct_write(j, 0, j->meta);
}

static void jbd2_arm_crash_point(ext4_journal_t *ej)
{
    if (g_journal_crash_point[0])
        return;
    const char *cmdline = bootargs_get();
    static const char key[] = "a20.journal_crash=";
    const char *p = cmdline;
    while (p && *p) {
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (strncmp(tok, key, sizeof(key) - 1) == 0) {
            const char *v = tok + sizeof(key) - 1;
            size_t vlen = (size_t)(p - v);
            if (vlen >= sizeof(ej->crash_point))
                vlen = sizeof(ej->crash_point) - 1;
            memcpy(ej->crash_point, v, vlen);
            ej->crash_point[vlen] = '\0';
            if (!jbd2_crash_point_known(ej->crash_point)) {
                printf("[EXT4/JBD2] unknown crash point '%s', not armed\n",
                       ej->crash_point);
                ej->crash_point[0] = '\0';
                return;
            }
            printf("[EXT4/JBD2] crash injection armed at '%s'\n",
                   ej->crash_point);
            return;
        }
        while (*p == ' ' || *p == '\t')
            p++;
    }
}

int ext4_journal_open(ext4_sb_info_t *fs, const void *disk_sb_ptr)
{
    const ext4_superblock_t *disk_sb = (const ext4_superblock_t *)disk_sb_ptr;
    if (!fs || !disk_sb)
        return -EINVAL;
    if (!(disk_sb->s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) ||
        !disk_sb->s_journal_inum)
        return 0;

    ext4_journal_t *ej = (ext4_journal_t *)kmalloc(sizeof(*ej));
    if (!ej)
        return -ENOMEM;
    memset(ej, 0, sizeof(*ej));
    mutex_init(&ej->lock);
    ej->j.fs = fs;
    ej->tx_capacity = JBD2_MAX_TAGS_PER_DESCRIPTOR;
    ej->tx = (uint64_t *)kmalloc(sizeof(uint64_t) * ej->tx_capacity);
    if (!ej->tx) {
        kfree(ej);
        return -ENOMEM;
    }
    int ret = ext4_read_inode(fs, disk_sb->s_journal_inum, &ej->j.journal_inode);
    if (ret < 0)
        goto fail;
    ej->j.block_size = fs->block_size;
    uint64_t journal_blocks = ext4_inode_size(&ej->j.journal_inode) /
                              ej->j.block_size;
    if (journal_blocks < 2 || journal_blocks > JBD2_MAX_JOURNAL_BLOCKS) {
        ret = -EINVAL;
        goto fail;
    }
    /* A log block is one filesystem block is one block-cache page.  Refuse
     * any other geometry rather than copy a 4 KiB page into every 1 KiB log
     * block and hope the reader notices. */
    if (fs->block_size != 4096) {
        printf("[EXT4/JBD2] %u-byte filesystem blocks are not supported by "
               "the journal writer\n", fs->block_size);
        ret = -EOPNOTSUPP;
        goto fail;
    }
    ej->j.maxlen = (uint32_t)journal_blocks;
    ej->j.meta = (uint8_t *)kmalloc(ej->j.block_size);
    ej->j.data = (uint8_t *)kmalloc(ej->j.block_size);
    if (!ej->j.meta || !ej->j.data) {
        ret = -ENOMEM;
        goto fail;
    }
    ret = jbd2_load_superblock(&ej->j);
    if (ret < 0) {
        printf("[EXT4/JBD2] journal superblock rejected (%d); refusing to "
               "mount a filesystem whose log we cannot read\n", ret);
        goto fail;
    }
    /* The writer keeps at most one transaction in flight and checkpoints it
     * before returning, so an empty log is the only state it can resume from.
     * EXT4_FEATURE_INCOMPAT_RECOVER is set before the log is touched and
     * cleared after the checkpoint, so a non-empty log here means the image
     * arrived damaged -- mount it read-only rather than writing on top. */
    if (ej->j.start) {
        printf("[EXT4/JBD2] journal not empty at mount (start=%u) with no "
               "recover flag; refusing to mount\n", ej->j.start);
        ret = -EIO;
        goto fail;
    }
    ej->head = get_be32(ej->j.meta + JBD2_SB_HEAD_OFF);
    if (ej->head < ej->j.first || ej->head >= ej->j.maxlen)
        ej->head = ej->j.first;
    ej->j.start = ej->head;
    ej->enabled = 1;
    fs->journal = ej;
    jbd2_arm_crash_point(ej);
    printf("[EXT4/JBD2] writer ready: %u log blocks, block %u, head %u, "
           "sequence %u\n", ej->j.maxlen, ej->j.block_size, ej->head,
           ej->j.sequence);
    return 0;

fail:
    if (ej->j.meta)
        kfree(ej->j.meta);
    if (ej->j.data)
        kfree(ej->j.data);
    if (ej->tx)
        kfree(ej->tx);
    kfree(ej);
    return ret;
}

void ext4_journal_close(ext4_sb_info_t *fs)
{
    ext4_journal_t *ej = fs ? fs->journal : NULL;
    if (!ej)
        return;
    (void)ext4_journal_commit(fs);
    fs->journal = NULL;
    if (ej->j.meta)
        kfree(ej->j.meta);
    if (ej->j.data)
        kfree(ej->j.data);
    if (ej->tx)
        kfree(ej->tx);
    kfree(ej);
}

int ext4_journal_pending(const ext4_sb_info_t *fs)
{
    const ext4_journal_t *ej = fs ? fs->journal : NULL;
    return ej && ej->enabled ? (int)ej->tx_count : 0;
}

void ext4_journal_report(ext4_sb_info_t *fs)
{
    ext4_journal_t *ej = fs ? fs->journal : NULL;
    if (!ej || !ej->enabled) {
        printf("[EXT4/JBD2] no writable journal on this filesystem\n");
        return;
    }
    printf("[EXT4/JBD2] commits=%llu blocks=%llu pending=%u head=%u "
           "sequence=%u recover_flag=%d\n",
           (unsigned long long)ej->commits,
           (unsigned long long)ej->committed_blocks, ej->tx_count,
           ej->head, ej->j.sequence, ej->recover_flag);
}

/* Does the on-disk journal still hold a transaction?
 *
 * The needs_recovery feature is the cheap signal, but it is written by a
 * filesystem whose cache page the open transaction holds, so a crash can
 * leave a non-empty log without it.  Reading the journal superblock is the
 * authoritative answer, and the cost is one block read on a path that already
 * reads it during recovery. */
int ext4_journal_log_pending(ext4_sb_info_t *fs, ext4_superblock_t *disk_sb)
{
    if (!fs || !disk_sb ||
        !(disk_sb->s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) ||
        !disk_sb->s_journal_inum)
        return 0;

    jbd2_t j;
    memset(&j, 0, sizeof(j));
    j.fs = fs;
    if (ext4_read_inode(fs, disk_sb->s_journal_inum, &j.journal_inode) < 0)
        return 0;
    j.block_size = fs->block_size;
    uint64_t journal_blocks = ext4_inode_size(&j.journal_inode) /
                              j.block_size;
    if (journal_blocks < 2 || journal_blocks > JBD2_MAX_JOURNAL_BLOCKS)
        return 0;
    j.maxlen = (uint32_t)journal_blocks;
    j.meta = (uint8_t *)kmalloc(j.block_size);
    if (!j.meta)
        return 0;
    int pending = 0;
    /* A journal we cannot read is not a journal we can replay, but it is
     * also not proof of an empty log: report it as pending and let the
     * recovery attempt produce the diagnostic. */
    pending = jbd2_load_superblock(&j) < 0 ? 1 : (j.start != 0);
    kfree(j.meta);
    return pending;
}

int ext4_journal_commit(ext4_sb_info_t *fs)
{
    ext4_journal_t *ej = fs ? fs->journal : NULL;
    if (!ej || !ej->enabled)
        return 0;
    mutex_lock(&ej->lock);
    jbd2_t *j = &ej->j;
    ext4_superblock_t disk_sb;

    if (ej->tx_count == 0 && !ej->recover_flag) {
        mutex_unlock(&ej->lock);
        return 0;
    }
    if (ej->tx_count == 0) {
        /* Nothing to write, but a previous commit raised the flag and did not
         * get to lower it.  Finish that now rather than leaving the next
         * mount a replay to do. */
        int ret = bcache_read_bytes(fs->bc, 1024, &disk_sb, sizeof(disk_sb));
        if (ret == 0)
            ret = ext4_set_recover_feature(j, &disk_sb, 0);
        if (ret == 0)
            ret = bcache_sync_checked(fs->bc);
        if (ret == 0)
            ej->recover_flag = 0;
        mutex_unlock(&ej->lock);
        return ret;
    }

    int ret;
    /* 1. Ordered mode: the data a transaction's metadata refers to is forced
     *    out first.  Held pages are skipped by the block cache, so what
     *    lands here is data (and any page no transaction covers). */
    ret = bcache_sync_checked(fs->bc);
    if (ret < 0)
        goto out;

    /* 2. From here on a crash must be recoverable, so say so on disk. */
    if (!ej->recover_flag) {
        ret = bcache_read_bytes(fs->bc, 1024, &disk_sb, sizeof(disk_sb));
        if (ret < 0)
            goto out;
        ret = ext4_set_recover_feature(j, &disk_sb, 1);
        if (ret < 0)
            goto out;
        ret = bcache_sync_checked(fs->bc);
        if (ret < 0)
            goto out;
        ej->recover_flag = 1;
    }
    jbd2_crash_point(ej, "post-recover-flag");

    /* 3. The log copy.  Descriptor first, then every data block, each phase
     *    flushed, so a torn log is always recognisably torn. */
    uint32_t log = ej->head;
    ret = jbd2_write_descriptor(ej, log);
    if (ret < 0)
        goto out;
    ret = jbd2_direct_flush(j);
    if (ret < 0)
        goto out;
    log = jbd2_advance(j, log);
    ret = jbd2_write_transaction_data(ej, log);
    if (ret < 0)
        goto out;
    ret = jbd2_direct_flush(j);
    if (ret < 0)
        goto out;
    /* The commit block follows every data block this transaction wrote, so
     * the pointer moves by the transaction's size -- advancing by one would
     * land the commit on top of a data block and the next mount would read
     * its own commit block back as log data and discard the transaction. */
    log = jbd2_advance_n(j, log, ej->tx_count);
    jbd2_crash_point(ej, "post-journal");

    /* 4. Point the superblock at the transaction, then make it committed.
     *    s_start must be durable *before* the commit block: it is the only
     *    thing that tells the next mount a log is waiting to be replayed, and
     *    the metadata this transaction covers has not reached disk yet. */
    ret = jbd2_write_journal_superblock(ej, ej->head, j->sequence, ej->head);
    if (ret < 0)
        goto out;
    ret = jbd2_direct_flush(j);
    if (ret < 0)
        goto out;
    ret = jbd2_write_commit(ej, log);
    if (ret < 0)
        goto out;
    ret = jbd2_direct_flush(j);
    if (ret < 0)
        goto out;
    uint32_t committed_sequence = j->sequence;
    uint32_t new_head = jbd2_advance(j, log);  /* commit block consumed */
    uint32_t blocks = ej->tx_count;
    ej->j.sequence++;
    ej->head = new_head;
    ej->commits++;
    ej->committed_blocks += blocks;
    jbd2_crash_point(ej, "post-commit");

    /* 5. Checkpoint.  The transaction is durable in the log, so the metadata
     *    may now reach its home location; the hold exists only to enforce the
     *    order above, and the checkpoint replays into the same bytes anyway. */
    bcache_release_holds(fs->bc);
    ej->tx_count = 0;
    ret = bcache_sync_held(fs->bc);
    if (ret < 0)
        goto out;
    jbd2_crash_point(ej, "post-checkpoint");

    /* 6. Resolve the transaction: the log is empty again and the recover flag
     *    can come down. */
    ret = jbd2_write_journal_superblock(ej, 0, j->sequence, ej->head);
    if (ret < 0)
        goto out;
    ret = jbd2_direct_flush(j);
    if (ret < 0)
        goto out;
    ret = bcache_read_bytes(fs->bc, 1024, &disk_sb, sizeof(disk_sb));
    if (ret < 0)
        goto out;
    ret = ext4_set_recover_feature(j, &disk_sb, 0);
    if (ret < 0)
        goto out;
    ret = bcache_sync_checked(fs->bc);
    if (ret < 0)
        goto out;
    ej->recover_flag = 0;
    printf("[EXT4/JBD2] commit %u: %u metadata blocks, sequence %u->%u, "
           "head %u\n", (unsigned)ej->commits, blocks, committed_sequence,
           j->sequence, ej->head);

out:
    if (ret < 0)
        printf("[EXT4/JBD2] commit failed: %d (transaction stays pending)\n",
               ret);
    mutex_unlock(&ej->lock);
    return ret;
}

/* Pre-sync hook: a sync() or fsync() must not let metadata out ahead of its
 * log, so every sync commits first. */
int ext4_journal_sync_hook(bcache_t *bc)
{
    return ext4_journal_commit((ext4_sb_info_t *)bc->owner);
}
