/* Same static ELF on host/guest: ordered versus random first dirtying.
 * Read every page before the timed loop; allocation/read faults are excluded.
 * Use a regular filesystem path, and keep fsync/close/unlink outside timing. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock"); exit(1); }
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static uint32_t random_state = 0x94ac1731;
static uint32_t random_u32(void)
{
    uint32_t x = random_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return random_state = x;
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    const char *path = argv[1], *order = argv[3];
    unsigned pages = strtoul(argv[2], NULL, 10);
    if (!pages || pages > 65536 ||
        (strcmp(order, "ascending") && strcmp(order, "descending") && strcmp(order, "random"))) return 2;
    uint32_t *indices = malloc((size_t)pages * sizeof(*indices));
    if (!indices) return 1;
    for (unsigned i = 0; i < pages; i++) indices[i] = i;
    if (!strcmp(order, "random")) {
        for (unsigned i = pages; i > 1; i--) {
            unsigned j = random_u32() % i;
            uint32_t tmp = indices[i - 1]; indices[i - 1] = indices[j]; indices[j] = tmp;
        }
    } else if (!strcmp(order, "descending")) {
        for (unsigned i = 0; i < pages; i++) indices[i] = pages - i - 1;
    }
    int rc = 1, fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { perror("open exclusive"); free(indices); return 1; }
    struct statfs fs;
    if (fstatfs(fd, &fs)) { perror("fstatfs"); goto out; }
    /* TMPFS_MAGIC: a tmpfs write path need not traverse file-cache dirty lists. */
    if ((unsigned long)fs.f_type == 0x01021994UL) {
        fprintf(stderr, "regular filesystem required, tmpfs rejected\n"); goto out;
    }
    if (ftruncate(fd, (off_t)pages * 4096)) { perror("truncate"); goto out; }
    unsigned char byte = 0;
    /* Allocate/write the file sequentially and clean its cache pages first,
     * so the timed random loop does not also measure first block allocation. */
    for (unsigned i = 0; i < pages; i++) {
        if (pwrite(fd, &byte, 1, (off_t)i * 4096) != 1) {
            perror("prepare write"); goto out;
        }
    }
    if (fsync(fd)) { perror("prepare fsync"); goto out; }
    for (unsigned i = 0; i < pages; i++) {
        if (pread(fd, &byte, 1, (off_t)i * 4096) != 1 || byte != 0) {
            perror("prewarm read"); goto out;
        }
    }
    uint64_t start = now_ns();
    for (unsigned i = 0; i < pages; i++) {
        byte = (unsigned char)((indices[i] % 251) + 1);
        if (pwrite(fd, &byte, 1, (off_t)indices[i] * 4096) != 1) {
            perror("first dirty write"); goto out;
        }
    }
    uint64_t elapsed = now_ns() - start;
    for (unsigned i = 0; i < pages; i++) {
        if (pread(fd, &byte, 1, (off_t)i * 4096) != 1 ||
            byte != (unsigned char)((i % 251) + 1)) {
            fprintf(stderr, "verify failed at page %u\n", i); goto out;
        }
    }
    uint64_t flush_start = now_ns();
    if (fsync(fd)) { perror("fsync"); goto out; }
    uint64_t flush_elapsed = now_ns() - flush_start;
    printf("DIRTY_INDEX order=%s pages=%u fs_type=0x%lx seed=0x94ac1731 elapsed_ns=%llu ns_per_page=%.2f verified=1 fsync_ns=%llu\n",
           order, pages, (unsigned long)fs.f_type, (unsigned long long)elapsed, (double)elapsed / pages, (unsigned long long)flush_elapsed);
    rc = 0;
out:
    if (close(fd)) { perror("close"); rc = 1; }
    if (unlink(path)) { perror("unlink"); rc = 1; }
    free(indices);
    return rc;
}
