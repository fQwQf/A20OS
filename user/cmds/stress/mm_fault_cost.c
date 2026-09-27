/*
 * mm_fault_cost -- attribute the demand-fault serialisation.
 *
 * mm_pt_scale measures the end-to-end number (a shared address space, N
 * threads faulting disjoint ranges).  That number alone cannot say *what* is
 * serialising, and the workload mixes three very different costs:
 *
 *   A. mmap/munmap      -- takes mm->lock for gap search + VMA insert
 *   B. the fault path   -- mm->lock, VMA lookup, buddy alloc, page zeroing
 *   C. the memory path  -- once pages are mapped, plain stores (no faults)
 *
 * This benchmark runs each in isolation with the same thread count so the
 * shortfall against the pure-compute ceiling can be attributed:
 *
 *   scale(A) low  -> the address-space lock in mmap is the serialiser
 *   scale(B) low  -> the fault path is the serialiser (what P5 targets)
 *   scale(C) low  -> the memory system, not the MM code, is the limit
 *
 * All three use disjoint per-thread ranges in one shared address space, so
 * they differ only in which kernel work they exercise.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define THREADS      2
#define REGION       (4u * 1024u * 1024u)   /* per-thread region */
#define PAGES        (REGION / 4096u)
#define FAULT_ROUNDS 8
#define MEM_ROUNDS  4096
#define MMAP_ITERS  4000

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static unsigned long g_sink;

/* A: mmap/munmap only -- the address-space lock, no page is ever touched. */
static void *phase_mmap(void *arg)
{
    unsigned long sum = 0;
    for (int r = 0; r < MMAP_ITERS; r++) {
        unsigned char *p = mmap(NULL, REGION, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            continue;
        munmap(p, REGION);
        sum += (unsigned long)(uintptr_t)p;
    }
    g_sink += sum;
    return NULL;
}

/* B: one mmap, then first-touch every page -- the real demand-fault path. */
static void *phase_fault(void *arg)
{
    unsigned long sum = 0;
    for (int r = 0; r < FAULT_ROUNDS; r++) {
        unsigned char *p = mmap(NULL, REGION, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            continue;
        for (unsigned off = 0; off < REGION; off += 4096u) {
            p[off] = (unsigned char)off;
            sum += p[off];
        }
        munmap(p, REGION);
    }
    g_sink += sum;
    return NULL;
}

/* C: pre-faulted, then repeatedly written -- no faults, no mmap churn.
 * This is the memory-path ceiling for the same access pattern. */
static void *phase_mem(void *arg)
{
    unsigned long sum = 0;
    unsigned char *p = mmap(NULL, REGION, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    for (unsigned off = 0; off < REGION; off += 4096u)
        p[off] = (unsigned char)off;          /* pre-fault */
    for (int r = 0; r < MEM_ROUNDS; r++) {
        for (unsigned off = 0; off < REGION; off += 4096u) {
            p[off] = (unsigned char)(r + off);
            sum += p[off];
        }
    }
    munmap(p, REGION);
    g_sink += sum;
    return NULL;
}

typedef void *(*phase_fn)(void *);

static double run1(phase_fn f)
{
    double t0 = now_sec();
    f(NULL);
    return now_sec() - t0;
}

static double runN(phase_fn f, int n)
{
    pthread_t th[THREADS];
    double t0 = now_sec();
    for (int i = 0; i < n; i++)
        pthread_create(&th[i], NULL, f, NULL);
    for (int i = 0; i < n; i++)
        pthread_join(th[i], NULL);
    return now_sec() - t0;
}

static void report(const char *name, phase_fn f, int threads)
{
    double one = run1(f);
    double n = runN(f, threads);
    double scale = (one * threads) / (n > 0 ? n : 1e-9);
    printf("MM_FAULT_COST: %-6s 1T=%.4fs %dT=%.4fs scale=%.2fx\n",
           name, one, threads, n, scale);
}

int main(void)
{
    int t = THREADS;
    printf("MM_FAULT_COST: threads=%d region=%uKB\n", t, REGION / 1024);
    report("mmap", phase_mmap, t);
    report("fault", phase_fault, t);
    report("mem", phase_mem, t);
    printf("MM_FAULT_COST: PASS\n");
    return 0;
}
