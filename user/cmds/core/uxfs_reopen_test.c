/*
 * uxfs_reopen_test — same-inode vnode identity / visibility regression on a
 * UXFS mount (docs/zh/fs/fs-consistency-model.md).
 *
 * The r4 defect: a second fd opened before the first fd's write held an
 * independent vnode whose size stayed 0, so its read returned EOF even after
 * the first fd wrote and fsync'ed.  With the canonical (sb,ino) vnode
 * identity index every open of one inode shares one vnode and one page cache.
 *
 * Checks (dir = $UXFS_TEST_DIR, default /ufs):
 *   r4        open fd1, open fd2 (before any write), write+fsync via fd1:
 *             fd2 must see the new size and bytes (fresh opens too);
 *   symmetric fd1 written+fsynced, then fd2 opened, writes and fsyncs:
 *             fd1 must read the new bytes;
 *   truncate  fd1 truncates while fd2 is already open: fd2 must see the new
 *             size and EOF exactly at it;
 *   unlink+recreate: after unlink the path is ENOENT, fd2 keeps reading the
 *             old inode, and a recreated file is a fresh object to new opens;
 *   mmap      a MAP_SHARED write through fd1 is visible to fd2's read, and a
 *             MAP_SHARED mapping over fd2 sees fd1's write.  A refused
 *             mmap() is reported as SKIP, never as PASS;
 *   keep      deterministic 64 KiB file + parent-directory fsync for the
 *             cold-boot verify run ($UXFS_TEST_VERIFY).
 *
 * The mount is expected to come from the service manager; if it is not up the
 * test self-starts ufsd on $UXFS_TEST_BLK (default 1, dev scratch disk) the
 * same way ufs_all_test does. UXFS_TEST_FS selects the backend (default fat);
 * ext4 also releases the boot-time native owner of the auxiliary disk.
 */
#define _GNU_SOURCE
#include "../../liba20rt/a20_arch.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define UXFS_MAGIC  0x55584653u /* 'UXFS' from uxfs_statfs */

#define R4_BYTES    8192
#define SYM_BYTES   4096
#define TRUNC_OLD   16384
#define TRUNC_NEW   4097
#define RECYCLE_BYTES 4096
#define MMAP_BYTES  4096
#define KEEP_BYTES  (64 * 1024)

static int fail(const char *tag, const char *detail)
{
    printf("UXFS_REOPEN: FAIL %s: %s (errno=%d %s)\n", tag, detail, errno,
           strerror(errno));
    return 1;
}

static void fill_pattern(unsigned char *buf, size_t len, unsigned base)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (unsigned char)(i * 31 + base);
}

static int write_full(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0)
            return -1;
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/* Returns the byte count actually delivered; short counts mean EOF. */
static long read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        done += (size_t)n;
    }
    return (long)done;
}

static int is_live(const char *dir)
{
    struct statfs st;
    if (statfs(dir, &st) != 0)
        return 0;
    return (unsigned int)st.f_type == UXFS_MAGIC;
}

static int wait_live(const char *dir, int tries)
{
    for (int i = 0; i < tries; i++) {
        if (is_live(dir))
            return 0;
        usleep(20000);
    }
    return -1;
}

