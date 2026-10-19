/*
 * vfs_bench — end-to-end VFS frontend workload used for paired C/Rust timing.
 *
 * Phases (all on a fresh scratch tree under /tmp so the input tree is identical
 * on both images):
 *   create   O_CREAT|O_TRUNC + one 4 KiB write per file
 *   read     re-open and read the whole file back
 *   stat     stat() by path
 *   rename   rename back and forth between two names
 *   unlink   unlink the files
 *   mixed    two children: one renames a name back and forth, one stats it
 *
 * Each phase prints one machine-readable line:
 *   VFS_BENCH: phase=<name> ops=<n> ns_total=<t> ns_per_op=<avg> p50=<p50> p99=<p99> max=<max>
 *
 * The caller pairs lines from the C-selector and Rust-selector images built
 * from the same tree and runs them in alternating order.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *g_base = "/tmp/vfs_bench";
#define BASE g_base
#define NFILES 200
#define FILE_BYTES 4096
#define STAT_ROUNDS 4000
#define RENAME_ROUNDS 1000
#define FRONTEND_ROUNDS 128
#define MAX_SAMPLES 8192

static uint64_t g_samples[MAX_SAMPLES];
static int g_nsamples;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sample_reset(void) { g_nsamples = 0; }
static void sample(uint64_t d)
{
    if (g_nsamples < MAX_SAMPLES)
        g_samples[g_nsamples++] = d;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void report(const char *phase, unsigned ops, uint64_t total)
{
    qsort(g_samples, (size_t)g_nsamples, sizeof(g_samples[0]), cmp_u64);
    uint64_t p50 = 0, p99 = 0, mx = 0;
    if (g_nsamples > 0) {
        p50 = g_samples[g_nsamples / 2];
        p99 = g_samples[(g_nsamples * 99) / 100];
        mx = g_samples[g_nsamples - 1];
    }
    printf("VFS_BENCH: phase=%s ops=%u ns_total=%llu ns_per_op=%llu p50=%llu p99=%llu max=%llu\n",
           phase, ops, (unsigned long long)total,
           (unsigned long long)(ops ? total / ops : 0),
           (unsigned long long)p50, (unsigned long long)p99, (unsigned long long)mx);
    fflush(stdout);
}

static void path_of(int i, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s/f%04d", g_base, i);
}

static int phase_create(void)
{
    char buf[FILE_BYTES];
    memset(buf, 0x5a, sizeof(buf));
    char path[128];
    sample_reset();
    uint64_t t0 = now_ns();
    for (int i = 0; i < NFILES; i++) {
        uint64_t s = now_ns();
        path_of(i, path, sizeof(path));
        int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd < 0)
            return -1;
        if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
            close(fd);
            return -2;
        }
        close(fd);
        sample(now_ns() - s);
    }
    report("create", NFILES, now_ns() - t0);
    return 0;
}

static int phase_read(void)
{
    char buf[FILE_BYTES];
    char path[128];
    sample_reset();
    uint64_t t0 = now_ns();
    for (int i = 0; i < NFILES; i++) {
        uint64_t s = now_ns();
        path_of(i, path, sizeof(path));
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            return -1;
        ssize_t got = read(fd, buf, sizeof(buf));
        close(fd);
        if (got != (ssize_t)sizeof(buf))
            return -2;
        sample(now_ns() - s);
    }
    report("read", NFILES, now_ns() - t0);
    return 0;
}

static int phase_stat(void)
{
    char path[128];
    sample_reset();
    uint64_t t0 = now_ns();
    for (int r = 0; r < STAT_ROUNDS; r++) {
        int i = r % NFILES;
        uint64_t s = now_ns();
        path_of(i, path, sizeof(path));
        struct stat st;
        if (stat(path, &st) != 0)
            return -1;
        sample(now_ns() - s);
    }
    report("stat", STAT_ROUNDS, now_ns() - t0);
    return 0;
}

static int phase_rename(void)
{
    char a[128], b[128];
    snprintf(a, sizeof(a), "%s/rn_a", g_base);
    snprintf(b, sizeof(b), "%s/rn_b", g_base);
    sample_reset();
    uint64_t t0 = now_ns();
    for (int i = 0; i < RENAME_ROUNDS; i++) {
        uint64_t s = now_ns();
        if (rename(a, b) != 0)
            return -1;
        if (rename(b, a) != 0)
            return -2;
        sample(now_ns() - s);
    }
    /* One sample covers the rename-away + rename-back pair.  Keep all
     * reported fields at that unit so p50/p99 are not mislabeled per call. */
    report("rename_pair", RENAME_ROUNDS, now_ns() - t0);
    return 0;
}

static int phase_unlink(void)
{
    char path[128];
    sample_reset();
    uint64_t t0 = now_ns();
    for (int i = 0; i < NFILES; i++) {
        uint64_t s = now_ns();
        path_of(i, path, sizeof(path));
        if (unlink(path) != 0)
            return -1;
        sample(now_ns() - s);
    }
    report("unlink", NFILES, now_ns() - t0);
    return 0;
}

