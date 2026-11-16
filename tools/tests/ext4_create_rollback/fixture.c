/* Shared ext4/cache: private object cleanup and mkdir parent-count recovery. */
#define main ext4_other_scenarios_main
#define pwrite create_fault_pwrite
#include "../../../kernel/fs/diskfs/tests/ext4_partial_trunc_host.c"
#undef pwrite
#undef main
extern long pwrite(int, const void *, unsigned long, long);

static uint64_t fault_offset;
static unsigned armed, hits, cleanup_hits, persistent, match_value;
static int value_width;
long create_fault_pwrite(int fd, const void *bytes, unsigned long size, long off)
{
    if (armed && off <= (long)fault_offset && (uint64_t)off + size > fault_offset) {
        unsigned value = 0;
        uint64_t at = fault_offset - off;
        if (value_width && at + value_width <= size)
            memcpy(&value, (const char *)bytes + at, value_width);
        if (!hits && (!value_width || value == match_value)) {
            hits++; return -1;
        }
        /* Fail both checked inode-clear attempts after failed initialization. */
        if (persistent && hits && value_width && value == 0 && cleanup_hits < 2) {
            cleanup_hits++; return -1;
        }
    }
    return pwrite(fd, bytes, size, off);
}
static int create_device_write(struct block_dev *dev, uint64_t lba,
                                const void *bytes, size_t count)
{
    (void)dev;
    return fsio_write((uint32_t)lba, bytes, (uint32_t)count) ? -EIO : 0;
}
static uint64_t inode_position(ext4_sb_info_t *sb, uint32_t ino)
{
    uint32_t group = (ino - 1) / sb->inodes_per_group;
    return (uint64_t)sb->group_descs[group].bg_inode_table_lo * sb->block_size +
           (uint64_t)((ino - 1) % sb->inodes_per_group) * sb->inode_size;
}
static int unchanged(ext4_sb_info_t *sb, const char *name,
                       const kstatfs_t *baseline, unsigned dirs, unsigned links)
{
    kstatfs_t got;
    ext4_inode_t parent;
    vnode_t *found = NULL;
    return !ext4_is_inconsistent(sb) &&
        ext4_lookup(g_root, name, &found) == -ENOENT &&
        ext4_vn_statfs(g_root, &got) == 0 &&
        got.f_bfree == baseline->f_bfree && got.f_ffree == baseline->f_ffree &&
        sb->group_descs[0].bg_used_dirs_count_lo == dirs &&
        ext4_read_inode(sb, EXT4_ROOT_INO, &parent) == 0 &&
        parent.i_links_count == links;
}
int main(int argc, char **argv)
{
    fscompat_log_sink = host_log_sink;
    if (argc < 2 || argc > 3) return 1;
    if (argc == 3 && !strcmp(argv[2], "verify-quarantine")) {
        if (mount_image(argv[1]) >= 0) return 9;
        host_report("ORPHAN_COLD_QUARANTINE_PASS\n");
        return 0;
    }
    persistent = argc == 3;
    if (persistent && strcmp(argv[2], "persistent")) return 1;
    if (mount_image(argv[1]) < 0) return 2;
    g_bc->dev->write_sector = create_device_write;
    ext4_vnode_priv_t *root = g_root->fs_data;
    ext4_sb_info_t *sb = root->sb;
    ext4_inode_t parent;
    kstatfs_t baseline;
    if (ext4_read_inode(sb, root->inode_num, &parent) < 0 ||
        ext4_vn_statfs(g_root, &baseline) < 0) return 3;
    unsigned dirs = sb->group_descs[0].bg_used_dirs_count_lo;
    unsigned links = parent.i_links_count;
    uint64_t directory = ext4_block_map(sb, &parent, 0) * sb->block_size;

    /* Reserve/release once to select the next inode deterministically without
     * depending on mkfs inode numbering. free_inode resets its allocation hint. */
    uint32_t next = ext4_alloc_inode(sb);
    if (!next || ext4_free_inode(sb, next) < 0) return 4;
    fault_offset = inode_position(sb, next) + offsetof(ext4_inode_t, i_mode);
    value_width = 2; match_value = S_IFLNK | 0777; armed = 1;
    int rc = ext4_vn_symlink(g_root, "symlink-init", "single");
    armed = 0;
    if (hits != 1 || rc != (persistent ? -117 : -EIO)) return 5;
    if (persistent) {
        vnode_t *created = NULL;
        kstatfs_t retained;
        if (cleanup_hits != 2 || !ext4_is_inconsistent(sb) ||
            ext4_vn_statfs(g_root, &retained) < 0 ||
            retained.f_ffree + 1 != baseline.f_ffree ||
            ext4_vn_create(g_root, "must-not-exist", 0600, &created) != -117)
            return 6;
        host_report("ORPHAN_QUARANTINE_PASS\n");
    } else {
        if (!unchanged(sb, "symlink-init", &baseline, dirs, links)) return 7;
        hits = 0; match_value = S_IFREG | 0600; armed = 1;
        vnode_t *created = NULL;
        rc = ext4_vn_create(g_root, "file-init", 0600, &created);
        armed = 0;
        if (rc != -EIO || hits != 1 || created ||
            !unchanged(sb, "file-init", &baseline, dirs, links)) return 8;

        fault_offset = directory; value_width = 0;
        hits = 0; armed = 1;
        rc = ext4_vn_create(g_root, "file-publish", 0600, &created);
        armed = 0;
        if (rc != -EIO || hits != 1 || created ||
            !unchanged(sb, "file-publish", &baseline, dirs, links)) return 10;
        hits = 0; armed = 1;
        rc = ext4_vn_symlink(g_root, "symlink-publish", "single");
        armed = 0;
        if (rc != -EIO || hits != 1 ||
            !unchanged(sb, "symlink-publish", &baseline, dirs, links)) return 11;

        fault_offset = inode_position(sb, root->inode_num) + offsetof(ext4_inode_t, i_links_count);
        value_width = 2; match_value = links + 1;
        hits = 0; armed = 1;
        rc = ext4_vn_mkdir(g_root, "parent-count", 0700);
        armed = 0;
        if (rc != -EIO || hits != 1 ||
            !unchanged(sb, "parent-count", &baseline, dirs, links)) return 12;
        fault_offset = directory; value_width = 0;
        hits = 0; armed = 1;
        rc = ext4_vn_mkdir(g_root, "directory-publish", 0700);
        armed = 0;
        if (rc != -EIO || hits != 1 ||
            !unchanged(sb, "directory-publish", &baseline, dirs, links)) return 13;

        if (ext4_vn_create(g_root, "file-publish", 0600, &created) < 0 || !created)
            return 14;
        vnode_put(created);
        if (ext4_vn_unlink(g_root, "file-publish") < 0 ||
            ext4_vn_symlink(g_root, "symlink-publish", "single") < 0) return 15;
        vnode_t *symlink = NULL;
        char target[16];
        if (ext4_lookup(g_root, "symlink-publish", &symlink) < 0 ||
            ext4_readlink(symlink, target, sizeof(target)) != 6 || strcmp(target, "single"))
            return 16;
        vnode_put(symlink);
        if (ext4_vn_unlink(g_root, "symlink-publish") < 0 ||
            ext4_vn_mkdir(g_root, "directory-publish", 0700) < 0 ||
            ext4_vn_rmdir(g_root, "directory-publish") < 0 ||
            !unchanged(sb, "directory-publish", &baseline, dirs, links)) return 17;
        if (ext4_unmount(g_root) < 0) return 18;
        g_root = NULL;
        host_report("ORPHAN_RETRY_PASS\n");
    }
    if (bcache_sync_checked(g_bc) < 0 || fsync(g_fd)) return 19;
    close(g_fd);
    return persistent ? 117 : 0;
}