static int ensure_mount(const char *dir)
{
    /* A service manager may be bringing the mount up concurrently. */
    if (wait_live(dir, 50) == 0)
        return 0;
    const char *idx = getenv("UXFS_TEST_BLK");
    if (!idx || !idx[0])
        idx = "1";
    const char *fstype = getenv("UXFS_TEST_FS");
    if (!fstype || !fstype[0])
        fstype = "fat";
    if (strcmp(fstype, "ext4") == 0) {
        /* The boot init may own the auxiliary ext4 disk at /extra. Release
         * those native mounts before the UXFS server registers this device. */
        static const char *const paths[] = {
            "/extra/dev/shm", "/extra/dev", "/extra/proc", "/extra/sys",
            "/extra/run", "/extra"
        };
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
            if (umount2(paths[i], 0) != 0 && errno != EINVAL)
                return -1;
        }
    }
    /* Daemonize the service: fork an intermediate that exits immediately so
     * the serving process is re-parented away from this test.  The kernel
     * force-kills a dying task's direct children when it exits abnormally
     * (proc exit path); a daemonized grandchild survives that cascade, the
     * same way service-manager-started providers do.  setsid additionally
     * detaches the service from this process's session/controlling group. */
    pid_t pid = fork();
    if (pid == 0) {
        pid_t grand = fork();
        if (grand > 0)
            _exit(0);
        if (grand == 0) {
            (void)setsid();
            execl(A20_NATIVE_BIN("ufsd"), "ufsd", dir, idx, fstype, (char *)0);
        }
        _exit(90);
    }
    if (pid < 0)
        return -1;
    if (wait_live(dir, 750) == 0) { /* mount RPCs on a slow disk */
        (void)waitpid(pid, NULL, WNOHANG); /* reap the intermediate */
        return 0;
    }
    (void)waitpid(pid, NULL, 0);
    return -1;
}

static int check_r4(const char *dir)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/UFXR4.BIN", dir);
    unlink(path);

    unsigned char *w = malloc(R4_BYTES);
    unsigned char *r = malloc(R4_BYTES);
    if (!w || !r) {
        free(w);
        free(r);
        return fail("r4-alloc", path);
    }

    int ret = 0;
    int fd1 = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd1 < 0)
        ret = fail("r4-open1", path);
    int fd2 = -1;
    if (ret == 0) {
        /* The second fd is opened BEFORE the first fd writes; this order is
         * the regression's trigger and must not be reordered. */
        fd2 = open(path, O_RDONLY);
        if (fd2 < 0)
            ret = fail("r4-open2", path);
    }

    fill_pattern(w, R4_BYTES, 0x21);
    if (ret == 0 && write_full(fd1, w, R4_BYTES) != 0)
        ret = fail("r4-write", path);
    if (ret == 0 && fsync(fd1) != 0)
        ret = fail("r4-fsync", path);

    if (ret == 0) {
        struct stat st;
        if (fstat(fd2, &st) != 0)
            ret = fail("r4-fstat-fd2", path);
        else if (st.st_size != R4_BYTES)
            ret = fail("r4-stale-size on second fd", path);
    }
    if (ret == 0) {
        memset(r, 0, R4_BYTES);
        long n = read_full(fd2, r, R4_BYTES);
        if (n != (long)R4_BYTES)
            ret = fail("r4-second-fd-read short/EOF", path);
        else if (memcmp(w, r, R4_BYTES) != 0)
            ret = fail("r4-second-fd content", path);
    }
    /* A fresh open after the write must also observe the new size/bytes. */
    if (ret == 0) {
        int fd3 = open(path, O_RDONLY);
        if (fd3 < 0)
            ret = fail("r4-open3", path);
        else {
            struct stat st;
            if (fstat(fd3, &st) != 0 || st.st_size != R4_BYTES)
                ret = fail("r4-fresh-open size", path);
            else {
                memset(r, 0, R4_BYTES);
                long n = read_full(fd3, r, R4_BYTES);
                if (n != (long)R4_BYTES || memcmp(w, r, R4_BYTES) != 0)
                    ret = fail("r4-fresh-open content", path);
            }
            close(fd3);
        }
    }

    if (ret == 0)
        printf("UXFS_REOPEN: r4 second-fd-read=PASS bytes=%d\n", R4_BYTES);
    if (fd2 >= 0)
        close(fd2);
    if (fd1 >= 0)
        close(fd1);
    unlink(path);
    free(w);
    free(r);
    return ret;
}

