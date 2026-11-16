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
static int inode_release_retry(void)
{
    vnode_t *held = NULL;
    if (ext4_vn_mkdir(g_root, "inode-release", 0700) < 0 ||
        ext4_lookup(g_root, "inode-release", &held) < 0 || !held) return -1;
    ext4_vnode_priv_t *priv = held->fs_data;
    ext4_sb_info_t *sb = priv->sb;
    uint32_t group = (priv->inode_num - 1) / sb->inodes_per_group;
    uint32_t bit = (priv->inode_num - 1) % sb->inodes_per_group;
    bitmap_byte = (uint64_t)sb->group_descs[group].bg_inode_bitmap_lo * sb->block_size + bit / 8;
    bitmap_mask = 1U << (bit % 8);
    if (ext4_vn_rmdir(g_root, "inode-release") < 0) return -2;
    armed = 1; hits = 0;
    int ret = ext4_vnode_finalize_unlinked(held);
    armed = 0;
    ext4_inode_t inode;
    if (ret != -EIO || hits != 1 || !priv->unlinked ||
        ext4_read_inode(sb, priv->inode_num, &inode) < 0 ||
        (inode.i_mode & S_IFMT) != S_IFDIR || !(inode.i_flags & EXT4_EXTENTS_FL))
        return -3;
    if (ext4_vnode_finalize_unlinked(held) < 0 || priv->unlinked) return -4;
    vnode_put(held);
    host_report("ORPHAN_INODE_RELEASE_RETRY_PASS\n");
    return 0;
}
static int owner_order_and_reuse(void)
{
    vnode_t *tail = NULL, *head = NULL;
    if (ext4_vn_create(g_root, "owner-tail", 0600, &tail) < 0 ||
        ext4_vn_create(g_root, "owner-head", 0600, &head) < 0 ||
        ext4_vn_unlink(g_root, "owner-tail") < 0 ||
        ext4_vn_unlink(g_root, "owner-head") < 0) return -1;
    /* Reaping the tail changes the head inode's i_dtime. Its owner persists. */
    if (ext4_vnode_finalize_unlinked(tail) < 0 ||
        ext4_vnode_finalize_unlinked(head) < 0) return -2;
    ext4_vnode_priv_t *priv = head->fs_data;
    ext4_sb_info_t *sb = priv->sb;
    uint32_t chain = 1;
    if (bcache_read_bytes(g_bc, EXT4_SB_OFFSET +
        offsetof(ext4_superblock_t, s_last_orphan), &chain, sizeof(chain)) < 0 ||
        chain != 0 || sb->orphan_vnodes) return -3;
    vnode_put(tail); vnode_put(head);

    vnode_t *old = NULL, *replacement = NULL;
    if (ext4_vn_create(g_root, "owner-old", 0600, &old) < 0 ||
        ext4_vn_unlink(g_root, "owner-old") < 0) return -4;
    ext4_vnode_priv_t *old_priv = old->fs_data;
    uint32_t reused = old_priv->inode_num;
    rw_mutex_write_lock(&sb->metadata_lock);
    int ret = ext4_orphan_recover(sb);
    rw_mutex_write_unlock(&sb->metadata_lock);
    if (ret < 0 || old_priv->unlinked || sb->orphan_vnodes) return -5;
    if (ext4_vn_create(g_root, "owner-new", 0600, &replacement) < 0) return -6;
    ext4_vnode_priv_t *new_priv = replacement->fs_data;
    if (new_priv->inode_num != reused ||
        ext4_vn_unlink(g_root, "owner-new") < 0 ||
        ext4_vnode_finalize_unlinked(old) < 0) return -7;
    ext4_inode_t inode;
    if (!new_priv->unlinked || ext4_read_inode(sb, reused, &inode) < 0 ||
        !inode.i_mode || inode.i_links_count != 0) return -8;
    vnode_put(old);
    if (ext4_vnode_finalize_unlinked(replacement) < 0) return -9;
    vnode_put(replacement);
    if (sb->orphan_vnodes) return -10;
    host_report("ORPHAN_OWNER_ORDER_REUSE_PASS\n");
    return 0;
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
    if (argc != 2) return 5;
    int release_ret = inode_release_retry();
    if (release_ret < 0) {
        printf("ORPHAN_INODE_RELEASE_FAILED rc=%d\n", release_ret);
        return 16;
    }
    hits = 0;
    int owner_ret = owner_order_and_reuse();
    if (owner_ret < 0) {
        printf("ORPHAN_OWNER_FAILED rc=%d\n", owner_ret);
        return 15;
    }
    if (ext4_vn_mkdir(g_root, "retry", 0700) < 0) return 5;
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
