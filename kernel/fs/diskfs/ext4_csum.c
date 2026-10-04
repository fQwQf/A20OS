/* ext4 metadata checksums (EXT4_FEATURE_RO_COMPAT_METADATA_CSUM).
 *
 * mke2fs turns metadata_csum on by default, so every image this kernel mounts
 * carries checksums on the superblock, the group descriptors, the inode table,
 * both bitmaps and every directory block.  Reading does not need them -- the
 * kernel's own parsers work fine on a stale checksum -- but writing does.  A
 * block written out with whatever checksum it happened to carry when it was
 * read is exactly what e2fsck reports as "inode checksum does not match", and
 * after a single guest write the file was invisible to debugfs, dumpe2fs and
 * e2fsck alike.  A filesystem whose inode checksums are wrong is also one
 * whose damage cannot be told apart from corruption, so this is load-bearing
 * rather than cosmetic.
 *
 * The shape of every formula below is ext4_chksum(): crc32c over the fs UUID
 * to get the seed, then the object seed (inode number, group number, biased
 * group number for the inode bitmap), then the object's own bytes with its own
 * checksum field read as zero.  The seed was confirmed against the value
 * dumpe2fs prints as "Checksum seed", and every formula was then checked
 * against a freshly mkfs'd image: e2fsck accepts the result and every
 * structure the kernel has not yet touched still matches.
 */
#include "fs/ext4.h"
#include "fs/ext4_internal.h"
#include "core/string.h"

/* i_checksum_lo sits inside the old i_osd2 tail and i_checksum_hi follows
 * i_extra_isize.  Both live outside the 128-byte ext4_inode_t this driver
 * keeps in memory, which is why writing the struct verbatim used to corrupt
 * every one of them. */
#define EXT4_INODE_CHECKSUM_LO_OFF 0x7c
#define EXT4_INODE_GENERATION_OFF  0x64
#define EXT4_INODE_CHECKSUM_HI_OFF 0x82

/* The dirent-area tail appended to a checksummed directory block. */
#define EXT4_DIR_TAIL_OFF   0x08
#define EXT4_DIR_TAIL_SZ    0x0c
#define EXT4_DIR_TAIL_RECLEN 0x0c

static void ext4_csum_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void ext4_csum_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* crc32c(~0, uuid) -- dumpe2fs's "Checksum seed", and the root of every
 * metadata checksum on the filesystem. */
uint32_t ext4_checksum_seed(const ext4_sb_info_t *sb)
{
    return ext4_crc32c(0xffffffffU, sb->s_uuid, sizeof(sb->s_uuid));
}

int ext4_has_metadata_csum(const ext4_sb_info_t *sb)
{
    return (sb->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) != 0;
}

/* The two on-disk descriptor shapes disagree about where bg_checksum lives: the
 * 64-byte descriptor carries bg_exclude_bitmap_lo, which pushes every later
 * field four bytes further out than in the legacy 32-byte one. */
size_t ext4_group_desc_checksum_offset(size_t desc_size)
{
    return desc_size >= 64 ? 0x1e : 0x1c;
}

void ext4_group_desc_checksum_put(ext4_sb_info_t *sb, uint32_t group,
                                  uint8_t *raw, size_t desc_size)
{
    uint8_t le_group[4];
    ext4_csum_put_le32(le_group, group);

    size_t csum_off = ext4_group_desc_checksum_offset(desc_size);
    ext4_csum_put_le16(raw + csum_off, 0);
    uint32_t crc = ext4_checksum_seed(sb);
    crc = ext4_crc32c(crc, le_group, sizeof(le_group));
    crc = ext4_crc32c(crc, raw, desc_size);
    ext4_csum_put_le16(raw + csum_off, (uint16_t)(crc & 0xffffU));
}

/* Rewrite the in-memory descriptor (always the 64-byte layout) into the
 * narrower on-disk shape a filesystem with s_desc_size == 32 reads back. */
void ext4_group_desc_pack_legacy(const ext4_group_desc_t *gd, uint8_t *out)
{
    memset(out, 0, 32);
    memcpy(out + 0x00, &gd->bg_block_bitmap_lo, 4);
    memcpy(out + 0x04, &gd->bg_inode_bitmap_lo, 4);
    memcpy(out + 0x08, &gd->bg_inode_table_lo, 4);
    memcpy(out + 0x0c, &gd->bg_free_blocks_count_lo, 2);
    memcpy(out + 0x0e, &gd->bg_free_inodes_count_lo, 2);
    memcpy(out + 0x10, &gd->bg_used_dirs_count_lo, 2);
    memcpy(out + 0x12, &gd->bg_flags, 2);
    memcpy(out + 0x14, &gd->bg_block_bitmap_csum_lo, 2);
    memcpy(out + 0x16, &gd->bg_inode_bitmap_csum_lo, 2);
    memcpy(out + 0x18, &gd->bg_itable_unused_lo, 2);
    memcpy(out + 0x1a, &gd->bg_checksum, 2);
    /* The 32-byte descriptor has no high halves at all; its tail after the
     * checksum is unused. */
    memset(out + 0x1c, 0, 4);
}