static int check_symmetric(const char *dir)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/UFXSYM.BIN", dir);
    unlink(path);

    unsigned char *wa = malloc(SYM_BYTES);
    unsigned char *wb = malloc(SYM_BYTES);
    unsigned char *r = malloc(SYM_BYTES);
    if (!wa || !wb || !r) {
        free(wa);
        free(wb);
        free(r);
        return fail("sym-alloc", path);
    }

    int ret = 0;
    int fd1 = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd1 < 0)
        ret = fail("sym-open1", path);
    fill_pattern(wa, SYM_BYTES, 0x40);
    if (ret == 0 && write_full(fd1, wa, SYM_BYTES) != 0)
        ret = fail("sym-write1", path);
    if (ret == 0 && fsync(fd1) != 0)
        ret = fail("sym-fsync1", path);

    int fd2 = -1;
    if (ret == 0) {
        fd2 = open(path, O_RDWR);
        if (fd2 < 0)
            ret = fail("sym-open2", path);
    }
    fill_pattern(wb, SYM_BYTES, 0x60);
    if (ret == 0 && write_full(fd2, wb, SYM_BYTES) != 0)
        ret = fail("sym-write2", path);
    if (ret == 0 && fsync(fd2) != 0)
        ret = fail("sym-fsync2", path);

    if (ret == 0) {
        struct stat st;
        if (fstat(fd1, &st) != 0 || st.st_size != SYM_BYTES)
            ret = fail("sym-fd1 size", path);
        else if (lseek(fd1, 0, SEEK_SET) < 0)
            ret = fail("sym-fd1 lseek", path);
        else {
            memset(r, 0, SYM_BYTES);
            long n = read_full(fd1, r, SYM_BYTES);
            if (n != (long)SYM_BYTES || memcmp(wb, r, SYM_BYTES) != 0)
                ret = fail("sym-fd1 content", path);
        }
    }

    if (ret == 0)
        printf("UXFS_REOPEN: symmetric fd1-read=PASS bytes=%d\n", SYM_BYTES);
    if (fd2 >= 0)
        close(fd2);
    if (fd1 >= 0)
        close(fd1);
    unlink(path);
    free(wa);
    free(wb);
    free(r);
    return ret;
}

