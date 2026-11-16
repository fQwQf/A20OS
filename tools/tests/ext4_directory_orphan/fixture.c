/* Held rmdir: checked cleanup retry, then fresh-process orphan recovery. */
#define main ext4_other_scenarios_main
#define pwrite directory_orphan_pwrite
#include "../../../kernel/fs/diskfs/tests/ext4_partial_trunc_host.c"
#undef pwrite
#undef main
extern long pwrite(int, const void *, unsigned long, long);

static uint64_t bitmap_byte;
static unsigned bitmap_mask, armed, hits;
long directory_orphan_pwrite(int fd, const void *buffer, unsigned long count,
                             long off)
{
    if (armed && !hits && off >= 0 && (uint64_t)off <= bitmap_byte &&
        (uint64_t)off + count > bitmap_byte &&
        !(((const uint8_t *)buffer)[bitmap_byte - (uint64_t)off] & bitmap_mask)) {
        hits++;
        return -1;
    }
    return pwrite(fd, buffer, count, off);
}
static int normalized_write(struct block_dev *dev, uint64_t lba,
                             const void *buffer, size_t count)
{
    (void)dev;
    return fsio_write((uint32_t)lba, buffer, (uint32_t)count) ? -EIO : 0;
}
static uint64_t number(const char *s)
{
    uint64_t n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + (unsigned)(*s++ - '0');
    return n;
}
static int absent(const char *name)
{
    vnode_t *vn = NULL;
    int ret = ext4_lookup(g_root, name, &vn);
    if (vn) vnode_put(vn);
    return ret == -ENOENT;
}
int main(int argc, char **argv)
{
    fscompat_log_sink = host_log_sink;
    if (argc < 2 || mount_image(argv[1]) < 0) return 1;
    g_bc->dev->write_sector = normalized_write;
    kstatfs_t baseline, after;
    if (ext4_vn_statfs(g_root, &baseline) < 0) return 2;
    if (argc == 5) {
        if (!absent("held-crash") || baseline.f_bfree != number(argv[3]) ||
            baseline.f_ffree != number(argv[4]) || ext4_unmount(g_root) < 0)
            return 3;
        if (fsync(g_fd) != 0) return 4;
        close(g_fd);
        host_report("DIR_ORPHAN_RECOVERY_PASS\n");
        return 0;
    }
    if (argc != 2 || ext4_vn_mkdir(g_root, "retry", 0700) < 0) return 5;
    vnode_t *held = NULL;
    if (ext4_lookup(g_root, "retry", &held) < 0 || !held) return 6;
    ext4_vnode_priv_t *priv = held->fs_data;
    ext4_sb_info_t *sb = priv->sb;
    ext4_inode_t inode;
    if (ext4_read_inode(sb, priv->inode_num, &inode) < 0) return 7;
    uint64_t block = ext4_block_map(sb, &inode, 0);
    uint32_t group = (uint32_t)((block - sb->first_data_block) / sb->blocks_per_group);
    uint32_t bit = (uint32_t)((block - sb->first_data_block) % sb->blocks_per_group);
    bitmap_byte = (uint64_t)sb->group_descs[group].bg_block_bitmap_lo * sb->block_size + bit / 8;
    bitmap_mask = 1U << (bit % 8);
    if (ext4_vn_rmdir(g_root, "retry") < 0 || !priv->unlinked || !absent("retry")) return 8;
    /* Removed directories cannot acquire unreachable children. */
    if (ext4_vn_mkdir(held, "child", 0700) != -ENOENT) return 9;
    armed = 1;
    if (ext4_vnode_finalize_unlinked(held) != -EIO || hits != 1 || !priv->unlinked)
        return 10;
    armed = 0;
    if (ext4_vnode_finalize_unlinked(held) < 0 || priv->unlinked) return 11;
    vnode_put(held);
    if (ext4_vn_statfs(g_root, &after) < 0 || after.f_bfree != baseline.f_bfree ||
        after.f_ffree != baseline.f_ffree) return 12;

    if (ext4_vn_mkdir(g_root, "held-crash", 0700) < 0 ||
        ext4_lookup(g_root, "held-crash", &held) < 0 || !held ||
        ext4_vn_rmdir(g_root, "held-crash") < 0) return 13;
    priv = held->fs_data;
    if (!priv->unlinked || ext4_read_inode(sb, priv->inode_num, &inode) < 0 ||
        inode.i_links_count != 0 || bcache_sync_checked(g_bc) < 0) return 14;
    printf("DIR_ORPHAN_PENDING ino=%u blocks=%llu inodes=%llu\n", priv->inode_num,
           (unsigned long long)baseline.f_bfree, (unsigned long long)baseline.f_ffree);
    /* Abrupt backend exit: no vnode release, filesystem unmount or clean bit. */
    close(g_fd);
    return 0;
}
