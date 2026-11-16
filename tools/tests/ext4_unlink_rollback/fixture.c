/* Real shared ext4/cache: a failed dirent write must not leave a named orphan. */
#define main ext4_other_scenarios_main
#define pwrite unlink_fault_pwrite
#include "../../../kernel/fs/diskfs/tests/ext4_partial_trunc_host.c"
#undef pwrite
#undef main
extern long pwrite(int, const void *, unsigned long, long);

static uint64_t fault_offset;
static unsigned armed, hits, failures = 1;
long unlink_fault_pwrite(int fd, const void *buffer, unsigned long size, long off)
{
    if (armed && hits < failures && off <= (long)fault_offset &&
        (uint64_t)off + size > fault_offset) {
        hits++;
        return -1;
    }
    return pwrite(fd, buffer, size, off);
}
static int unlink_device_write(struct block_dev *dev, uint64_t lba,
                                 const void *buffer, size_t count)
{
    (void)dev;
    return fsio_write((uint32_t)lba, buffer, (uint32_t)count) ? -EIO : 0;
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
    if (argc == 3) {
        if (strcmp(argv[2], "persistent")) return 1;
        failures = 3;
    }
    if (mount_image(argv[1]) < 0) return 2;
    g_bc->dev->write_sector = unlink_device_write;
    ext4_vnode_priv_t *root = g_root->fs_data;
    ext4_sb_info_t *sb = root->sb;
    ext4_inode_t directory;
    if (ext4_read_inode(sb, root->inode_num, &directory) < 0) return 3;
    fault_offset = ext4_block_map(sb, &directory, 0) * sb->block_size;
    vnode_t *file = NULL;
    if (ext4_lookup(g_root, "single", &file) < 0) return 4;
    uint32_t ino = ((ext4_vnode_priv_t *)file->fs_data)->inode_num;
    const char *remove_name = "single";
    unsigned original_links = 1;
#ifdef EXT4_TEST_HARDLINK_UNLINK
    if (ext4_vn_link(g_root, "alias", file) < 0) return 15;
    remove_name = "alias";
    original_links = 2;
    uint32_t group = (ino - 1) / sb->inodes_per_group;
    uint32_t index = (ino - 1) % sb->inodes_per_group;
    /* The runner deliberately formats this fixture without the 64bit feature. */
    uint64_t table = sb->group_descs[group].bg_inode_table_lo;
    fault_offset = table * sb->block_size + (uint64_t)index * sb->inode_size +
                   offsetof(ext4_inode_t, i_links_count);
#endif
    armed = 1;
    int rc = ext4_vn_unlink(g_root, remove_name);
    armed = 0;
    printf("UNLINK_ROLLBACK: rc=%d hits=%u\n", rc, hits);
    if (hits != failures || rc != (failures == 1 ? -EIO : -117)) return 5;
    if (failures == 3) {
        vnode_t *created = NULL;
        if (!ext4_is_inconsistent(sb) ||
            ext4_vn_create(g_root, "must-not-exist", 0600, &created) != -117)
            return 6;
        host_report("ORPHAN_QUARANTINE_PASS\n");
    } else {
        ext4_inode_t restored;
        ext4_superblock_t disk;
        if (ext4_is_inconsistent(sb) || ext4_read_inode(sb, ino, &restored) < 0 ||
            restored.i_links_count != original_links ||
            bcache_read_bytes(g_bc, EXT4_SB_OFFSET, &disk, sizeof(disk)) < 0 ||
            disk.s_last_orphan != 0) return 7;
        vnode_t *found = NULL;
        if (ext4_lookup(g_root, "single", &found) < 0 || found != file) return 8;
        vnode_put(found);
        if (ext4_lookup(g_root, remove_name, &found) < 0 || found != file) return 16;
        vnode_put(found);
        uint8_t page[4096];
        if (ext4_vn_readpage(file, 0, page, sizeof(page)) < 0) return 10;
        for (unsigned i = 0; i < sizeof(page); i++)
            if (page[i] != (uint8_t)((i * 37 + 19) % 251)) return 11;
#ifdef EXT4_TEST_HARDLINK_UNLINK
        /* Also fail after the count write was accepted, at name removal. */
        fault_offset = ext4_block_map(sb, &directory, 0) * sb->block_size;
        hits = 0; armed = 1;
        rc = ext4_vn_unlink(g_root, remove_name);
        armed = 0;
        if (rc != -EIO || hits != 1 ||
            ext4_read_inode(sb, ino, &restored) < 0 || restored.i_links_count != 2)
            return 18;
        if (ext4_lookup(g_root, remove_name, &found) < 0 || found != file) return 19;
        vnode_put(found);
#endif
        if (ext4_vn_unlink(g_root, remove_name) < 0) return 12;
#ifdef EXT4_TEST_HARDLINK_UNLINK
        if (ext4_vn_unlink(g_root, "single") < 0) return 17;
#endif
        vnode_put(file);
        if (ext4_unmount(g_root) < 0) return 13;
        g_root = NULL;
        host_report("ORPHAN_RETRY_PASS\n");
    }
    if (bcache_sync_checked(g_bc) < 0 || fsync(g_fd)) return 14;
    close(g_fd);
    return failures == 3 ? 117 : 0;
}