static int check_truncate(const char *dir)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/UFXTRC.BIN", dir);
    unlink(path);

    unsigned char *w = malloc(TRUNC_OLD);
    unsigned char *r = malloc(TRUNC_OLD);
    if (!w || !r) {
        free(w);
        free(r);
        return fail("trunc-alloc", path);
    }

    int ret = 0;
    int fd1 = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd1 < 0)
        ret = fail("trunc-open1", path);
    fill_pattern(w, TRUNC_OLD, 0x80);
    if (ret == 0 && write_full(fd1, w, TRUNC_OLD) != 0)
        ret = fail("trunc-write", path);
    if (ret == 0 && fsync(fd1) != 0)
        ret = fail("trunc-fsync", path);

    int fd2 = -1;
    if (ret == 0) {
        /* fd2 exists before the truncate; it must follow the new size. */
        fd2 = open(path, O_RDONLY);
        if (fd2 < 0)
            ret = fail("trunc-open2", path);
    }
    if (ret == 0 && getenv("UXFS_EXPECT_TRUNC_FAIL_ONCE")) {
        errno = 0;
        int trunc_rc = ftruncate(fd1, TRUNC_NEW);
        if (trunc_rc == 0 || errno != EIO) {
            ret = fail("trunc-injected-error", path);
        } else {
            struct stat st;
            if (fstat(fd2, &st) != 0 || st.st_size != TRUNC_OLD ||
                lseek(fd2, 0, SEEK_SET) != 0 ||
                read_full(fd2, r, TRUNC_OLD) != TRUNC_OLD ||
                memcmp(w, r, TRUNC_OLD) != 0)
                ret = fail("trunc-failure-state", path);
            else
                printf("UXFS_REOPEN: truncate-failure-rollback=PASS errno=EIO size=%d\n",
                       TRUNC_OLD);
        }
    }
    if (ret == 0 && ftruncate(fd1, TRUNC_NEW) != 0)
        ret = fail("trunc-ftruncate", path);
    if (ret == 0 && fsync(fd1) != 0)
        ret = fail("trunc-fsync2", path);

    if (ret == 0) {
        struct stat st;
        if (fstat(fd2, &st) != 0)
            ret = fail("trunc-fstat-fd2", path);
        else if (st.st_size != TRUNC_NEW)
            ret = fail("trunc-fd2 stale size", path);
    }
    if (ret == 0) {
        memset(r, 0, TRUNC_NEW);
        long n = lseek(fd2, 0, SEEK_SET) == 0 ? read_full(fd2, r, TRUNC_NEW) : -1;
        if (n != (long)TRUNC_NEW)
            ret = fail("trunc-fd2 short read", path);
        else if (memcmp(w, r, TRUNC_NEW) != 0)
            ret = fail("trunc-fd2 content", path);
    }
    if (ret == 0) {
        char tail;
        if (read(fd2, &tail, 1) != 0)
            ret = fail("trunc-fd2 EOF", path);
    }

    /* Exercise the block-boundary and full-drop paths on the same open
     * inode, then close/reopen so the persisted size is checked as well. */
    if (ret == 0 && ftruncate(fd1, 4096) != 0)
        ret = fail("trunc-4096", path);
    if (ret == 0 && fsync(fd1) != 0)
        ret = fail("trunc-4096-fsync", path);
    if (ret == 0) {
        struct stat st;
        if (fstat(fd2, &st) != 0 || st.st_size != 4096)
            ret = fail("trunc-4096-size", path);
    }
    if (ret == 0 && (lseek(fd2, 0, SEEK_SET) != 0 ||
                     read_full(fd2, r, 4096) != 4096 ||
                     memcmp(w, r, 4096) != 0))
        ret = fail("trunc-4096-readback", path);
    if (ret == 0 && ftruncate(fd1, 0) != 0)
        ret = fail("trunc-zero", path);
    if (ret == 0 && fsync(fd1) != 0)
        ret = fail("trunc-zero-fsync", path);
    if (fd2 >= 0) { close(fd2); fd2 = -1; }
    if (fd1 >= 0) { close(fd1); fd1 = -1; }
    if (ret == 0) {
        fd1 = open(path, O_RDONLY);
        struct stat st;
        if (fd1 < 0 || fstat(fd1, &st) != 0 || st.st_size != 0)
            ret = fail("trunc-zero-reopen-size", path);
        else if (read(fd1, r, 1) != 0)
            ret = fail("trunc-zero-reopen-eof", path);
    }

    if (ret == 0)
        printf("UXFS_REOPEN: truncate-boundaries=PASS sizes=4097,4096,0\n");
    if (fd2 >= 0)
        close(fd2);
    if (fd1 >= 0)
        close(fd1);
    unlink(path);
    free(w);
    free(r);
    return ret;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Narrow opt-in controller timing case. Run the same guest payload once
 * against each UFSD build (UXFS_BENCH_IMPL=c|rust), preserving the full
 * workload's default path and reporting correctness with every sample. */
