/*
 * cortenmm_bench -- the five microbenchmarks from the CortenMM paper (§6.2),
 * so the port can be measured against the numbers the design actually claims.
 *
 * The paper reports, for CortenMMadv vs Linux on a single thread, a 7.8% to
 * 46.8% win on four of the five, and attributes the difference to "the time
 * Linux spends in the VMA".  It also reports that Linux loses on unmap-virt
 * because that operation must split VMA-tree nodes, whereas removing a page
 * table is cheaper.  Neither claim is checkable with a single benchmark, which
 * is why mm_pt_scale alone showed nothing: it only covers PF.
 *
 * Each benchmark runs in two contention variants, as the paper does:
 *
 *   low   -- every thread works on a private, disjoint region.  This is the
 *            case where a correct transactional interface should scale nearly
 *            linearly, because disjoint regions must not serialise.
 *   high  -- every thread works on a random region inside one large shared
 *            region.  Contention for the last-level PT page is expected here
 *            even in a correct implementation.
 *
 * Methodology notes, learned the hard way while measuring this port:
 *
 *   - Report single-thread throughput per phase as the primary number.  The
 *     paper's headline single-thread claim is a per-operation cost, and it is
 *     the only comparison that fits inside a 4-vCPU KVM guest; the paper's
 *     33x-2270x figures are at 384 cores and cannot be reproduced here.
 *   - Do not derive a speedup ratio from a separately timed single-thread
 *     run.  That baseline drifts by ~17% run to run and the ratio inherits
 *     the whole drift, which manufactures convincing but fictional numbers.
 *     Timed regions for 1T and NT are taken back to back in one process.
 *   - Prime /proc/a20/perf before the timed region.  perf counters are
 *     dormant until the first read, so a post-hoc read discards the whole
 *     benchmark (see docs/roadmap §8.20).
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS 64
#define POOL_MB     512
#define SLICE_MB    2
#define ROUNDS      256      /* default; overridable as argv[3] */

/* One benchmark selected by argv[1]; both variants always run. */
enum bench { B_MMAP, B_MMAP_PF, B_PF, B_UNMAP_VIRT, B_UNMAP, B_MAX };

static int   g_threads = 4;
static int   g_rounds  = ROUNDS;
static int   g_bench;
static int   g_high_contention;

static unsigned char *g_pool;      /* one large shared region */
static size_t         g_pool_len;
static unsigned char *g_priv[MAX_THREADS]; /* per-thread slice for low mode */
static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static const char *bench_name(int b)
{
    switch (b) {
    case B_MMAP:       return "mmap";
    case B_MMAP_PF:    return "mmap-PF";
    case B_PF:         return "PF";
    case B_UNMAP_VIRT: return "unmap-virt";
    case B_UNMAP:      return "unmap";
    default:           return "?";
    }
}

/* Pick the region this thread should work on for the current round. */
static unsigned char *pick_region(int id, int round)
{
    if (!g_high_contention)
        return g_priv[id];
    pthread_mutex_lock(&g_pool_mu);
    size_t off = ((size_t)(round * 7919 + id * 104729) %
                  (g_pool_len - SLICE_MB * 1024u)) & ~((size_t)SLICE_MB * 1024u - 1);
    unsigned char *p = g_pool + off;
    pthread_mutex_unlock(&g_pool_mu);
    return p;
}

