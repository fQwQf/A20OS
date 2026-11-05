/* Same static musl ELF on Linux/A20OS. Exercise fork's brk clone range,
 * including large untouched heaps; do not include brk growth/restore in time. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock"); exit(1); }
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    char *end;
    unsigned long bytes = strtoul(argv[1], &end, 10);
    if (*end || bytes > 512UL * 1024 * 1024 || bytes % 4096) return 2;
    unsigned long iterations = strtoul(argv[2], &end, 10);
    if (*end || !iterations || iterations > 10000) return 2;
    int touched = !strcmp(argv[3], "touched");
    if (!touched && strcmp(argv[3], "untouched")) return 2;
    uintptr_t base = (uintptr_t)syscall(SYS_brk, 0);
    if (!base || base > UINTPTR_MAX - bytes ||
        (uintptr_t)syscall(SYS_brk, base + bytes) != base + bytes) {
        fprintf(stderr, "brk growth failed errno=%d\n", errno); return 1;
    }
    if (touched) {
        for (size_t i = 0; i < bytes; i += 4096)
            *(volatile unsigned char *)(base + i) = (unsigned char)(i / 4096 % 251 + 1);
    }
    uint64_t start = now_ns();
    for (unsigned long i = 0; i < iterations; i++) {
        pid_t child = fork();
        if (child < 0) { perror("fork"); return 1; }
        if (!child) {
            if (touched && bytes) *(volatile unsigned char *)base = 0x5a;
            _exit(0);
        }
        int status;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr, "wait/child failed\n"); return 1;
        }
        if (touched && bytes && *(volatile unsigned char *)base != 1) {
            fprintf(stderr, "parent COW isolation failed\n"); return 1;
        }
    }
    uint64_t elapsed = now_ns() - start;
    if (touched) {
        for (size_t i = 0; i < bytes; i += 4096) {
            if (*(volatile unsigned char *)(base + i) != (unsigned char)(i / 4096 % 251 + 1)) {
                fprintf(stderr, "parent page verification failed\n"); return 1;
            }
        }
    }
    if ((uintptr_t)syscall(SYS_brk, base) != base) {
        fprintf(stderr, "brk restore failed\n"); return 1;
    }
    printf("BRK_FORK bytes=%lu iterations=%lu touched=%d elapsed_ns=%llu ns_per_fork=%.2f verified=1\n",
           bytes, iterations, touched, (unsigned long long)elapsed, (double)elapsed / iterations);
    return 0;
}