static int check_truncate_bench(const char *dir)
{
    const char *impl = getenv("UXFS_BENCH_IMPL");
    if (!impl || (strcmp(impl, "c") != 0 && strcmp(impl, "rust") != 0)) {
        printf("UXFS_BENCH: FAIL set UXFS_BENCH_IMPL=c|rust\n");
        return 1;
    }
    char path[300];
    if (snprintf(path, sizeof(path), "%s/UFXBENCH.BIN", dir) >= (int)sizeof(path))
        return fail("bench-path", dir);
    unsigned char *w = malloc(TRUNC_OLD);
    unsigned char *r = malloc(TRUNC_NEW);
    if (!w || !r) {
        free(w);
        free(r);
        return fail("bench-alloc", path);
    }
    fill_pattern(w, TRUNC_OLD, 0x6b); /* fixed seed across C/Rust runs */
    int ret = 0;
    for (int round = 0; round <= 5 && ret == 0; round++) {
        (void)unlink(path);
        uint64_t total_start = monotonic_ns();
        uint64_t t0 = total_start;
        int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0 || write_full(fd, w, TRUNC_OLD) != 0) {
            ret = fail("bench-write", path);
            if (fd >= 0) close(fd);
            break;
        }
        uint64_t write_ns = monotonic_ns() - t0;

        t0 = monotonic_ns();
        if (ftruncate(fd, TRUNC_NEW) != 0) {
            ret = fail("bench-truncate", path);
            close(fd);
            break;
        }
        uint64_t truncate_ns = monotonic_ns() - t0;

        t0 = monotonic_ns();
        if (fsync(fd) != 0) {
            ret = fail("bench-sync", path);
            close(fd);
            break;
        }
        uint64_t sync_ns = monotonic_ns() - t0;

        t0 = monotonic_ns();
        if (close(fd) != 0) {
            ret = fail("bench-close", path);
            break;
        }
        fd = open(path, O_RDONLY);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0 || st.st_size != TRUNC_NEW ||
            read_full(fd, r, TRUNC_NEW) != TRUNC_NEW ||
            memcmp(w, r, TRUNC_NEW) != 0) {
            ret = fail("bench-reopen-content-or-size", path);
        } else {
            char tail;
            if (read(fd, &tail, 1) != 0)
                ret = fail("bench-reopen-eof", path);
        }
        uint64_t reopen_ns = monotonic_ns() - t0;
        if (fd >= 0)
            close(fd);
        if (ret == 0) {
            uint64_t total_ns = monotonic_ns() - total_start;
            printf("UXFS_BENCH: impl=%s round=%s write_ns=%llu truncate_ns=%llu sync_ns=%llu reopen_verify_ns=%llu total_ns=%llu size=%d content=PASS eof=PASS\n",
                   impl, round == 0 ? "warmup" : (round == 1 ? "1" :
                   round == 2 ? "2" : round == 3 ? "3" :
                   round == 4 ? "4" : "5"),
                   (unsigned long long)write_ns,
                   (unsigned long long)truncate_ns,
                   (unsigned long long)sync_ns,
                   (unsigned long long)reopen_ns,
                   (unsigned long long)total_ns, TRUNC_NEW);
        }
    }
    (void)unlink(path);
    free(w);
    free(r);
    if (ret == 0)
        printf("UXFS_BENCH: PASS impl=%s warmup=1 paired_rounds=5 seed=0x6b bytes=%d->%d\n",
               impl, TRUNC_OLD, TRUNC_NEW);
    return ret;
}

