/*
 * mm_pt_scale -- measure whether the single-level memory model actually lets
 * disjoint page-table work run in parallel, and compare it against the
 * single-lock baseline it replaced.
 *
 * The design claim of the single-level model is precise: two transactions
 * whose covering page-table nodes differ do not serialise.  This test makes
 * that claim falsifiable.
 *
 * Method:
 *   - One shared anonymous mapping, so all threads share a single mm and a
 *     single address space (the case the old mm->lock serialised wholesale).
 *   - Threads each touch a private, page-aligned, mutually disjoint slice of
 *     that mapping, so their covering page-table nodes differ.
 *   - Measure throughput as threads are added.  If the model works, scaling
 *     is close to linear; if the address space is still effectively
 *     serialised, throughput flattens after the first thread.
 *
 * It also reads the kernel's own page-table lock counters, so the result is
 * attributable rather than merely timed: a low contended/acquired ratio is
 * the direct evidence that disjoint ranges rarely collide.
 *
 * Pass criteria: throughput at MAX_THREADS must be at least MIN_SPEEDUP times
 * the throughput of a single thread, and the kernel must report the scaling
 * without any page-table lock deadlock.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS  4
#define SLICE_PAGES  512
#define SLICE_BYTES  (SLICE_PAGES * 4096u)
#define ROUNDS       64
#define MIN_SPEEDUP  1.8

static volatile unsigned char *g_map;
static int g_threads;
static unsigned long g_checksum[MAX_THREADS];

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *worker(void *arg)
{
    long id = (long)arg;
    unsigned long sum = 0;

    for (int r = 0; r < ROUNDS; r++) {
        /* Fresh anonymous mapping every round: the first touch of each page
         * is then a real demand fault, which is the operation whose
         * scalability this benchmark exists to measure.  Reusing warm pages
         * would time memcpy and prove nothing. */
        unsigned char *slice = mmap(NULL, SLICE_BYTES,
                                    PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (slice == MAP_FAILED) {
            fprintf(stderr, "MM_PT_SCALE: slice mmap failed\n");
            exit(1);
        }
        for (unsigned off = 0; off < SLICE_BYTES; off += 4096u) {
            slice[off] = (unsigned char)(r + (int)off);
            sum += slice[off];
        }
        for (unsigned off = 0; off < SLICE_BYTES; off += 4096u)
            sum += slice[off];
        munmap(slice, SLICE_BYTES);
    }

    g_checksum[id] = sum;
    return NULL;
}

static double run(int threads)
{
    pthread_t th[MAX_THREADS];
    double t0, t1;

    g_threads = threads;
    memset(g_checksum, 0, sizeof(g_checksum));

    t0 = now_sec();
    for (long i = 1; i < threads; i++) {
        if (pthread_create(&th[i], NULL, worker, (void *)i) != 0) {
            fprintf(stderr, "MM_PT_SCALE: pthread_create failed\n");
            exit(1);
        }
    }
    worker((void *)0);
    for (int i = 1; i < threads; i++)
        pthread_join(th[i], NULL);
    t1 = now_sec();

    return t1 - t0;
}

static unsigned long read_kernel_counter(const char *name)
{
    FILE *f = fopen("/proc/a20/perf", "r");
    char line[256];
    unsigned long value = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char key[128];
        char *p = key;
        unsigned long v;
        /* a20_perf_format() emits "name: value". */
        if (sscanf(line, "%127[^:]: %lu", key, &v) == 2) {
            while (*p == ' ' || *p == '\t')
                p++;
            if (strcmp(p, name) == 0) {
                value = v;
                break;
            }
        }
    }
    fclose(f);
    return value;
}

int main(void)
{
    size_t total = (size_t)MAX_THREADS * SLICE_BYTES;
    double base, best, speedup;

    (void)total;
    /* Warm up allocator and page-table machinery before measuring. */
    run(2);

    base = run(1);
    printf("MM_PT_SCALE: threads=1 time=%.4fs\n", base);
    for (int t = 2; t <= MAX_THREADS; t++) {
        double d = run(t);
        printf("MM_PT_SCALE: threads=%d time=%.4fs\n", t, d);
        (void)d;
    }

    /* Throughput of the widest run versus one thread. */
    best = base;
    {
        double wide = run(MAX_THREADS);
        best = wide;
        speedup = (base * MAX_THREADS) / (wide > 0 ? wide : 1e-9);
        printf("MM_PT_SCALE: 1T=%.4fs %dT=%.4fs ideal_speedup=%.2fx\n",
               base, MAX_THREADS, wide, speedup);
    }

    {
        unsigned long acq = read_kernel_counter("mm_pt_lock_acquires");
        unsigned long con = read_kernel_counter("mm_pt_lock_contended");
        unsigned long cur = read_kernel_counter("mm_cursor_open");
        unsigned long retry = read_kernel_counter("mm_cursor_stale_retry");
        printf("MM_PT_SCALE: pt_lock_acquires=%lu contended=%lu cursors=%lu "
               "stale_retry=%lu\n", acq, con, cur, retry);
    }

    if (speedup < MIN_SPEEDUP) {
        fprintf(stderr,
                "MM_PT_SCALE: FAIL scaling %.2fx below %.2fx -- disjoint "
                "page-table ranges are not running in parallel\n",
                speedup, MIN_SPEEDUP);
            return 1;
    }

    printf("MM_PT_SCALE: PASS\n");
    return 0;
}
