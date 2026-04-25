#ifndef _EXT4_H
#define _EXT4_H

#include "core/types.h"
#include "fs/vfs.h"
#include "fs/block_cache.h"

#define EXT4_DISK_MAGIC        0xEF53

#define EXT4_BLOCK_SIZE_MIN    1024
#define EXT4_INODE_SIZE_DEFAULT 256

#define EXT4_GOOD_OLD_REV      0
#define EXT4_DYNAMIC_REV       1

#define EXT4_SECRM_FL          0x00000001
#define EXT4_UNRM_FL           0x00000002
#define EXT4_COMPR_FL          0x00000004
#define EXT4_SYNC_FL           0x00000008
#define EXT4_IMMUTABLE_FL      0x00000010
#define EXT4_APPEND_FL         0x00000020
#define EXT4_NODUMP_FL         0x00000040
#define EXT4_NOATIME_FL        0x00000080
#define EXT4_DIRSYNC_FL        0x00010000
#define EXT4_TOPDIR_FL         0x00020000
#define EXT4_HUGE_FILE_FL      0x00040000
#define EXT4_EXTENTS_FL        0x00080000
#define EXT4_EA_INODE_FL       0x00200000

#define EXT4_FEATURE_COMPAT_EXT_ATTR     0x0008
#define EXT4_FEATURE_COMPAT_RESIZE_INODE 0x0010
#define EXT4_FEATURE_COMPAT_DIR_INDEX    0x0020

#define EXT4_FEATURE_INCOMPAT_COMPRESSION  0x0001
#define EXT4_FEATURE_INCOMPAT_FILETYPE     0x0002
#define EXT4_FEATURE_INCOMPAT_RECOVER      0x0004
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV  0x0008
#define EXT4_FEATURE_INCOMPAT_META_BG      0x0010
#define EXT4_FEATURE_INCOMPAT_EXTENTS      0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT        0x0080
#define EXT4_FEATURE_INCOMPAT_MMP          0x0100
#define EXT4_FEATURE_INCOMPAT_FLEX_BG      0x0200
#define EXT4_FEATURE_INCOMPAT_EA_INODE     0x0400
#define EXT4_FEATURE_INCOMPAT_DIRDATA      0x1000

#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER  0x0001
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE    0x0002
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR     0x0004
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE     0x0008
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM      0x0010
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK     0x0020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE   0x0040

#define EXT4_FT_UNKNOWN    0
#define EXT4_FT_REG_FILE   1
#define EXT4_FT_DIR        2
#define EXT4_FT_CHRDEV     3
#define EXT4_FT_BLKDEV     4
#define EXT4_FT_FIFO       5
#define EXT4_FT_SOCK       6
#define EXT4_FT_SYMLINK    7

#define EXT4_ROOT_INO      2

#define S_IFLNK            0120000

typedef struct __attribute__((packed)) ext4_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint16_t s_journal_uuid[8];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_reserved_char_pad;
    uint16_t s_reserved_pad;
    uint32_t s_reserved[162];
} ext4_superblock_t;

typedef struct __attribute__((packed)) ext4_group_desc {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} ext4_group_desc_t;

#define EXT4_INODE_SIZE_STATIC 128

typedef struct __attribute__((packed)) ext4_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    union {
        struct {
            uint32_t i_block[15];
        } i_data;
        struct {
            uint16_t eh_magic;
            uint16_t eh_entries;
            uint16_t eh_max;
            uint16_t eh_depth;
            uint32_t eh_generation;
            uint8_t  eh_data[60 - 12];
        } i_extent;
    } i_block;
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;
    uint32_t i_obso_faddr;
    uint32_t i_osd2[3];
    uint16_t i_extra_isize;
    uint16_t i_pad1;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
} ext4_inode_t;

#define EXT4_EXT_MAGIC      0xF30A

typedef struct __attribute__((packed)) ext4_extent_header {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} ext4_extent_header_t;

typedef struct __attribute__((packed)) ext4_extent_idx {
    uint32_t ei_block;
    uint32_t ei_leaf_lo;
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} ext4_extent_idx_t;

typedef struct __attribute__((packed)) ext4_extent {
    uint32_t ee_block;
    uint16_t ee_len;
    uint16_t ee_start_hi;
    uint32_t ee_start_lo;
} ext4_extent_t;

typedef struct __attribute__((packed)) ext4_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[0];
} ext4_dir_entry_t;

typedef struct ext4_sb_info {
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t first_data_block;
    uint32_t groups_count;
    uint32_t addr_per_block;
    uint32_t desc_per_block;
    uint32_t desc_size;
    uint32_t inodes_count;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint64_t block_group_desc_table_byte;
    ext4_group_desc_t *group_descs;
    bcache_t *bc;
} ext4_sb_info_t;

typedef struct ext4_vnode_priv {
    ext4_sb_info_t *sb;
    uint32_t        inode_num;
    uint32_t        file_size;
    int             type;
} ext4_vnode_priv_t;

typedef struct ext4_fctx {
    ext4_sb_info_t *sb;
    uint32_t        inode_num;
    uint32_t        file_size;
    int             is_dir;
    size_t          file_off;
    size_t          dir_off;
} ext4_fctx_t;

vnode_t *ext4_mount(bcache_t *bc);
void     ext4_unmount(vnode_t *root);
vfile_t *ext4_open_vnode(vnode_t *vn, int flags);

#endif