static int check_unlink_recreate(const char *dir)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/UFXREC.BIN", dir);
    unlink(path);

    unsigned char *wa = malloc(RECYCLE_BYTES);
    unsigned char *wb = malloc(RECYCLE_BYTES);
    unsigned char *r = malloc(RECYCLE_BYTES);
    if (!wa || !wb || !r) {
        free(wa);
        free(wb);
        free(r);
        return fail("recycle-alloc", path);
    }

    int ret = 0;
    int fd1 = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd1 < 0)
        ret = fail("recycle-open1", path);
    fill_pattern(wa, RECYCLE_BYTES, 0xa0);
    if (ret == 0 && write_full(fd1, wa, RECYCLE_BYTES) != 0)
        ret = fail("recycle-write", path);
    if (ret == 0 && fsync(fd1) != 0)
        ret = fail("recycle-fsync", path);

    int fd2 = -1;
    if (ret == 0) {
        fd2 = open(path, O_RDONLY);
        if (fd2 < 0)
            ret = fail("recycle-open2", path);
    }
    if (ret == 0 && unlink(path) != 0)
        ret = fail("recycle-unlink", path);
    if (ret == 0) {
        if (open(path, O_RDONLY) >= 0)
            ret = fail("recycle-still-present", path);
        else if (errno != ENOENT)
            ret = fail("recycle-unlink errno", path);
    }
    /* The unlinked fd keeps reading its own inode. */
    if (ret == 0) {
        memset(r, 0, RECYCLE_BYTES);
        long n = read_full(fd2, r, RECYCLE_BYTES);
        if (n != (long)RECYCLE_BYTES || memcmp(wa, r, RECYCLE_BYTES) != 0)
            ret = fail("recycle-old-fd after unlink", path);
    }
    /* Recreate the same path: new opens must see the new file, even if the
     * backend reuses the freed inode number. */
    if (ret == 0) {
        int fd3 = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd3 < 0)
            ret = fail("recycle-recreate", path);
        else if (write_full(fd3, wb, RECYCLE_BYTES) != 0 || fsync(fd3) != 0)
            ret = fail("recycle-recreate write", path);
        if (fd3 >= 0)
            close(fd3);
    }
    if (ret == 0) {
        int fd4 = open(path, O_RDONLY);
        if (fd4 < 0)
            ret = fail("recycle-open4", path);
        else {
            struct stat st;
            memset(r, 0, RECYCLE_BYTES);
            long n = read_full(fd4, r, RECYCLE_BYTES);
            if (fstat(fd4, &st) != 0 || st.st_size != RECYCLE_BYTES ||
                n != (long)RECYCLE_BYTES)
                ret = fail("recycle-new size/read", path);
            else if (memcmp(wb, r, RECYCLE_BYTES) != 0)
                ret = fail("recycle-new content", path);
            close(fd4);
        }
    }
    /* The stale fd still belongs to the old inode and keeps its bytes. */
    if (ret == 0 && lseek(fd2, 0, SEEK_SET) < 0)
        ret = fail("recycle-fd2 lseek", path);
    if (ret == 0) {
        memset(r, 0, RECYCLE_BYTES);
        long n = read_full(fd2, r, RECYCLE_BYTES);
        if (n != (long)RECYCLE_BYTES || memcmp(wa, r, RECYCLE_BYTES) != 0)
            ret = fail("recycle-old fd changed", path);
    }

    if (ret == 0)
        printf("UXFS_REOPEN: unlink-recreate=PASS\n");
    if (fd2 >= 0)
        close(fd2);
    if (fd1 >= 0)
        close(fd1);
    unlink(path);
    free(wa);
    free(wb);
    free(r);
    return ret;
}

/* MAP_SHARED store through fd1 must be visible to fd2's read without msync. */
static int check_mmap_write_to_read(const char *dir)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/UFXMM1.BIN", dir);
    unlink(path);

    unsigned char *wb = malloc(MMAP_BYTES);
    unsigned char *r = malloc(MMAP_BYTES);
    if (!wb || !r) {
        free(wb);
        free(r);
        return fail("mmap1-alloc", path);
    }

    int ret = 0;
    int fd1 = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    unsigned char *init = malloc(MMAP_BYTES);
    if (fd1 < 0 || !init) {
        free(init);
        free(wb);
        free(r);
        if (fd1 >= 0)
            close(fd1);
        return fail("mmap1-open", path);
    }
    fill_pattern(init, MMAP_BYTES, 0xd0);
    if (write_full(fd1, init, MMAP_BYTES) != 0 || fsync(fd1) != 0)
        ret = fail("mmap1-seed", path);

    int fd2 = -1;
    if (ret == 0) {
        fd2 = open(path, O_RDWR);
        if (fd2 < 0)
            ret = fail("mmap1-open2", path);
    }
    char *mem = MAP_FAILED;
    if (ret == 0) {
        mem = mmap(NULL, MMAP_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd1,
                   0);
        if (mem == MAP_FAILED) {
            printf("UXFS_REOPEN: mmap-write-read=SKIP errno=%d %s\n", errno,
                   strerror(errno));
            close(fd2);
            close(fd1);
            unlink(path);
            free(init);
            free(wb);
            free(r);
            return 0;
        }
    }
    fill_pattern(wb, MMAP_BYTES, 0xe0);
    if (ret == 0) {
        memcpy(mem, wb, MMAP_BYTES);
        if (lseek(fd2, 0, SEEK_SET) < 0)
            ret = fail("mmap1-lseek", path);
    }
    if (ret == 0) {
        memset(r, 0, MMAP_BYTES);
        long n = read_full(fd2, r, MMAP_BYTES);
        if (n != (long)MMAP_BYTES || memcmp(wb, r, MMAP_BYTES) != 0)
            ret = fail("mmap1-read-after-store", path);
    }
    if (ret == 0)
        printf("UXFS_REOPEN: mmap-write-read=PASS bytes=%d\n", MMAP_BYTES);
    if (mem != MAP_FAILED) {
        fsync(fd1);
        munmap(mem, MMAP_BYTES);
    }
    if (fd2 >= 0)
        close(fd2);
    if (fd1 >= 0)
        close(fd1);
    unlink(path);
    free(init);
    free(wb);
    free(r);
    return ret;
}

