/* Real journal mount failures must retire their cache callback before free. */
#define main ext4_other_scenarios_main
#define a20_malloc mount_base_malloc
#define a20_free mount_base_free
#include "../../../kernel/fs/diskfs/tests/ext4_partial_trunc_host.c"
#undef a20_free
#undef a20_malloc
#undef main

static int fault_kind, fault_hits, owner_frees, dangling_owner;
static ext4_sb_info_t *observed_owner;

void *a20_malloc(uint64_t size)
{
    if (fault_kind && !fault_hits && g_bc && g_bc->sync_hook) {
        ext4_sb_info_t *sb = g_bc->owner;
        uint64_t target = fault_kind == 2 ? sizeof(vnode_t) :
                          (uint64_t)sb->groups_count * sizeof(uint32_t);
        if (size == target) {
            observed_owner = sb;
            fault_hits++;
            if (fault_kind == 3) ext4_mark_inconsistent(sb);
            return NULL;
        }
    }
    return mount_base_malloc(size);
}

void a20_free(void *ptr)
{
    if (ptr && ptr == observed_owner) {
        owner_frees++;
        if (g_bc->owner == ptr || g_bc->sync_hook) dangling_owner++;
        observed_owner = NULL;
    }
    mount_base_free(ptr);
}

int main(int argc, char **argv)
{
    fscompat_log_sink = host_log_sink;
    if (argc != 2) return 1;
    /* Both allocations happen after journal open and hook registration. */
    for (int kind = 1; kind <= 2; kind++) {
        fault_kind = kind;
        fault_hits = owner_frees = dangling_owner = 0;
        int failed = kind == 1 ? mount_image(argv[1]) :
                     ((g_root = ext4_mount(g_bc)) ? 0 : -1);
        if (!failed || fault_hits != 1 || owner_frees != 1 ||
            dangling_owner || g_bc->owner || g_bc->sync_hook) {
            printf("MOUNT_ABORT: kind=%d hits=%d frees=%d dangling=%d\n",
                   kind, fault_hits, owner_frees, dangling_owner);
            return 2;
        }
        fault_kind = 0;
        if (bcache_sync_checked(g_bc) < 0) return 3;
        g_root = ext4_mount(g_bc);
        if (!g_root || ext4_unmount(g_root) < 0) return 4;
        g_root = NULL;
    }
    /* A failed close still owns the callback and must keep its owner alive. */
    fault_kind = 3;
    fault_hits = owner_frees = dangling_owner = 0;
    g_root = ext4_mount(g_bc);
    if (g_root || fault_hits != 1 || owner_frees || dangling_owner ||
        !observed_owner || g_bc->owner != observed_owner || !g_bc->sync_hook ||
        !ext4_is_inconsistent(observed_owner) ||
        bcache_sync_checked(g_bc) != -117) return 5;
    /* The retained failed transaction/cache are deliberately not destroyed. */
    if (fsync(g_fd) != 0) return 6;
    close(g_fd);
    host_report("MOUNT_ABORT_PASS\n");
    return 0;
}