static void *worker(void *arg)
{
    long id = (long)arg;

    for (int r = 0; r < g_rounds; r++) {
        switch (g_bench) {
        case B_MMAP: {
            /* Map without touching: measures descriptor/VMA setup only. */
            void *p = mmap(NULL, SLICE_MB * 1024u, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p != MAP_FAILED)
                munmap(p, SLICE_MB * 1024u);
            break;
        }
        case B_MMAP_PF: {
            /* Map and touch one page: the paper's mmap-plus-first-fault. */
            void *p = mmap(NULL, SLICE_MB * 1024u, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p != MAP_FAILED) {
                ((volatile unsigned char *)p)[0] = 1;
                munmap(p, SLICE_MB * 1024u);
            }
            break;
        }
        case B_PF: {
            /* Fault on a pre-existing mapping: page fault cost alone.
             *
             * MADV_DONTNEED drops the page table entries but keeps the VMA
             * (mm_madvise_dontneed only validates VMAs then unmaps PTEs), so
             * every page below faults cold while the mmap cost stays out of
             * the measurement.  Re-touching a fixed slice instead would have
             * measured memcpy from round 1 on -- the warm-page trap mm_pt_scale
             * warns about, and the reason the old baseline read 0.0014s for
             * 1T, which is ~93M pages/s and physically impossible for real
             * faults.  The ANON_VIRT reservation survives the discard, so
             * these faults still take the VMA-free path, as they should. */
            unsigned char *base = pick_region(id, r);
            size_t len = SLICE_MB * 1024u;
            madvise(base, len, MADV_DONTNEED);
            for (size_t off = 0; off < len; off += 4096)
                base[off] = (unsigned char)(off + r);
            break;
        }
        case B_UNMAP_VIRT: {
            /* Unmap a hole in the middle: Linux must split the VMA, and the
             * paper's claim is that dropping the page table is cheaper. */
            unsigned char *base = pick_region(id, r);
            size_t len = SLICE_MB * 1024u;
            size_t a = len / 4, b = a + len / 4;
            /* Populate the hole first.  Without this the unmap frees no pages
             * at all, so the phase measured a VMA split over an untouched
             * range rather than the teardown of a populated one -- and the
             * "next round still faults" claim below was simply false. */
            for (size_t off = 0; off < len; off += 4096)
                base[off] = (unsigned char)(off + r);
            munmap(base + a, b - a);
            /* Put it back so the next round still faults. */
            void *p = mmap(base + a, b - a, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            (void)p;
            break;
        }
        case B_UNMAP: {
            /* Unmap a whole range: no VMA splitting required. */
            unsigned char *base = pick_region(id, r);
            size_t len = SLICE_MB * 1024u;
            void *p = mmap(base, len, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            (void)p;
            for (size_t off = 0; off < len; off += 4096)
                base[off] = (unsigned char)r;
            munmap(base, len);
            /* Restore the mapping so the shared pool stays faultable. */
            void *q = mmap(base, len, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            (void)q;
            break;
        }
        default:
            break;
        }
    }
    return NULL;
}

/* Time `nt` threads over the whole benchmark, in one process. */
static double run_once(int nt, unsigned long *ops_out)
{
    pthread_t th[MAX_THREADS];
    double t0, t1;

    t0 = now_sec();
    for (long i = 0; i < nt; i++)
        pthread_create(&th[i], NULL, worker, (void *)i);
    for (int i = 0; i < nt; i++)
        pthread_join(th[i], NULL);
    t1 = now_sec();

    *ops_out = (unsigned long)nt * (unsigned long)g_rounds;
    return t1 - t0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s {mmap|mmap-pf|pf|unmap-virt|unmap|all} [threads] [rounds]\n",
                argv[0]);
        return 2;
    }

    if (!strcmp(argv[1], "mmap"))            g_bench = B_MMAP;
    else if (!strcmp(argv[1], "mmap-pf"))    g_bench = B_MMAP_PF;
    else if (!strcmp(argv[1], "pf"))         g_bench = B_PF;
    else if (!strcmp(argv[1], "unmap-virt")) g_bench = B_UNMAP_VIRT;
    else if (!strcmp(argv[1], "unmap"))      g_bench = B_UNMAP;
    else if (!strcmp(argv[1], "all"))        g_bench = -1;
    else { fprintf(stderr, "unknown benchmark: %s\n", argv[1]); return 2; }

    if (argc >= 4)
        g_rounds = atoi(argv[3]);
    if (g_rounds < 1)
        g_rounds = 1;

    if (argc >= 3) {
        g_threads = atoi(argv[2]);
        if (g_threads < 1) g_threads = 1;
        if (g_threads > MAX_THREADS) g_threads = MAX_THREADS;
    }

    g_pool_len = (size_t)POOL_MB * 1024u * 1024u;
    g_pool = mmap(NULL, g_pool_len, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_pool == MAP_FAILED) {
        fprintf(stderr, "pool mmap failed\n");
        return 1;
    }
    for (int i = 0; i < g_threads; i++) {
        g_priv[i] = g_pool + (size_t)i * SLICE_MB * 1024u;
    }

    int first = (g_bench < 0) ? 0 : g_bench;
    int last  = (g_bench < 0) ? B_MAX - 1 : g_bench;
    int rc = 0;

    for (int b = first; b <= last; b++) {
        g_bench = b;
        for (int high = 0; high <= 1; high++) {
            g_high_contention = high;

            /*
             * Prime the perf counters so a later read reflects this
             * benchmark rather than being discarded (see §8.20).
             */
            system("cat /proc/a20/perf > /dev/null 2>&1");

            /* 1T and NT are timed back to back here, so the comparison
             * cannot inherit drift from a separately timed baseline. */
            unsigned long ops1 = 0, opsN = 0;
            double t1  = run_once(1, &ops1);
            double tN  = run_once(g_threads, &opsN);

            double thr1 = (t1 > 0) ? (double)ops1 / t1 : 0.0;
            double thrN = (tN > 0) ? (double)opsN / tN : 0.0;
            /* Throughput ratio: NT threads doing NT times the work. */
            double scaling = (thr1 > 0) ? thrN / thr1 : 0.0;
            /* Per-operation cost at N threads, the paper's single-thread metric
             * generalised: ns per operation, normalised by thread count so
             * the value is comparable to the paper's per-op cost. */
            double ns_per_op = (tN > 0)
                ? (tN * 1e9) / ((double)opsN) : 0.0;

            printf("CORTENMM_BENCH: %-11s %-4s threads=%d "
                   "1T=%.4fs %.1f ops/s  %dT=%.4fs %.1f ops/s "
                   "scaling=%.2fx ns_per_op=%.0f\n",
                   bench_name(b), high ? "high" : "low", g_threads,
                   t1, thr1, g_threads, tN, thrN, scaling, ns_per_op);
            fflush(stdout);
        }
    }

    munmap(g_pool, g_pool_len);
    return rc;
}