/* A MAP_SHARED mapping over fd2 (opened before the write) must see fd1's
 * write. */
static int check_mmap_read_after_write(const char *dir)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/UFXMM2.BIN", dir);
    unlink(path);

    unsigned char *w = malloc(MMAP_BYTES);
    if (!w)
        return fail("mmap2-alloc", path);

    int ret = 0;
    int fd1 = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd1 < 0)
        ret = fail("mmap2-open1", path);
    int fd2 = -1;
    if (ret == 0) {
        fd2 = open(path, O_RDONLY);
        if (fd2 < 0)
            ret = fail("mmap2-open2", path);
    }
    fill_pattern(w, MMAP_BYTES, 0x38);
    if (ret == 0 && write_full(fd1, w, MMAP_BYTES) != 0)
        ret = fail("mmap2-write", path);
    if (ret == 0 && fsync(fd1) != 0)
        ret = fail("mmap2-fsync", path);

    char *mem = MAP_FAILED;
    if (ret == 0) {
        mem = mmap(NULL, MMAP_BYTES, PROT_READ, MAP_SHARED, fd2, 0);
        if (mem == MAP_FAILED) {
            printf("UXFS_REOPEN: mmap-read-after-write=SKIP errno=%d %s\n",
                   errno, strerror(errno));
            close(fd1);
            close(fd2);
            unlink(path);
            free(w);
            return 0;
        }
    }
    if (ret == 0 && memcmp(mem, w, MMAP_BYTES) != 0)
        ret = fail("mmap2-mapped content", path);
    if (ret == 0)
        printf("UXFS_REOPEN: mmap-read-after-write=PASS bytes=%d\n",
               MMAP_BYTES);
    if (mem != MAP_FAILED)
        munmap(mem, MMAP_BYTES);
    if (fd1 >= 0)
        close(fd1);
    if (fd2 >= 0)
        close(fd2);
    unlink(path);
    free(w);
    return ret;
}

static void fill_keep(unsigned char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (unsigned char)(i * 7 + 13);
}

