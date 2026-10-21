#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_mount
#define SYS_mount 40
#endif
#ifndef SYS_umount2
#define SYS_umount2 39
#endif

/*
 * Concurrent VFS workload for the SMP lock-contention measurements.
 *
 * Every worker hammers the same directory, the same directory entries and the
 * same page cache from its own CPU, which is what makes dcache, page_cache and
 * block_cache contend; each worker also mounts and unmounts a private ramfs so
 * the mount table is touched concurrently.  Work is entirely deterministic:
 * a worker's per-round operation set comes from a PRNG seeded with
 * (seed, worker), and every selected operation must succeed, so the parent can
 * replay the same sequence to predict the exact per-worker counts and compare
 * them against what the workers report.  A silently dropped or failed
 * operation therefore shows up as a count mismatch instead of a lost result.
 *
 * Usage: vfs_concurrency [--seed N] [--workers N] [--rounds N] [--ops MASK]
 *
 * --ops narrows the round operation mask, which is what makes a failure in
 * this workload bisectable down to the operation that caused it.
 */

#define ROOT_DIR "/tmp/vfsconc"
#define DEFAULT_SEED 20261021u
#define DEFAULT_WORKERS 4
#define DEFAULT_ROUNDS 64
#define PAYLOAD_MAX 512

/* Round operation bits.  All operations use round-indexed names, so the
 * selected set is the whole schedule: nothing depends on runtime state. */
#define OP_CREATE_WRITE 0x1u
#define OP_RENAME 0x2u
#define OP_STAT 0x4u
#define OP_UNLINK 0x8u
#define OP_MKDIR_RMDIR 0x10u
#define OP_MOUNT_CYCLE 0x20u
#define OP_MASK 0x3fu

static uint32_t prng_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static uint32_t round_ops(uint32_t seed, int worker, int round,
                          uint32_t ops_mask) {
    uint32_t state = seed ^ (uint32_t)(worker * 0x9e3779b9u) ^
                     (uint32_t)(round * 0x85ebca6bu + 0x165667b1u);
    if (state == 0)
        state = 0x2545f491u;
    prng_next(&state);
    uint32_t ops = prng_next(&state) & ops_mask;
    /* Close the mask under the rename chain so every selected operation has
     * its input: unlink needs the renamed file, stat needs it too, and a
     * rename needs the created file.  The parent normalizes identically, so
     * the schedule stays a pure function of (seed, worker, round). */
    if (ops & OP_UNLINK)
        ops |= OP_STAT;
    if (ops & OP_STAT)
        ops |= OP_RENAME;
    if (ops & OP_RENAME)
        ops |= OP_CREATE_WRITE;
    return ops;
}

struct counts {
    unsigned long create, rename, stat_, unlink_, mkdir_rmdir, mount_cycle;
};

/* The report travels over a shared pipe, so it carries its own worker index:
 * the parent must not assume the read order matches the fork order. */
struct report {
    int worker;
    struct counts done;
};

static void count_ops(struct counts *c, uint32_t ops) {
    if (ops & OP_CREATE_WRITE)
        c->create++;
    if (ops & OP_RENAME)
        c->rename++;
    if (ops & OP_STAT)
        c->stat_++;
    if (ops & OP_UNLINK)
        c->unlink_++;
    if (ops & OP_MKDIR_RMDIR)
        c->mkdir_rmdir++;
    if (ops & OP_MOUNT_CYCLE)
        c->mount_cycle++;
}

static void worker_path(char *buf, size_t size, const char *dir, int round,
                        const char *prefix, const char *suffix) {
    snprintf(buf, size, "%s/%s%04d%s", dir, prefix, round, suffix);
}

