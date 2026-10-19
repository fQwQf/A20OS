/*
 * Host differential harness for the VFS mount-table orchestration engine
 * (VFS_MOUNT_STATE_IMPL).
 *
 * Drives a C transcription of the pre-migration umount(2) matching/busy logic
 * and move_mount(2) validation order and the Rust engine
 * (kernel/fs/vfs/mount_state.rs, linked as a host staticlib) over the same
 * deterministic inputs, comparing the result and the plan.
 *
 * Exit 0 on success; on mismatch prints the input and both results, exits 1.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXP 512
#define ENOENT 2
#define EBUSY 16
#define EINVAL 22

#define VFS_UMOUNT_FORCE 0x1
#define VFS_UMOUNT_DETACH 0x2
#define VFS_UMOUNT_EXPIRE 0x4

typedef struct {
    const char *path;
    uint32_t root_users;
    uint32_t has_child;
    uint32_t cwd_users;
    uint32_t is_ns_root;
} vfs_mount_cand_t;

typedef struct {
    int index;
    uint32_t detach_children;
    uint32_t invalidate_all;
} vfs_umount_plan_t;

extern int a20_vfs_umount_plan(const char *query, int flags, int check_busy,
                               const vfs_mount_cand_t *cands, size_t n,
                               vfs_umount_plan_t *out);
extern int a20_vfs_move_mount_check(const char *from, const char *to, int found,
                                    const char *from_path, int from_detached,
                                    int target_covered, int *out_noop);

static void norm_copy(char *dst, size_t cap, const char *src)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
    size_t len = strlen(dst);
    while (len > 1 && dst[len - 1] == '/') {
        dst[len - 1] = '\0';
        len--;
    }
}

/* ---- C reference: pre-migration vfs_umount_flags / vfs_umount ---- */
static int ref_umount_plan(const char *query, int flags, int check_busy,
                           const vfs_mount_cand_t *c, int n, vfs_umount_plan_t *out)
{
    out->index = -1;
    out->detach_children = 0;
    out->invalidate_all = 0;
    if (!query)
        return -EINVAL;
    char norm[MAXP];
    norm_copy(norm, sizeof(norm), query);
    for (int i = 0; i < n; i++) {
        if (!c[i].path)
            continue;
        char mn[MAXP];
        norm_copy(mn, sizeof(mn), c[i].path);
        if (strcmp(mn, norm) != 0)
            continue;
        if (check_busy && !(flags & VFS_UMOUNT_DETACH)) {
            if (c[i].root_users > 1)
                return -EBUSY;
            if (c[i].has_child)
                return -EBUSY;
            if (c[i].cwd_users > 0)
                return -EBUSY;
            if (c[i].is_ns_root)
                return -EINVAL;
        }
        out->index = i;
        out->detach_children = c[i].has_child;
        out->invalidate_all = 1;
        return 0;
    }
    return -EINVAL;
}

/* ---- C reference: pre-migration vfs_move_mount checks ---- */
static int ref_move_check(const char *from, const char *to, int found,
                          const char *from_path, int detached, int covered,
                          int *out_noop)
{
    *out_noop = 0;
    if (!from || !to || from[0] != '/' || to[0] != '/')
        return -EINVAL;
    if (!found)
        return -ENOENT;
    if (detached)
        return -EINVAL;
    if (covered)
        return -EBUSY;
    if (strcmp(from_path, to) == 0) {
        *out_noop = 1;
        return 0;
    }
    if (strncmp(to, from_path, strlen(from_path)) == 0 &&
        (to[strlen(from_path)] == '\0' || to[strlen(from_path)] == '/'))
        return -EINVAL;
    return 0;
}

static uint64_t rng_state;
static uint64_t rnd(void)
{
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return rng_state >> 17;
}

static const char *g_paths[] = {
    "/", "/m", "/m/", "/data", "/data/sub", "/data/sub/", "/da", "/datax",
    "/a", "/a/b", "/a/b/c", "rel", "", "//m", "/m//", "/nope", "/a/b/",
};
#define NPATHS (sizeof(g_paths) / sizeof(g_paths[0]))

int main(int argc, char **argv)
{
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 200000;
    rng_state = 0x3f84d5b5b5470917ull;

    for (unsigned step = 0; step < rounds; step++) {
        /* ---- umount plan ---- */
        const char *query = g_paths[rnd() % NPATHS];
        int flags = (int)(rnd() % 8);
        int check_busy = (int)(rnd() % 2);
        int n = (int)(rnd() % 6);
        vfs_mount_cand_t cands[6];
        for (int i = 0; i < n; i++) {
            cands[i].path = g_paths[rnd() % NPATHS];
            cands[i].root_users = (uint32_t)(rnd() % 3);
            cands[i].has_child = (uint32_t)(rnd() % 2);
            cands[i].cwd_users = (uint32_t)(rnd() % 2);
            cands[i].is_ns_root = (uint32_t)(rnd() % 2);
        }
        vfs_umount_plan_t a, b;
        memset(&a, 0xaa, sizeof(a));
        memset(&b, 0xaa, sizeof(b));
        int ra = ref_umount_plan(query, flags, check_busy, cands, n, &a);
        int rb = a20_vfs_umount_plan(query, flags, check_busy, cands, (size_t)n, &b);
        if (ra != rb ||
            (ra == 0 && (a.index != b.index ||
                         a.detach_children != b.detach_children ||
                         a.invalidate_all != b.invalidate_all))) {
            printf("umount mismatch step %u query='%s' flags=%d busy=%d n=%d\n"
                   "  C:    rc=%d idx=%d det=%u inv=%u\n"
                   "  RUST: rc=%d idx=%d det=%u inv=%u\n",
                   step, query, flags, check_busy, n,
                   ra, a.index, a.detach_children, a.invalidate_all,
                   rb, b.index, b.detach_children, b.invalidate_all);
            return 1;
        }

        /* ---- move_mount validation ---- */
        const char *from = g_paths[rnd() % NPATHS];
        const char *to = g_paths[rnd() % NPATHS];
        int found = (int)(rnd() % 2);
        const char *from_path = g_paths[rnd() % NPATHS];
        int detached = (int)(rnd() % 2);
        int covered = (int)(rnd() % 2);
        int na = 0, nb = 0;
        ra = ref_move_check(from, to, found, from_path, detached, covered, &na);
        rb = a20_vfs_move_mount_check(from, to, found, from_path, detached, covered, &nb);
        if (ra != rb || na != nb) {
            printf("move mismatch step %u from='%s' to='%s' found=%d from_path='%s' "
                   "det=%d cov=%d\n  C:    rc=%d noop=%d\n  RUST: rc=%d noop=%d\n",
                   step, from, to, found, from_path, detached, covered,
                   ra, na, rb, nb);
            return 1;
        }

        /* Null-argument edges. */
        if (step % 64 == 0) {
            vfs_umount_plan_t p;
            if (a20_vfs_umount_plan(NULL, 0, 1, cands, (size_t)n, &p) != -EINVAL) {
                printf("umount null-query mismatch step %u\n", step);
                return 1;
            }
            int no = 0;
            if (a20_vfs_move_mount_check(NULL, to, found, from_path, detached, covered, &no)
                != -EINVAL) {
                printf("move null-from mismatch step %u\n", step);
                return 1;
            }
        }
    }

    printf("vfs mount state C/Rust differential PASS (%u steps)\n", rounds);
    return 0;
}
