/* Production ext4 + Rust cache: count errors must not publish/remove names. */
#define main ext4_other_scenarios_main
#define pwrite directory_fault_pwrite
#include "../../../kernel/fs/diskfs/tests/ext4_partial_trunc_host.c"
#undef pwrite
#undef main
extern long pwrite(int, const void *, unsigned long, long);

static unsigned armed, hits, target_count;
static uint64_t group_count_off;
long directory_fault_pwrite(int fd, const void *buffer, unsigned long count,
                            long off)
{
    const uint8_t *bytes = buffer;
    if (armed && !hits) {
        int fail = 0;
        if (armed == 1 && off <= (long)group_count_off &&
            (uint64_t)off + count >= group_count_off + 2) {
            size_t at = (size_t)(group_count_off - (uint64_t)off);
            unsigned value = bytes[at] | ((unsigned)bytes[at + 1] << 8);
            fail = value == target_count;
        } else if (armed == 2 && count >= 24) {
            fail = bytes[8] == '.' && bytes[20] == '.' && bytes[21] == '.';
        }
        if (fail) { hits++; return -1; }
    }
    return pwrite(fd, buffer, count, off);
}
/* Match the production Native device bridge's EIO normalization. */
static int directory_device_write(struct block_dev *dev, uint64_t lba,
                                  const void *buffer, size_t count)
{
    (void)dev;
    return fsio_write((uint32_t)lba, buffer, (uint32_t)count) ? -EIO : 0;
}
static int absent(const char *name)
{
    vnode_t *vn = NULL;
    int rc = ext4_lookup(g_root, name, &vn);
    if (vn) vnode_put(vn);
    return rc == -ENOENT;
}
static int same_free(const kstatfs_t *expected)
{
    kstatfs_t got;
    return ext4_vn_statfs(g_root, &got) == 0 &&
           got.f_bfree == expected->f_bfree && got.f_ffree == expected->f_ffree;
}
int main(int argc, char **argv)
{
    fscompat_log_sink = host_log_sink;
    if (argc != 2 || mount_image(argv[1]) < 0) return 1;
    g_bc->dev->write_sector = directory_device_write;
    ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)g_root->fs_data)->sb;
    group_count_off = sb->block_group_desc_table_byte +
                      offsetof(ext4_group_desc_t, bg_used_dirs_count_lo);
    kstatfs_t original, created;
    if (ext4_vn_statfs(g_root, &original) != 0) return 2;
    target_count = sb->group_descs[0].bg_used_dirs_count_lo + 1;
    armed = 1; hits = 0;
    int rc = ext4_vn_mkdir(g_root, "count-failure", 0700);
    if (rc != -EIO || hits != 1 || !absent("count-failure") || !same_free(&original)) {
        printf("DIR_COUNT: mkdir rc=%d hits=%u\n", rc, hits); return 3;
    }
    armed = 2; hits = 0;
    rc = ext4_vn_mkdir(g_root, "block-failure", 0700);
    if (rc != -EIO || hits != 1 || !absent("block-failure") || !same_free(&original)) {
        printf("DIR_COUNT: data rc=%d hits=%u\n", rc, hits); return 4;
    }
    armed = 0;
    if (ext4_vn_mkdir(g_root, "removal-failure", 0700) != 0 ||
        ext4_vn_statfs(g_root, &created) != 0) return 5;
    target_count = sb->group_descs[0].bg_used_dirs_count_lo - 1;
    armed = 1; hits = 0;
    rc = ext4_vn_rmdir(g_root, "removal-failure");
    if (rc != -EIO || hits != 1 || absent("removal-failure") || !same_free(&created)) {
        printf("DIR_COUNT: rmdir rc=%d hits=%u\n", rc, hits); return 6;
    }
    armed = 0;
    if (ext4_vn_rmdir(g_root, "removal-failure") != 0 || !same_free(&original)) return 7;
    if (ext4_unmount(g_root) != 0 || fsync(g_fd) != 0) return 8;
    close(g_fd);
    host_report("DIR_ACCOUNT_PASS\n");
    return 0;
}