static int worker_run(int worker, uint32_t seed, int rounds, uint32_t ops_mask,
                      char *dir, size_t dir_size, int report_fd) {
    char path[256], renamed[256], subdir[256];
    char payload[PAYLOAD_MAX];
    struct counts done = {0};

    snprintf(dir, dir_size, ROOT_DIR "/w%d", worker);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return 100;

    for (int round = 0; round < rounds; round++) {
        uint32_t ops = round_ops(seed, worker, round, ops_mask);
        count_ops(&done, ops);

        if (ops & OP_CREATE_WRITE) {
            size_t len = 1 + (unsigned)((ops >> 3) * 7u + (unsigned)round) %
                                 PAYLOAD_MAX;
            memset(payload, (int)(0x40 + worker), len);
            worker_path(path, sizeof(path), dir, round, "f", "");
            int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
            if (fd < 0)
                return 101;
            if (write(fd, payload, len) != (ssize_t)len)
                return 102;
            if (close(fd) != 0)
                return 103;
        }
        if (ops & OP_RENAME) {
            worker_path(path, sizeof(path), dir, round, "f", "");
            worker_path(renamed, sizeof(renamed), dir, round, "g", "");
            if (rename(path, renamed) != 0)
                return 104;
        }
        if (ops & OP_STAT) {
            struct stat st;
            worker_path(renamed, sizeof(renamed), dir, round, "g", "");
            if (stat(renamed, &st) != 0)
                return 105;
        }
        if (ops & OP_UNLINK) {
            worker_path(renamed, sizeof(renamed), dir, round, "g", "");
            if (unlink(renamed) != 0)
                return 106;
        }
        if (ops & OP_MKDIR_RMDIR) {
            snprintf(subdir, sizeof(subdir), "%s/d%04d", dir, round);
            if (mkdir(subdir, 0755) != 0)
                return 107;
            if (rmdir(subdir) != 0)
                return 108;
        }
        if (ops & OP_MOUNT_CYCLE) {
            snprintf(subdir, sizeof(subdir), ROOT_DIR "/m%d", worker);
            if (mkdir(subdir, 0755) != 0 && errno != EEXIST)
                return 109;
            if (syscall(SYS_mount, "none", subdir, "ramfs", 0, "") != 0)
                return 110;
            if (syscall(SYS_umount2, subdir, 0) != 0)
                return 111;
        }
        /* Round-scoped names make the rounds independent, but a round that
         * created without renaming or unlinking leaves a file behind.  Clear
         * both names so the next round starts clean and the directory ends
         * empty; these unlinks are housekeeping, not scheduled operations. */
        worker_path(path, sizeof(path), dir, round, "f", "");
        worker_path(renamed, sizeof(renamed), dir, round, "g", "");
        unlink(path);
        unlink(renamed);
    }

    printf("VFS_CONC: worker=%d create=%lu rename=%lu stat=%lu unlink=%lu "
           "mkdir-rmdir=%lu mount-cycle=%lu\n",
           worker, done.create, done.rename, done.stat_, done.unlink_,
           done.mkdir_rmdir, done.mount_cycle);
    fflush(stdout);
    if (report_fd >= 0) {
        struct report msg = {.worker = worker, .done = done};
        ssize_t n = write(report_fd, &msg, sizeof(msg));
        if (n != (ssize_t)sizeof(msg))
            return 112;
    }
    return 0;
}

