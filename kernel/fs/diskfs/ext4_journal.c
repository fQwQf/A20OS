#include "fs/ext4_internal.h"
#include "mm/slab.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/stdio.h"

/*
 * Minimal, fail-closed JBD2 recovery for ext4's internal journal.
 *
 * A20OS does not implement a writable journal.  This file only replays a
 * pre-existing, checksummed recovery log before the filesystem becomes
 * writable, then marks that log empty and clears EXT4_FEATURE_INCOMPAT_RECOVER.
 * Unknown JBD2 formats are rejected instead of being mounted unsafely.
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

typedef struct jbd2_recovery {
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
} jbd2_recovery_t;

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

static uint32_t crc32c(uint32_t seed, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = seed;
    crc32c_init_table();
    while (len--)
        crc = crc32c_table[(crc ^ *p++) & 0xffU] ^ (crc >> 8);
    return crc;
}

static uint32_t jbd2_advance(const jbd2_recovery_t *j, uint32_t block)
{
    block++;
    return block == j->maxlen ? j->first : block;
}

static int jbd2_read_block(jbd2_recovery_t *j, uint32_t logical, void *buffer)
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

static int jbd2_write_block(jbd2_recovery_t *j, uint32_t logical,
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

static int jbd2_metadata_checksum_ok(jbd2_recovery_t *j, uint8_t *block)
{
    uint32_t offset = j->block_size - JBD2_CHECKSUM_TAIL_BYTES;
    uint32_t stored = get_be32(block + offset);
    put_be32(block + offset, 0);
    uint32_t calculated = crc32c(j->checksum_seed, block, j->block_size);
    put_be32(block + offset, stored);
    return calculated == stored;
}

static int jbd2_commit_checksum_ok(jbd2_recovery_t *j, uint8_t *block)
{
    uint32_t stored = get_be32(block + 16);
    put_be32(block + 16, 0);
    uint32_t calculated = crc32c(j->checksum_seed, block, j->block_size);
    put_be32(block + 16, stored);
    return calculated == stored;
}

static int jbd2_data_checksum_ok(jbd2_recovery_t *j, const uint8_t *block,
                                 uint32_t sequence, uint32_t stored)
{
    uint8_t encoded_sequence[4];
    put_be32(encoded_sequence, sequence);
    uint32_t crc = crc32c(j->checksum_seed, encoded_sequence,
                          sizeof(encoded_sequence));
    crc = crc32c(crc, block, j->block_size);
    return crc == stored;
}

static int jbd2_tid_geq(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) >= 0;
}

static int jbd2_set_revoke(jbd2_recovery_t *j, uint64_t block,
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

static int jbd2_is_revoked(jbd2_recovery_t *j, uint64_t block,
                           uint32_t sequence)
{
    for (uint32_t i = 0; i < j->revoke_count; i++) {
        if (j->revokes[i].block == block &&
            jbd2_tid_geq(j->revokes[i].sequence, sequence))
            return 1;
    }
    return 0;
}

static int jbd2_process_revoke(jbd2_recovery_t *j, uint8_t *block,
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

static int jbd2_process_descriptor(jbd2_recovery_t *j, uint8_t *descriptor,
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
            if (ret < 0)
                return ret;
            if (!jbd2_data_checksum_ok(j, j->data, sequence, checksum))
                return -EIO;
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

static int jbd2_run_pass(jbd2_recovery_t *j, enum jbd2_recovery_pass pass)
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
        if (ret < 0)
            return ret;
        log_block = jbd2_advance(j, log_block);
        consumed++;

        uint32_t magic = get_be32(j->meta);
        uint32_t type = get_be32(j->meta + 4);
        uint32_t block_sequence = get_be32(j->meta + 8);
        if (magic != JBD2_MAGIC_NUMBER || block_sequence != sequence) {
            if (pass == JBD2_PASS_SCAN)
                break;
            return -EIO;
        }

        switch (type) {
        case JBD2_DESCRIPTOR_BLOCK:
            ret = jbd2_process_descriptor(j, j->meta, sequence, &log_block,
                                          &consumed, pass);
            if (ret < 0)
                return ret;
            break;
        case JBD2_REVOKE_BLOCK:
            ret = jbd2_process_revoke(j, j->meta, sequence, pass);
            if (ret < 0)
                return ret;
            break;
        case JBD2_COMMIT_BLOCK:
            if (!jbd2_commit_checksum_ok(j, j->meta))
                return -EIO;
            sequence++;
            head = log_block;
            break;
        default:
            return -EOPNOTSUPP;
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
    uint32_t calculated = crc32c(0xffffffffU, block,
                                 JBD2_SUPERBLOCK_BYTES);
    put_be32(block + JBD2_SUPERBLOCK_CHECKSUM_OFF, stored);
    return calculated == stored;
}

static int jbd2_load_superblock(jbd2_recovery_t *j)
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
    j->checksum_seed = crc32c(0xffffffffU, j->uuid, sizeof(j->uuid));

    if (j->block_size != j->fs->block_size || j->block_size != 4096 ||
        j->maxlen < 2 || j->maxlen > JBD2_MAX_JOURNAL_BLOCKS ||
        j->first < 1 || j->first >= j->maxlen ||
        (j->start && (j->start < j->first || j->start >= j->maxlen)) ||
        ext4_inode_size(&j->journal_inode) /
            j->block_size < j->maxlen)
        return -EINVAL;
    return 0;
}

static int jbd2_mark_empty(jbd2_recovery_t *j)
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
    uint32_t checksum = crc32c(0xffffffffU, j->meta,
                               JBD2_SUPERBLOCK_BYTES);
    put_be32(j->meta + JBD2_SUPERBLOCK_CHECKSUM_OFF, checksum);
    return jbd2_write_block(j, 0, j->meta);
}

static int ext4_clear_recover_feature(jbd2_recovery_t *j,
                                      ext4_superblock_t *disk_sb)
{
    ext4_superblock_t recovered;
    if (bcache_read_bytes(j->fs->bc, 1024, &recovered,
                          sizeof(recovered)) < 0)
        return -EIO;
    if (recovered.s_magic != EXT4_DISK_MAGIC)
        return -EINVAL;
    uint8_t *raw = (uint8_t *)&recovered;
    if (recovered.s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        uint32_t stored = (uint32_t)raw[EXT4_SUPERBLOCK_CHECKSUM_OFF] |
                          ((uint32_t)raw[EXT4_SUPERBLOCK_CHECKSUM_OFF + 1] << 8) |
                          ((uint32_t)raw[EXT4_SUPERBLOCK_CHECKSUM_OFF + 2] << 16) |
                          ((uint32_t)raw[EXT4_SUPERBLOCK_CHECKSUM_OFF + 3] << 24);
        uint32_t calculated = crc32c(0xffffffffU, raw,
                                     EXT4_SUPERBLOCK_CHECKSUM_OFF);
        if (stored != calculated)
            return -EIO;
    }
    recovered.s_feature_incompat &= ~EXT4_FEATURE_INCOMPAT_RECOVER;
    if (recovered.s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        uint32_t checksum = crc32c(0xffffffffU, raw,
                                   EXT4_SUPERBLOCK_CHECKSUM_OFF);
        put_le32(raw + EXT4_SUPERBLOCK_CHECKSUM_OFF, checksum);
    }
    if (bcache_write_bytes(j->fs->bc, 1024, &recovered,
                           sizeof(recovered)) < 0)
        return -EIO;
    *disk_sb = recovered;
    j->fs->s_feature_incompat = recovered.s_feature_incompat;
    return 0;
}

int ext4_journal_recover(ext4_sb_info_t *fs, ext4_superblock_t *disk_sb)
{
    if (!fs || !disk_sb ||
        !(disk_sb->s_feature_compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) ||
        !disk_sb->s_journal_inum)
        return -EINVAL;

    jbd2_recovery_t j;
    memset(&j, 0, sizeof(j));
    j.fs = fs;
    int ret = ext4_read_inode(fs, disk_sb->s_journal_inum,
                              &j.journal_inode);
    if (ret < 0)
        return ret;

    /* The official images use 4 KiB journal blocks.  Allocate before
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
        ret = jbd2_run_pass(&j, JBD2_PASS_SCAN);
        if (ret < 0)
            goto out;
        if (j.scan_revoke_records) {
            j.revoke_capacity = j.scan_revoke_records;
            j.revokes = (jbd2_revoke_entry_t *)kmalloc(
                (size_t)j.revoke_capacity * sizeof(*j.revokes));
            if (!j.revokes) {
                ret = -ENOMEM;
                goto out;
            }
        }
        ret = jbd2_run_pass(&j, JBD2_PASS_REVOKE);
        if (ret < 0)
            goto out;
        ret = jbd2_run_pass(&j, JBD2_PASS_REPLAY);
        if (ret < 0)
            goto out;
        bcache_sync(fs->bc);
        printf("[EXT4/JBD2] replay complete transactions=%u blocks=%u "
               "revokes=%u hits=%u head=%u\n",
               j.end_sequence - j.sequence, j.replayed_blocks,
               j.revoke_count, j.revoke_hits, j.head);
    }

    ret = jbd2_mark_empty(&j);
    if (ret < 0)
        goto out;
    bcache_sync(fs->bc);
    ret = ext4_clear_recover_feature(&j, disk_sb);
    if (ret < 0)
        goto out;
    bcache_sync(fs->bc);
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
