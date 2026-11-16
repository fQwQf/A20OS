/* Run the production Native ext4/orphan/cache backend on a real host image. */
#define main ext4_other_scenarios_main
#define pwrite orphan_fault_pwrite
#include "../../../kernel/fs/diskfs/tests/ext4_partial_trunc_host.c"
#undef pwrite
#undef main
#include "fs/file.h"
extern long pwrite(int, const void *, unsigned long, long);

static unsigned fault_active, fault_write, fault_hits;
static unsigned first_failure = 5, failure_count = 2;
long orphan_fault_pwrite(int fd, const void *buffer, unsigned long size, long off)
{
    if (fault_active && ++fault_write >= first_failure &&
        fault_write < first_failure + failure_count) {
        fault_hits++;
        printf("ORPHAN_FAULT: write=%u off=%lu\n", fault_write, (unsigned long)off);
        return -1;
    }
    return pwrite(fd, buffer, size, off);
}
int main(int argc, char **argv)
{
    fscompat_log_sink = host_log_sink;
    if (argc < 2 || argc > 3) return 1;
    if (argc == 3 && strcmp(argv[2], "verify-quarantine") == 0) {
        if (mount_image(argv[1]) >= 0) return 9;
        host_report("ORPHAN_COLD_QUARANTINE_PASS\n");
        return 0;
    }
    if (argc == 3) {
        if (strcmp(argv[2], "persistent") != 0) return 1;
        failure_count = 3; /* reject both bounded restoration attempts */
    }
    if (mount_image(argv[1]) < 0) return 1;
    vnode_t *vn = NULL;
    if (ext4_lookup(g_root, "single", &vn) < 0) return 2;
    vfile_t *vf = ext4_open_vnode(vn, 1);
    if (!vf || ext4_vn_unlink(g_root, "single") < 0 ||
        bcache_sync_checked(g_bc) < 0) return 3;
    if (ext4_fclose(vf) < 0) return 4;
    fault_active = 1;
    int rc = ext4_vnode_finalize_unlinked(vn);
    fault_active = 0;
    printf("ORPHAN_RESULT: rc=%d hits=%u writes=%u\n", rc, fault_hits, fault_write);
    host_report(rc == -117 ? "ORPHAN_FATAL\n" : rc < 0 ? "ORPHAN_RETRYABLE\n" : "ORPHAN_UNEXPECTED_SUCCESS\n");
    if (fault_hits != failure_count || rc >= 0) return 5;
    /* A bounded, successful compensation keeps the orphan reachable. Retry
     * the real operation, then require a cleanly unmounted disk for fsck. */
    if (rc == -117) {
        ext4_sb_info_t *sb = ((ext4_vnode_priv_t *)g_root->fs_data)->sb;
        vnode_t *created = NULL;
        if (!ext4_is_inconsistent(sb) ||
            ext4_vn_create(g_root, "must-not-exist", 0600, &created) != -117)
            return 10;
        host_report("ORPHAN_QUARANTINE_PASS\n");
    } else {
        if (ext4_vnode_finalize_unlinked(vn) != 0) return 6;
        vnode_put(vf->vnode);
        vfile_free(vf);
        vnode_put(vn);
        if (ext4_unmount(g_root) != 0) return 7;
        g_root = NULL;
        host_report("ORPHAN_RETRY_PASS\n");
    }
    if (bcache_sync_checked(g_bc) < 0 || fsync(g_fd) != 0) return 8;
    close(g_fd);
    return rc == -117 ? 117 : 0;
}