void ext4_inode_checksum_put(ext4_sb_info_t *sb, uint32_t ino, uint8_t *raw,
                             size_t inode_size)
{
    uint8_t le_ino[4];
    ext4_csum_put_le32(le_ino, ino);

    /* i_generation is folded in ahead of the inode body; offset and count are
     * both zero for inodes and contribute nothing. */
    uint8_t generation[4];
    memcpy(generation, raw + EXT4_INODE_GENERATION_OFF, sizeof(generation));

    ext4_csum_put_le16(raw + EXT4_INODE_CHECKSUM_LO_OFF, 0);
    ext4_csum_put_le16(raw + EXT4_INODE_CHECKSUM_HI_OFF, 0);

    uint32_t crc = ext4_checksum_seed(sb);
    crc = ext4_crc32c(crc, le_ino, sizeof(le_ino));
    crc = ext4_crc32c(crc, generation, sizeof(generation));
    crc = ext4_crc32c(crc, raw, inode_size);

    ext4_csum_put_le16(raw + EXT4_INODE_CHECKSUM_LO_OFF,
                       (uint16_t)(crc & 0xffffU));
    ext4_csum_put_le16(raw + EXT4_INODE_CHECKSUM_HI_OFF,
                       (uint16_t)((crc >> 16) & 0xffffU));
}

/* Both bitmaps hash straight off the UUID seed with no group number folded in
 * -- the group is already part of the descriptor that carries the result, and
 * mke2fs has never hashed it into the bitmap checksum itself.  The block bitmap
 * is hashed over its whole block; the inode bitmap only over the bytes its
 * bits can reach, which is inodes_per_group / 8. */
void ext4_block_bitmap_checksum_put(ext4_sb_info_t *sb, uint32_t group,
                                    const uint8_t *raw, size_t len)
{
    uint32_t crc = ext4_crc32c(ext4_checksum_seed(sb), raw, len);
    sb->group_descs[group].bg_block_bitmap_csum_lo = (uint16_t)(crc & 0xffffU);
    sb->group_descs[group].bg_block_bitmap_csum_hi = (uint16_t)(crc >> 16);
}

void ext4_inode_bitmap_checksum_put(ext4_sb_info_t *sb, uint32_t group,
                                    const uint8_t *raw, size_t len)
{
    uint32_t crc = ext4_crc32c(ext4_checksum_seed(sb), raw, len);
    sb->group_descs[group].bg_inode_bitmap_csum_lo = (uint16_t)(crc & 0xffffU);
    sb->group_descs[group].bg_inode_bitmap_csum_hi = (uint16_t)(crc >> 16);
}

/* A checksummed directory block ends in a 12-byte tail rather than the unused
 * inode number a non-checksummed block ends with: four reserved zero bytes,
 * rec_len 12, a zero name length, file type 0xDE ("EXT4_DIRENT_TAIL"), and the
 * block's own checksum in the last four bytes. */
size_t ext4_dir_block_tail_off(size_t block_size)
{
    return block_size - EXT4_DIR_TAIL_SZ;
}

void ext4_dir_block_tail_set(uint8_t *raw, size_t block_size)
{
    uint8_t *tail = raw + ext4_dir_block_tail_off(block_size);
    memset(tail, 0, EXT4_DIR_TAIL_SZ);
    ext4_csum_put_le16(tail + 0x04, EXT4_DIR_TAIL_RECLEN);
    tail[0x06] = 0;
    tail[0x07] = EXT4_FT_DIRENT_TAIL;
}

/* The directory checksum covers the entries only -- the tail is excluded
 * entirely -- and its object seed is (inode number, inode generation).  It is
 * the generation rather than the logical block index that distinguishes two
 * blocks of the same directory; with i_generation pinned at 0 on a fresh
 * filesystem the two coincide, and it is the multi-block directory that tells
 * them apart. */
void ext4_dir_block_checksum_put(ext4_sb_info_t *sb, uint32_t ino,
                                 uint32_t generation, uint8_t *raw,
                                 size_t block_size)
{
    uint8_t le_ino[4], le_gen[4];
    ext4_csum_put_le32(le_ino, ino);
    ext4_csum_put_le32(le_gen, generation);

    uint32_t crc = ext4_crc32c(ext4_checksum_seed(sb), le_ino, sizeof(le_ino));
    crc = ext4_crc32c(crc, le_gen, sizeof(le_gen));
    crc = ext4_crc32c(crc, raw, ext4_dir_block_tail_off(block_size));

    ext4_csum_put_le32(raw + block_size - 4, crc);
}

/* An extent block carries a bare crc32c in its last four bytes, over everything
 * before it, seeded the same way as a directory block. */
void ext4_extent_block_checksum_put(ext4_sb_info_t *sb, uint32_t ino,
                                    uint32_t generation, uint8_t *raw,
                                    size_t block_size)
{
    uint8_t le_ino[4], le_gen[4];
    ext4_csum_put_le32(le_ino, ino);
    ext4_csum_put_le32(le_gen, generation);

    uint32_t crc = ext4_crc32c(ext4_checksum_seed(sb), le_ino, sizeof(le_ino));
    crc = ext4_crc32c(crc, le_gen, sizeof(le_gen));
    crc = ext4_crc32c(crc, raw, block_size - 4);

    ext4_csum_put_le32(raw + block_size - 4, crc);
}
