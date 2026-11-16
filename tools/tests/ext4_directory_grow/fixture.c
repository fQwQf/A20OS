/* Real ext4/cache: restore mapping preimages and the enlarged parent inode. */
#define main ext4_other_scenarios_main
#define pwrite grow_fault_pwrite
#include "../../../kernel/fs/diskfs/tests/ext4_partial_trunc_host.c"
#undef pwrite
#undef main
extern long pwrite(int, const void *, unsigned long, long);

static uint64_t fault_offset;
static unsigned armed, hits, failures = 1, enlarged_inode;
static uint32_t enlarged_size;
long grow_fault_pwrite(int fd, const void *bytes, unsigned long size, long off)
{
    if (armed && hits < failures && off <= (long)fault_offset &&
        (uint64_t)off + size > fault_offset) {
        int match = 1;
        if (enlarged_inode) {
            uint64_t at = fault_offset - off;
            uint32_t value = 0;
            if (at + sizeof(value) > size) return -1;
            memcpy(&value, (const char *)bytes + at, sizeof(value));
            match = value == enlarged_size;
        }
        if (match) { hits++; return -1; }
    }
    return pwrite(fd, bytes, size, off);
}
static int grow_device_write(struct block_dev *dev, uint64_t lba,
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
static void long_name(char *name, unsigned i)
{
    memset(name, 'x', 255); name[0] = 'a' + i; name[255] = 0;
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
    int persistent = argc == 3;
    if (persistent && strcmp(argv[2], "persistent")) return 1;
    if (mount_image(argv[1]) < 0) return 2;
    g_bc->dev->write_sector = grow_device_write;
    ext4_vnode_priv_t *root = g_root->fs_data;
    ext4_sb_info_t *sb = root->sb;
    vnode_t *file = NULL;
    if (ext4_lookup(g_root, "single", &file) < 0) return 3;
    uint32_t ino = ((ext4_vnode_priv_t *)file->fs_data)->inode_num;

    /* Four separated preallocations promote the file's root to depth one.
     * An insertion at EOF must restore the existing leaf on failure without
     * removing any of those mappings beyond EOF. */
    ext4_inode_t inode, original;
    if (ext4_read_inode(sb, ino, &inode) < 0) return 4;
    for (unsigned i = 0; i < 4; i++) {
        uint64_t block = ext4_alloc_block(sb);
        if (!block || ext4_block_grow_commit(sb, ino, &inode, 64 + 4 * i,
                                              block, ext4_inode_size(&inode)) < 0)
            return 5;
    }
    original = inode;
    ext4_extent_header_t hdr;
    ext4_extent_idx_t index;
    memcpy(&hdr, &inode.i_block, sizeof(hdr));
    memcpy(&index, (char *)&inode.i_block + sizeof(hdr), sizeof(index));
    if (hdr.eh_depth != 1 || hdr.eh_entries != 1) return 6;
    uint64_t leaf = (uint64_t)index.ei_leaf_lo | ((uint64_t)index.ei_leaf_hi << 32);
    uint8_t before[4096], after[4096];
    if (sb->block_size != sizeof(before) ||
        bcache_read_bytes(g_bc, leaf * sb->block_size, before, sb->block_size) < 0)
        return 7;
    kstatfs_t baseline, restored;
    if (ext4_vn_statfs(g_root, &baseline) < 0) return 8;
    uint64_t data = ext4_alloc_block(sb);
    if (!data) return 10;
    fault_offset = leaf * sb->block_size;
    failures = persistent ? 3 : 1; armed = 1;
    int rc = ext4_block_grow_commit(sb, ino, &inode, 4, data, 5 * sb->block_size);
    armed = 0;
    if (hits != failures || rc != (persistent ? -117 : -EIO)) return 11;
    if (persistent) {
        vnode_t *created = NULL;
        if (!ext4_is_inconsistent(sb) ||
            ext4_vn_create(g_root, "must-not-exist", 0600, &created) != -117)
            return 12;
        host_report("ORPHAN_QUARANTINE_PASS\n");
    } else {
        if (ext4_is_inconsistent(sb) || memcmp(&inode, &original, sizeof(inode)) ||
            bcache_read_bytes(g_bc, leaf * sb->block_size, after, sb->block_size) < 0 ||
            memcmp(before, after, sb->block_size) ||
            ext4_free_block(sb, data) < 0 || ext4_vn_statfs(g_root, &restored) < 0 ||
            baseline.f_bfree != restored.f_bfree) return 13;

        /* Fail the inode write after the existing mapping block was changed. */
        data = ext4_alloc_block(sb);
        fault_offset = inode_position(sb, ino) + offsetof(ext4_inode_t, i_size_lo);
        enlarged_inode = 1; enlarged_size = 5 * sb->block_size;
        hits = 0; armed = 1;
        rc = ext4_block_grow_commit(sb, ino, &inode, 4, data, enlarged_size);
        armed = 0; enlarged_inode = 0;
        ext4_inode_t disk;
        if (rc != -EIO || hits != 1 || ext4_is_inconsistent(sb) ||
            ext4_read_inode(sb, ino, &disk) < 0 ||
            memcmp(&disk.i_block, &original.i_block, sizeof(disk.i_block)) ||
            ext4_inode_size(&disk) != ext4_inode_size(&original) ||
            bcache_read_bytes(g_bc, leaf * sb->block_size, after, sb->block_size) < 0 ||
            memcmp(before, after, sb->block_size) || ext4_free_block(sb, data) < 0)
            return 14;
        /* Drop only fixture preallocations before the end-to-end link case. */
        if (ext4_block_truncate_at_checked(sb, ino, &inode, 4) < 0) return 15;

        char name[256];
        for (unsigned i = 0; i < 15; i++) {
            long_name(name, i);
            if (ext4_vn_link(g_root, name, file) < 0) return 16;
        }
        ext4_inode_t old_parent, new_parent, old_file, new_file;
        if (ext4_read_inode(sb, root->inode_num, &old_parent) < 0 ||
            ext4_inode_size(&old_parent) != sb->block_size ||
            ext4_read_inode(sb, ino, &old_file) < 0 ||
            ext4_vn_statfs(g_root, &baseline) < 0) return 17;
        char growth_name[256];
        memset(growth_name, 'g', 255); growth_name[255] = 0;
        fault_offset = inode_position(sb, root->inode_num) + offsetof(ext4_inode_t, i_size_lo);
        enlarged_inode = 1; enlarged_size = 2 * sb->block_size;
        hits = 0; armed = 1;
        rc = ext4_vn_link(g_root, growth_name, file);
        armed = 0; enlarged_inode = 0;
        vnode_t *missing = NULL;
        if (rc != -EIO || hits != 1 || ext4_is_inconsistent(sb) ||
            ext4_read_inode(sb, root->inode_num, &new_parent) < 0 ||
            ext4_inode_size(&new_parent) != ext4_inode_size(&old_parent) ||
            memcmp(&old_parent.i_block, &new_parent.i_block, sizeof(old_parent.i_block)) ||
            root->file_size != sb->block_size ||
            ext4_read_inode(sb, ino, &new_file) < 0 ||
            old_file.i_links_count != new_file.i_links_count ||
            ext4_lookup(g_root, growth_name, &missing) != -ENOENT ||
            ext4_vn_statfs(g_root, &restored) < 0 || baseline.f_bfree != restored.f_bfree)
            return 18;
        if (ext4_vn_link(g_root, growth_name, file) < 0 ||
            root->file_size != 2 * sb->block_size ||
            ext4_vn_unlink(g_root, growth_name) < 0) return 19;
        for (unsigned i = 0; i < 15; i++) {
            long_name(name, i);
            if (ext4_vn_unlink(g_root, name) < 0) return 20;
        }
        uint8_t page[4096];
        if (ext4_vn_readpage(file, 0, page, sizeof(page)) < 0) return 21;
        for (unsigned i = 0; i < sizeof(page); i++)
            if (page[i] != (uint8_t)((i * 37 + 19) % 251)) return 22;
        vnode_put(file);
        if (ext4_unmount(g_root) < 0) return 23;
        g_root = NULL;
        host_report("ORPHAN_RETRY_PASS\n");
    }
    if (bcache_sync_checked(g_bc) < 0 || fsync(g_fd)) return 24;
    close(g_fd);
    return persistent ? 117 : 0;
}
