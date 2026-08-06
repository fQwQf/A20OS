/*
 * clock_bench — verify and measure the vDSO time path
 * (docs/hybrid-kernel/01-roadmap.md phase 3B).
 *
 * Compares musl clock_gettime (served by the vDSO when AT_SYSINFO_EHDR is
 * present) against the raw syscall in the same process, and cross-checks
 * the two results for sanity (diff < 1 s, monotonic non-decreasing).
 */
#define _GNU_SOURCE
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/auxv.h>

static uint64_t ts_ns(const struct timespec *t)
{
    return (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec;
}

/* Exercise demand stack growth well beyond the 128 KiB initial mapping, then
 * call the vDSO from the expanded stack.  The old fixed vDSO layout lived only
 * about 256 KiB below the stack top and collided with real Rust workloads. */
__attribute__((noinline)) static int stack_growth_vdso_probe(void)
{
    volatile unsigned char span[384 * 1024];
    struct timespec ts;
    unsigned int checksum = 0;

    for (size_t i = 0; i < sizeof(span); i += 4096) {
        span[i] = (unsigned char)(i / 4096);
        checksum += span[i];
    }
    return clock_gettime(CLOCK_MONOTONIC, &ts) == 0 &&
           ts.tv_sec >= 0 && checksum != 0;
}

int main(void)
{
    printf("CLOCK_BENCH: auxv_ehdr=0x%lx\n", (unsigned long)getauxval(33));
    enum { N = 100000 };
    struct timespec a, b, ts;

    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < N; i++)
        clock_gettime(CLOCK_MONOTONIC, &ts);
    clock_gettime(CLOCK_MONOTONIC, &b);
    uint64_t libc_ns = ts_ns(&b) - ts_ns(&a);

    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < N; i++)
        syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    clock_gettime(CLOCK_MONOTONIC, &b);
    uint64_t sys_ns = ts_ns(&b) - ts_ns(&a);

    /* Cross-check the vDSO value against the kernel syscall value. */
    struct timespec x, y;
    clock_gettime(CLOCK_REALTIME, &x);
    syscall(SYS_clock_gettime, CLOCK_REALTIME, &y);
    uint64_t dx = ts_ns(&x), dy = ts_ns(&y);
    uint64_t diff = dx > dy ? dx - dy : dy - dx;

    int mono_ok = 1;
    struct timespec prev, cur;
    clock_gettime(CLOCK_MONOTONIC, &prev);
    for (int i = 0; i < 1000; i++) {
        clock_gettime(CLOCK_MONOTONIC, &cur);
        if (ts_ns(&cur) < ts_ns(&prev))
            mono_ok = 0;
        prev = cur;
    }

    struct timeval tv;
    gettimeofday(&tv, 0);
    int stack_growth_ok = stack_growth_vdso_probe();

    printf("CLOCK_BENCH: libc_ns_per_call=%llu sys_ns_per_call=%llu diff_ns=%llu mono_ok=%d stack_growth_ok=%d gtod_sec=%llu\n",
           (unsigned long long)(libc_ns / N),
           (unsigned long long)(sys_ns / N),
           (unsigned long long)diff, mono_ok, stack_growth_ok,
           (unsigned long long)tv.tv_sec);
    printf("CLOCK_BENCH: %s\n",
           (diff < 1000000000ull && mono_ok && stack_growth_ok) ? "PASS" : "FAIL");
    return 0;
}
