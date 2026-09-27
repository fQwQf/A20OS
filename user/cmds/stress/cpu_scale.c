/*
 * cpu_scale -- environment probe, not a kernel test.
 *
 * Pure integer work in N threads with no shared memory.  Its only purpose is
 * to establish whether the execution environment can actually run guest
 * vCPUs concurrently: if this does not scale, then no memory-management
 * change can ever show a speedup here and a poor mm_pt_scale result says
 * nothing about the kernel.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define SPIN_ITERS 60000000L
#define MAXN       4

static volatile uint64_t sink;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *spin(void *arg)
{
    uint64_t x = (uint64_t)(uintptr_t)arg + 1;
    for (long i = 0; i < SPIN_ITERS; i++)
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    sink = x;
    return NULL;
}

int main(void)
{
    pthread_t th[MAXN];
    double base;

    spin((void *)0);
    base = now_sec(); spin((void *)0); base = now_sec() - base;

    printf("CPU_SCALE: 1T=%.4fs\n", base);
    for (int n = 2; n <= MAXN; n++) {
        double t0 = now_sec();
        for (int i = 0; i < n; i++)
            pthread_create(&th[i], NULL, spin, (void *)(uintptr_t)i);
        for (int i = 0; i < n; i++)
            pthread_join(th[i], NULL);
        double d = now_sec() - t0;
        printf("CPU_SCALE: %dT=%.4fs speedup=%.2fx\n", n, d,
               (base * n) / (d > 0 ? d : 1e-9));
    }
    return 0;
}