static int keep_path(char *out, size_t cap, const char *dir)
{
    int n = snprintf(out, cap, "%s/UFXKEEP.BIN", dir);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

static int check_keep(const char *dir)
{
    char path[300];
    if (keep_path(path, sizeof(path), dir) != 0)
        return fail("keep-path", dir);
    unlink(path);

    unsigned char *w = malloc(KEEP_BYTES);
    if (!w)
        return fail("keep-alloc", path);

    int ret = 0;
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        ret = fail("keep-open", path);
    fill_keep(w, KEEP_BYTES);
    if (ret == 0 && write_full(fd, w, KEEP_BYTES) != 0)
        ret = fail("keep-write", path);
    if (ret == 0 && fsync(fd) != 0)
        ret = fail("keep-fsync", path);
    if (ret == 0 && close(fd) != 0)
        ret = fail("keep-close", path);

    /* The directory entry itself is durable only if the parent is synced. */
    if (ret == 0) {
        int dfd = open(dir, O_RDONLY);
        if (dfd < 0)
            ret = fail("keep-open-dir", dir);
        else {
            if (fsync(dfd) != 0)
                ret = fail("keep-fsync-dir", dir);
            close(dfd);
        }
    }
    if (ret == 0)
        printf("UXFS_TEST_KEEP: PASS bytes=%d path=%s\n", KEEP_BYTES, path);
    free(w);
    return ret;
}

static int verify_kept(const char *dir)
{
    char path[300];
    if (keep_path(path, sizeof(path), dir) != 0)
        return fail("keep-path", dir);

    unsigned char *w = malloc(KEEP_BYTES);
    unsigned char *r = malloc(KEEP_BYTES);
    if (!w || !r) {
        free(w);
        free(r);
        return fail("cold-alloc", path);
    }
    fill_keep(w, KEEP_BYTES);

    int ret = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        ret = fail("cold-open", path);
    if (ret == 0) {
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size != KEEP_BYTES)
            ret = fail("cold-size", path);
    }
    if (ret == 0) {
        memset(r, 0, KEEP_BYTES);
        long n = read_full(fd, r, KEEP_BYTES);
        if (n != (long)KEEP_BYTES)
            ret = fail("cold-short-read", path);
        else if (memcmp(w, r, KEEP_BYTES) != 0)
            ret = fail("cold-content", path);
    }
    if (ret == 0) {
        char tail;
        if (read(fd, &tail, 1) != 0)
            ret = fail("cold-tail", path);
    }
    if (ret == 0)
        printf("UXFS_REOPEN_COLD: PASS bytes=%d\n", KEEP_BYTES);
    if (fd >= 0)
        close(fd);
    free(w);
    free(r);
    return ret;
}

int main(void)
{
    const char *dir = getenv("UXFS_TEST_DIR");
    const char *which = getenv("UXFS_TEST_CASE");
    int rc = 0;
    if (!dir || !dir[0])
        dir = "/ufs";

    if (ensure_mount(dir) != 0) {
        printf("UXFS_REOPEN: FAIL mount %s (errno=%d %s)\n", dir, errno,
               strerror(errno));
        return 1;
    }
    printf("UXFS_REOPEN: mount-ready dir=%s\n", dir);

    if (getenv("UXFS_TEST_VERIFY"))
        return verify_kept(dir) ? 1 : 0;

    /* Phase selection is test-only; an unset or "full" value preserves the
     * original complete sequence and ordering. */
    if (which && which[0] && strcmp(which, "full") != 0) {
        if (strcmp(which, "r4") == 0)
            rc = check_r4(dir);
        else if (strcmp(which, "symmetric") == 0)
            rc = check_symmetric(dir);
        else if (strcmp(which, "truncate") == 0)
            rc = check_truncate(dir);
        else if (strcmp(which, "bench") == 0)
            rc = check_truncate_bench(dir);
        else if (strcmp(which, "unlink-recreate") == 0)
            rc = check_unlink_recreate(dir);
        else if (strcmp(which, "mmap") == 0) {
            rc = check_mmap_write_to_read(dir);
            if (!rc)
                rc = check_mmap_read_after_write(dir);
        } else if (strcmp(which, "keep") == 0)
            rc = check_keep(dir);
        else {
            printf("UXFS_REOPEN: FAIL unknown test case %s\n", which);
            return 2;
        }
        if (rc == 0)
            printf("UXFS_TEST_CASE: %s PASS\n", which);
        return rc ? 1 : 0;
    }

    if (check_r4(dir))
        return 1;
    if (check_symmetric(dir))
        return 1;
    if (check_truncate(dir))
        return 1;
    if (check_unlink_recreate(dir))
        return 1;
    if (check_mmap_write_to_read(dir))
        return 1;
    if (check_mmap_read_after_write(dir))
        return 1;
    if (check_keep(dir))
        return 1;

    printf("UXFS_REOPEN: PASS\n");
    return 0;
}
