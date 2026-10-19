/*
 * Self-exec chain: isolates the exec-replacement cost from fork/scheduler.
 *
 * Fork+exec+wait benchmarks are dominated by fork and demand faulting, which
 * swamps the loader decision delta.  This program execs *itself* `total` times
 * in a single process (no fork, no wait), carrying the start timestamp in argv,
 * and prints the mean per-exec time.  The A/B harness runs it on the C and Rust
 * selector builds with the same userland and target.
 *
 *   exec_chain <remaining> <start_ns> <total>     (internally)
 *   exec_chain 2000 0 0                            (entry point)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static unsigned long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned)ts.tv_nsec;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("EXEC_CHAIN: bad argv\n");
        return 2;
    }
    long remaining = atol(argv[1]);
    unsigned long long start = strtoull(argv[2], NULL, 10);
    long total = atol(argv[3]);

    if (remaining <= 0) {
        unsigned long long end = now_ns();
        long n = total > 0 ? total : 1;
        printf("EXEC_CHAIN: total=%ld per_exec_ns=%llu elapsed_ns=%llu\n",
               total, (end - start) / (unsigned long long)n, end - start);
        fflush(stdout);
        return 0;
    }
    if (start == 0) {
        start = now_ns();
        total = remaining;
    }

    char rbuf[24], sbuf[24], tbuf[24];
    snprintf(rbuf, sizeof(rbuf), "%ld", remaining - 1);
    snprintf(sbuf, sizeof(sbuf), "%llu", start);
    snprintf(tbuf, sizeof(tbuf), "%ld", total);

    char *cargv[] = { "/bin/exec_chain", rbuf, sbuf, tbuf, NULL };
    execve("/bin/exec_chain", cargv, environ);
    printf("EXEC_CHAIN: FAIL execve errno=%d\n", errno);
    return 1;
}
