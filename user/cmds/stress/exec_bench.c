/*
 * Paired fork+execve+wait micro-benchmark for the exec-replacement path.
 *
 * The per-exec cost a selector adds is tiny next to fork/scheduler/demand-fault
 * cost, and the emulated target is noisy between boots.  So the benchmark runs
 * several rounds of `iters` execs, takes each round's median, and reports the
 * best round median: the round least disturbed by host or boot transients.
 * The A/B harness compares that number across alternating C/R selector builds
 * with the same userland, target, SMP and warmup.
 *
 *   exec_bench [iters] [rounds] [target]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static unsigned long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned)ts.tv_nsec;
}

static int cmp_ull(const void *a, const void *b)
{
    unsigned long long x = *(const unsigned long long *)a;
    unsigned long long y = *(const unsigned long long *)b;
    return (x > y) - (x < y);
}

static int run_one(const char *target)
{
    char *argv[] = { (char *)target, NULL };
    char *envp[] = { NULL };
    unsigned long long t0 = now_ns();
    pid_t pid = fork();
    if (pid == 0) {
        execve(target, argv, envp);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    return (int)(now_ns() - t0);
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 200;
    int rounds = argc > 2 ? atoi(argv[2]) : 5;
    const char *target = argc > 3 ? argv[3] : "/bin/true";
    if (iters <= 0 || iters > 100000)
        iters = 200;
    if (rounds <= 0 || rounds > 64)
        rounds = 5;

    unsigned long long *samples = malloc(sizeof(*samples) * (size_t)iters);
    if (!samples) {
        printf("EXEC_BENCH: FAIL nomem\n");
        return 1;
    }

    for (int i = 0; i < 32; i++) {
        if (run_one(target) < 0) {
            printf("EXEC_BENCH: FAIL warmup\n");
            free(samples);
            return 1;
        }
    }

    unsigned long long best_median = 0, best_mean = 0, best_min = 0;
    for (int r = 0; r < rounds; r++) {
        unsigned long long sum = 0;
        for (int i = 0; i < iters; i++) {
            int d = run_one(target);
            if (d < 0) {
                printf("EXEC_BENCH: FAIL round=%d iter=%d\n", r, i);
                free(samples);
                return 1;
            }
            samples[i] = (unsigned long long)d;
            sum += (unsigned long long)d;
        }
        qsort(samples, (size_t)iters, sizeof(*samples), cmp_ull);
        unsigned long long median = samples[iters / 2];
        printf("EXEC_BENCH_ROUND: round=%d mean_ns=%llu median_ns=%llu "
               "min_ns=%llu\n", r, sum / (unsigned long long)iters, median,
               samples[0]);
        if (r == 0 || median < best_median) {
            best_median = median;
            best_mean = sum / (unsigned long long)iters;
            best_min = samples[0];
        }
    }

    printf("EXEC_BENCH: iters=%d rounds=%d best_median_ns=%llu "
           "that_mean_ns=%llu that_min_ns=%llu target=%s\n",
           iters, rounds, best_median, best_mean, best_min, target);
    free(samples);
    return 0;
}