static int phase_mixed(void)
{
    char a[128], b[128];
    snprintf(a, sizeof(a), "%s/mx_a", g_base);
    snprintf(b, sizeof(b), "%s/mx_b", g_base);
    int fd = open(a, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    if (write(fd, "x", 1) != 1) {
        close(fd);
        return -2;
    }
    close(fd);

    pid_t renamer = fork();
    if (renamer < 0)
        return -3;
    if (renamer == 0) {
        for (int i = 0; i < 500; i++) {
            if (rename(a, b) != 0 && errno != ENOENT)
                _exit(1);
            if (rename(b, a) != 0 && errno != ENOENT)
                _exit(2);
        }
        _exit(0);
    }
    sample_reset();
    uint64_t t0 = now_ns();
    for (int i = 0; i < 2000; i++) {
        uint64_t s = now_ns();
        struct stat st;
        (void)stat(a, &st);
        sample(now_ns() - s);
    }
    uint64_t total = now_ns() - t0;
    int st = 0;
    if (waitpid(renamer, &st, 0) != renamer || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
        return -4;
    report("mixed", 2000, total);
    unlink(a);
    unlink(b);
    return 0;
}

/* Exercise the mount-tree constructor/removal transaction added to the Rust
 * frontend.  Each sample is one full mount+unmount lifecycle on a private
 * tmpfs mountpoint; backend construction remains the same C callback in both
 * images. */
static int phase_mount_lifecycle(void)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/mountpoint", g_base);
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;
    sample_reset();
    uint64_t t0 = now_ns();
    for (int i = 0; i < FRONTEND_ROUNDS; i++) {
        uint64_t s = now_ns();
        if (mount("none", path, "tmpfs", 0, NULL) != 0)
            return -2;
        if (umount2(path, 0) != 0)
            return -3;
        sample(now_ns() - s);
    }
    report("mount_umount", FRONTEND_ROUNDS, now_ns() - t0);
    if (rmdir(path) != 0)
        return -4;
    return 0;
}

/* Read generated procfs content to EOF, not merely the first page.  The maps
 * and net writers are Rust-owned in the final image and C-backed in the
 * 50f0d34 baseline image. */
static int phase_procfs_content(const char *path, const char *phase)
{
    char buf[2048];
    sample_reset();
    uint64_t t0 = now_ns();
    for (int i = 0; i < FRONTEND_ROUNDS; i++) {
        uint64_t s = now_ns();
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            return -1;
        size_t total = 0;
        for (;;) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n < 0) {
                close(fd);
                return -2;
            }
            if (n == 0)
                break;
            total += (size_t)n;
        }
        if (close(fd) != 0 || total == 0)
            return -3;
        sample(now_ns() - s);
    }
    report(phase, FRONTEND_ROUNDS, now_ns() - t0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        g_base = argv[1];
    if (mkdir(BASE, 0755) != 0 && errno != EEXIST)
        printf("VFS_BENCH: mkdir errno=%d\n", errno);

    if (phase_create() != 0) { printf("VFS_BENCH: FAIL create errno=%d\n", errno); return 1; }
    if (phase_read() != 0)   { printf("VFS_BENCH: FAIL read errno=%d\n", errno); return 1; }
    if (phase_stat() != 0)   { printf("VFS_BENCH: FAIL stat errno=%d\n", errno); return 1; }

    /* The rename phase needs a live name. */
    char a[128];
    snprintf(a, sizeof(a), "%s/rn_a", g_base);
    int fd = open(a, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) { printf("VFS_BENCH: FAIL rename-setup errno=%d\n", errno); return 1; }
    close(fd);
    if (phase_rename() != 0) { printf("VFS_BENCH: FAIL rename errno=%d\n", errno); return 1; }
    unlink(a);

    if (phase_mixed() != 0)  { printf("VFS_BENCH: FAIL mixed errno=%d\n", errno); return 1; }
    if (phase_unlink() != 0) { printf("VFS_BENCH: FAIL unlink errno=%d\n", errno); return 1; }
    if (phase_mount_lifecycle() != 0) { printf("VFS_BENCH: FAIL mount_umount errno=%d\n", errno); return 1; }
    if (phase_procfs_content("/proc/self/maps", "proc_maps") != 0) {
        printf("VFS_BENCH: FAIL proc_maps errno=%d\n", errno); return 1;
    }
    if (phase_procfs_content("/proc/self/smaps", "proc_smaps") != 0) {
        printf("VFS_BENCH: FAIL proc_smaps errno=%d\n", errno); return 1;
    }
    if (phase_procfs_content("/proc/net/tcp", "proc_net_tcp") != 0) {
        printf("VFS_BENCH: FAIL proc_net_tcp errno=%d\n", errno); return 1;
    }

    rmdir(BASE);
    printf("VFS_BENCH: DONE base=%s\n", g_base);
    return 0;
}