int main(int argc, char **argv) {
    uint32_t seed = DEFAULT_SEED;
    uint32_t ops_mask = OP_MASK;
    int workers = DEFAULT_WORKERS, rounds = DEFAULT_ROUNDS;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc)
            workers = atoi(argv[++i]);
        else if (strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
            rounds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ops") == 0 && i + 1 < argc)
            ops_mask = (uint32_t)strtoul(argv[++i], NULL, 0) & OP_MASK;
        else {
            printf("VFS_CONC: FAIL bad-argument %s\n", argv[i]);
            return 1;
        }
    }
    if (workers < 1 || workers > 16 || rounds < 1 || rounds > 4096) {
        printf("VFS_CONC: FAIL bad-parameters workers=%d rounds=%d\n",
               workers, rounds);
        return 1;
    }

    printf("VFS_CONC: start seed=%u workers=%d rounds=%d ops=0x%x\n", seed,
           workers, rounds, ops_mask);

    mkdir(ROOT_DIR, 0755);
    int report[2];
    if (pipe(report) != 0) {
        printf("VFS_CONC: FAIL pipe errno=%d\n", errno);
        return 1;
    }
    char dir[64];
    pid_t pids[16];
    int failures = 0;
    for (int w = 0; w < workers; w++) {
        pid_t pid = fork();
        if (pid < 0) {
            printf("VFS_CONC: FAIL fork errno=%d\n", errno);
            return 1;
        }
        if (pid == 0) {
            close(report[0]);
            _exit(worker_run(w, seed, rounds, ops_mask, dir, sizeof(dir),
                             report[1]));
        }
        pids[w] = pid;
    }
    close(report[1]);

    /* Each worker reports the operation counts it actually completed; the
     * report names its own worker, so the shared pipe's read order does not
     * have to match the fork order. */
    struct report msgs[16];
    struct counts seen[16];
    memset(seen, 0, sizeof(seen));
    unsigned reported = 0;
    size_t got = 0;
    while (got < workers * sizeof(msgs[0])) {
        ssize_t n = read(report[0], (char *)msgs + got,
                         workers * sizeof(msgs[0]) - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(report[0]);
    for (size_t off = 0; off + sizeof(msgs[0]) <= got; off += sizeof(msgs[0])) {
        struct report msg;
        memcpy(&msg, (char *)msgs + off, sizeof(msg));
        if (msg.worker < 0 || msg.worker >= workers) {
            printf("VFS_CONC: FAIL bad-worker-index %d\n", msg.worker);
            failures++;
            continue;
        }
        seen[msg.worker] = msg.done;
        reported++;
    }
    if (reported != (unsigned)workers) {
        printf("VFS_CONC: FAIL worker-reports got=%u want=%d\n", reported,
               workers);
        failures++;
    }

    for (int w = 0; w < workers; w++) {
        int status = 0;
        if (waitpid(pids[w], &status, 0) != pids[w] || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            printf("VFS_CONC: FAIL worker=%d status=%d\n", w, status);
            failures++;
        }
    }


    /* Replay the same schedule: every selected operation had to succeed, so
     * the observed counts are a function of (seed, worker, rounds) alone.  A
     * silently failed or dropped operation shows up as a mismatch here. */
    for (int w = 0; w < workers; w++) {
        struct counts want = {0};
        for (int round = 0; round < rounds; round++)
            count_ops(&want, round_ops(seed, w, round, ops_mask));
        if (memcmp(&want, &seen[w], sizeof(want)) != 0) {
            printf("VFS_CONC: FAIL worker=%d counts want=%lu/%lu/%lu/%lu/%lu/%lu"
                   " got=%lu/%lu/%lu/%lu/%lu/%lu\n",
                   w, want.create, want.rename, want.stat_, want.unlink_,
                   want.mkdir_rmdir, want.mount_cycle, seen[w].create,
                   seen[w].rename, seen[w].stat_, seen[w].unlink_,
                   seen[w].mkdir_rmdir, seen[w].mount_cycle);
            failures++;
        }
    }
    if (!failures)
        printf("VFS_CONC: counts-match PASS\n");

    /* Every worker dir must be empty again and the mount points unused. */
    for (int w = 0; w < workers; w++) {
        char wd[64];
        snprintf(wd, sizeof(wd), ROOT_DIR "/w%d", w);
        if (rmdir(wd) != 0) {
            printf("VFS_CONC: FAIL worker=%d dir-not-empty errno=%d\n", w,
                   errno);
            failures++;
        }
    }
    for (int w = 0; w < workers; w++) {
        char md[64];
        snprintf(md, sizeof(md), ROOT_DIR "/m%d", w);
        rmdir(md);
    }
    if (rmdir(ROOT_DIR) != 0) {
        printf("VFS_CONC: FAIL root-not-empty errno=%d\n", errno);
        failures++;
    } else {
        printf("VFS_CONC: cleanup PASS\n");
    }

    if (failures)
        return 1;
    printf("VFS_CONC: PASS\n");
    return 0;
}
