/* mm_bench: cost probe for two mm hot paths.
 *
 * Phase A brackets a MADV_DONTNEED over a fully faulted multi-megabyte range
 * with the kernel's own vma_lookups / vma_lookup_steps counters, so the size of
 * the VMA walk the syscall performs is reported rather than inferred.  This
 * is the Linux ABI madvise(2) path, which implements the release advice in
 * abi/linux/sys_mm.c and does not call mm_madvise_dontneed(); the counter is
 * what distinguishes the two implementations.
 *
 * Phase B times page-allocator churn -- fault in and release a multi-megabyte
 * anonymous region repeatedly -- the path whose per-frame diagnostic
 * bookkeeping is compiled out by CONFIG_DEBUG_MM_TRACE.  Wall clock is a
 * coarse instrument for that change, so the phase reports the minimum over
 * several rounds as well as the mean.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PERF_PATH    "/proc/a20/perf"
#define CHURN_MB     16UL
#define CHURN_ROUNDS 12
#define SPAN_MB      128UL

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Arm the kernel counter bank: a read both starts collection and takes the
 * snapshot, so counters left dormant report zero regardless of the workload. */
static int perf_arm(void)
{
    char buf[4096];
    int fd = open(PERF_PATH, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    return n > 0 ? 0 : -1;
}

static int perf_reset(void)
{
    int fd = open(PERF_PATH, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, "reset", 5);
    close(fd);
    return n == 5 ? 0 : -1;
}

static unsigned long perf_get(const char *key)
{
    char buf[8192];
    char needle[96];
    snprintf(needle, sizeof(needle), "\n%s: ", key);
    int fd = open(PERF_PATH, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    /* The first line has no leading newline, so probe it separately. */
    if (strncmp(buf, key, strlen(key)) == 0 && buf[strlen(key)] == ':') {
        char *p = buf + strlen(key) + 1;
        while (*p == ' ')
            p++;
        return strtoul(p, NULL, 10);
    }
    char *p = strstr(buf, needle);
    if (!p)
        return 0;
    p += strlen(needle);
    return strtoul(p, NULL, 10);
}

static void phase_a(void)
{
    size_t span = SPAN_MB * 1024 * 1024;
    char *base = mmap(NULL, span, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        printf("MM_BENCH: FAIL mmap span errno=%d\n", errno);
        return;
    }

    /* Every page is touched so the release advice has a full complement of
     * present leaves to tear down rather than walking a mostly empty range. */
    for (size_t off = 0; off < span; off += 4096)
        base[off] = (char)off;

    if (perf_arm() || perf_reset()) {
        printf("MM_BENCH: FAIL perf errno=%d\n", errno);
        munmap(base, span);
        return;
    }

    unsigned long lookups0 = perf_get("vma_lookups");
    unsigned long steps0 = perf_get("vma_lookup_steps");

    uint64_t t0 = now_ns();
    int rc = madvise(base, span, MADV_DONTNEED);
    uint64_t t1 = now_ns();

    unsigned long lookups = perf_get("vma_lookups");
    unsigned long steps = perf_get("vma_lookup_steps");

    printf("MM_BENCH phase_a span_mb=%lu rc=%d lookups0=%lu steps0=%lu "
           "lookups=%lu lookup_steps=%lu ns=%llu\n",
           SPAN_MB, rc, lookups0, steps0, lookups, steps,
           (unsigned long long)(t1 - t0));

    munmap(base, span);
}

static void phase_b(void)
{
    size_t len = CHURN_MB * 1024 * 1024;
    uint64_t best = ~0ull;
    uint64_t total = 0;

    for (int round = 0; round < CHURN_ROUNDS; round++) {
        uint64_t t0 = now_ns();
        char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            printf("MM_BENCH: FAIL churn mmap errno=%d\n", errno);
            return;
        }
        for (size_t off = 0; off < len; off += 4096)
            p[off] = (char)off;
        munmap(p, len);
        uint64_t d = now_ns() - t0;
        if (d < best)
            best = d;
        total += d;
    }

    printf("MM_BENCH phase_b churn_mb=%lu rounds=%d min_ns=%llu mean_ns=%llu\n",
           CHURN_MB, CHURN_ROUNDS, (unsigned long long)best,
           (unsigned long long)(total / CHURN_ROUNDS));
}

int main(void)
{
    phase_a();
    phase_b();
    printf("MM_BENCH: DONE\n");
    return 0;
}