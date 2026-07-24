#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define TOTAL_ITERATIONS 120000000ULL
#define MAX_WORKERS 32

static uint64_t mix(uint64_t value)
{
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    return value * 2685821657736338717ULL;
}

static uint64_t elapsed_ns(const struct timespec *start,
                           const struct timespec *end)
{
    return (uint64_t)(end->tv_sec - start->tv_sec) * 1000000000ULL +
           (uint64_t)end->tv_nsec - (uint64_t)start->tv_nsec;
}

int main(int argc, char **argv)
{
    int workers = argc > 1 ? atoi(argv[1]) : 1;
    if (workers < 1 || workers > MAX_WORKERS) {
        fprintf(stderr, "usage: smp_bench [workers:1-%d]\n", MAX_WORKERS);
        return 2;
    }

    int result_pipe[2];
    if (pipe(result_pipe) != 0) {
        perror("pipe");
        return 1;
    }

    uint64_t per_worker = TOTAL_ITERATIONS / (uint64_t)workers;
    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int worker = 0; worker < workers; worker++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            close(result_pipe[0]);

            uint64_t value = 0x9e3779b97f4a7c15ULL ^ (uint64_t)worker;
            for (uint64_t i = 0; i < per_worker; i++)
                value = mix(value + i);
            unsigned cpu_value = 0;
            if (syscall(SYS_getcpu, &cpu_value, NULL, NULL) < 0)
                _exit(1);
            unsigned char cpu = (unsigned char)cpu_value;
            if (write(result_pipe[1], &cpu, 1) != 1)
                _exit(1);
            _exit((int)(value & 0x7f));
        }
    }

    close(result_pipe[1]);

    int failures = 0;
    unsigned cpu_counts[MAX_WORKERS] = {0};
    for (int i = 0; i < workers; i++) {
        unsigned char cpu;
        if (read(result_pipe[0], &cpu, 1) != 1 || cpu >= MAX_WORKERS)
            failures++;
        else
            cpu_counts[cpu]++;
    }
    close(result_pipe[0]);
    for (int i = 0; i < workers; i++) {
        if (wait(NULL) < 0) {
            perror("wait");
            failures++;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    uint64_t ns = elapsed_ns(&start, &end);
    uint64_t ops_per_sec = ns ? TOTAL_ITERATIONS * 1000000000ULL / ns : 0;
    printf("SMP_BENCH workers=%d elapsed_ns=%llu ops_per_sec=%llu status=%s\n",
           workers, (unsigned long long)ns, (unsigned long long)ops_per_sec,
           failures ? "FAIL" : "PASS");
    printf("SMP_BENCH cpus");
    for (int cpu = 0; cpu < MAX_WORKERS; cpu++) {
        if (cpu_counts[cpu])
            printf(" %d:%u", cpu, cpu_counts[cpu]);
    }
    printf("\n");
    return failures ? 1 : 0;
}
