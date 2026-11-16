/* Production ext4/cache: failed link publication restores namespace/count. */
#define main ext4_other_scenarios_main
#define pwrite link_fault_pwrite
#include "../../../kernel/fs/diskfs/tests/ext4_partial_trunc_host.c"
#undef pwrite
#undef main
extern long pwrite(int, const void *, unsigned long, long);

static uint64_t inode_offset, directory_offset;
static unsigned armed, at_inode, at_growth, hits, rollback_hits, persistent;
static char growth_name[256];
static int overlaps(long off, unsigned long size, uint64_t byte)
{
    return off <= (long)byte && (uint64_t)off + size > byte;
}
long link_fault_pwrite(int fd, const void *buffer, unsigned long size, long off)
{
    if (armed) {
        uint64_t target = at_inode ? inode_offset : directory_offset;
        int match = at_growth ?
            (size >= 8 + 255 && !memcmp((const char *)buffer + 8, growth_name, 255)) :
            overlaps(off, size, target);
        if (!hits && match) {
            hits++;
            return -1;
        }
        /* The count increment passed; fail both later count restoration
         * attempts after the directory block itself was restored. */
        if (persistent && hits && rollback_hits < 2 &&
            overlaps(off, size, inode_offset)) {
            rollback_hits++;
            return -1;
        }
    }
    return pwrite(fd, buffer, size, off);
}
static int link_device_write(struct block_dev *dev, uint64_t lba,
                              const void *buffer, size_t count)
{
    (void)dev;
    return fsio_write((uint32_t)lba, buffer, (uint32_t)count) ? -EIO : 0;
}
static int unchanged(ext4_sb_info_t *sb, uint32_t ino, ext4_inode_t *before)
{
    ext4_inode_t after;
    vnode_t *alias = NULL;
    return !ext4_is_inconsistent(sb) &&
           ext4_read_inode(sb, ino, &after) == 0 &&
           after.i_links_count == before->i_links_count &&
           after.i_ctime == before->i_ctime &&
           ext4_lookup(g_root, "alias", &alias) == -ENOENT;
}
static void fill_name(char *name, unsigned i)
{
    memset(name, 'x', 255);
    name[0] = 'f'; name[1] = 'a' + i;
    name[255] = 0;
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
        persistent = 1;
    }
    if (mount_image(argv[1]) < 0) return 2;
    g_bc->dev->write_sector = link_device_write;
    ext4_vnode_priv_t *root = g_root->fs_data;
    ext4_sb_info_t *sb = root->sb;
    ext4_inode_t directory, before;
    vnode_t *file = NULL;
    if (ext4_read_inode(sb, root->inode_num, &directory) < 0 ||
        ext4_lookup(g_root, "single", &file) < 0) return 3;
    uint32_t ino = ((ext4_vnode_priv_t *)file->fs_data)->inode_num;
    if (ext4_read_inode(sb, ino, &before) < 0) return 4;
    uint32_t group = (ino - 1) / sb->inodes_per_group;
    uint32_t index = (ino - 1) % sb->inodes_per_group;
    inode_offset = (uint64_t)sb->group_descs[group].bg_inode_table_lo *
                   sb->block_size + (uint64_t)index * sb->inode_size;
    directory_offset = ext4_block_map(sb, &directory, 0) * sb->block_size;
    if (!persistent) {
        at_inode = 1; armed = 1;
        int rc = ext4_vn_link(g_root, "alias", file);
        armed = 0;
        if (rc != -EIO || hits != 1 || !unchanged(sb, ino, &before)) return 5;
    }
    at_inode = 0; hits = 0; armed = 1;
    int rc = ext4_vn_link(g_root, "alias", file);
    armed = 0;
    if (rc != (persistent ? -117 : -EIO) || hits != 1) return 6;
    if (persistent) {
        vnode_t *created = NULL;
        if (rollback_hits != 2 || !ext4_is_inconsistent(sb) ||
            ext4_vn_create(g_root, "must-not-exist", 0600, &created) != -117)
            return 7;
        host_report("ORPHAN_QUARANTINE_PASS\n");
    } else {
        if (!unchanged(sb, ino, &before)) return 8;
        /* Fifteen long aliases fill the first 4 KiB directory block. Fail
         * initialization of the next block while it is still unpublished. */
        char name[256];
        for (unsigned i = 0; i < 15; i++) {
            fill_name(name, i);
            if (ext4_vn_link(g_root, name, file) < 0) return 17;
        }
        ext4_inode_t old_dir, new_dir, old_file, new_file;
        if (ext4_read_inode(sb, root->inode_num, &old_dir) < 0 ||
            ext4_read_inode(sb, ino, &old_file) < 0 ||
            ext4_inode_size(&old_dir) != sb->block_size) return 18;
        kstatfs_t free_before, free_after;
        if (ext4_vn_statfs(g_root, &free_before) < 0) return 22;
        memset(growth_name, 'g', 255); growth_name[255] = 0;
        at_growth = 1; hits = 0; armed = 1;
        rc = ext4_vn_link(g_root, growth_name, file);
        armed = 0; at_growth = 0;
        vnode_t *missing = NULL;
        if (rc != -EIO || hits != 1 || ext4_is_inconsistent(sb) ||
            ext4_vn_statfs(g_root, &free_after) < 0 ||
            free_after.f_bfree != free_before.f_bfree ||
            ext4_read_inode(sb, root->inode_num, &new_dir) < 0 ||
            ext4_read_inode(sb, ino, &new_file) < 0 ||
            memcmp(&old_dir.i_block, &new_dir.i_block, sizeof(old_dir.i_block)) ||
            ext4_inode_size(&old_dir) != ext4_inode_size(&new_dir) ||
            old_file.i_links_count != new_file.i_links_count ||
            ext4_lookup(g_root, growth_name, &missing) != -ENOENT) return 19;
        if (ext4_vn_link(g_root, growth_name, file) < 0 ||
            ext4_vn_unlink(g_root, growth_name) < 0) return 20;
        for (unsigned i = 0; i < 15; i++) {
            fill_name(name, i);
            if (ext4_vn_unlink(g_root, name) < 0) return 21;
        }
        if (ext4_vn_link(g_root, "alias", file) < 0) return 10;
        vnode_t *alias = NULL;
        ext4_inode_t after;
        if (ext4_lookup(g_root, "alias", &alias) < 0 || alias != file ||
            ext4_read_inode(sb, ino, &after) < 0 || after.i_links_count != 2)
            return 11;
        vnode_put(alias);
        uint8_t page[4096];
        if (ext4_vn_readpage(file, 0, page, sizeof(page)) < 0) return 12;
        for (unsigned i = 0; i < sizeof(page); i++)
            if (page[i] != (uint8_t)((i * 37 + 19) % 251)) return 13;
        if (ext4_vn_unlink(g_root, "alias") < 0 ||
            ext4_vn_unlink(g_root, "single") < 0) return 14;
        vnode_put(file);
        if (ext4_unmount(g_root) < 0) return 15;
        g_root = NULL;
        host_report("ORPHAN_RETRY_PASS\n");
    }
    if (bcache_sync_checked(g_bc) < 0 || fsync(g_fd)) return 16;
    close(g_fd);
    return persistent ? 117 : 0;
}
