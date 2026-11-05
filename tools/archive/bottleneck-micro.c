/* Controlled diagnostic workloads; execute the same static ELF on both OSes. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *mode = argv[1];
    unsigned n = strtoul(argv[2], NULL, 10);
    unsigned mb = argc > 3 ? strtoul(argv[3], NULL, 10) : 0;
    volatile unsigned char *mapping = NULL;
    if (mb) {
        mapping = mmap(NULL, (size_t)mb << 20, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED) { perror("mmap"); return 1; }
        /* "sparse" reserves address space; "touched" populates every page. */
        if (strstr(mode, "touched"))
            for (size_t i = 0; i < ((size_t)mb << 20); i += 4096) mapping[i] = 1;
        if (strstr(mode, "dontfork") && madvise((void *)mapping, (size_t)mb << 20, MADV_DONTFORK)) {
            perror("madvise DONTFORK"); return 1;
        }
    }
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    int futex = 0;
    uint64_t fork_ns = 0, wait_ns = 0, start = now();
    for (unsigned i = 0; i < n; i++) {
        if (!strcmp(mode, "clock")) {
            (void)now();
        } else if (!strcmp(mode, "yield")) {
            if (sched_yield()) return 1;
        } else if (!strcmp(mode, "getpid")) {
            if (syscall(SYS_getpid) <= 0) return 1;
        } else if (!strcmp(mode, "futex-wake")) {
            long r = syscall(SYS_futex, &futex, 1 | 128, 1, NULL, NULL, 0);
            if (r != 0) { perror("futex wake"); return 1; }
        } else if (!strcmp(mode, "futex-mismatch")) {
            long r = syscall(SYS_futex, &futex, 0 | 128, 1, NULL, NULL, 0);
            if (r != -1 || errno != EAGAIN) { perror("futex mismatch"); return 1; }
        } else if (!strcmp(mode, "mutex")) {
            if (pthread_mutex_lock(&mutex) || pthread_mutex_unlock(&mutex)) return 1;
        } else if (!strncmp(mode, "fork", 4)) {
            uint64_t begin = now();
            pid_t p = fork();
            if (p < 0) { perror("fork"); return 1; }
            if (!p) _exit(0);
            uint64_t created = now();
            int status = 0;
            if (waitpid(p, &status, 0) != p || !WIFEXITED(status) || WEXITSTATUS(status)) {
                perror("waitpid"); return 1;
            }
            fork_ns += created - begin;
            wait_ns += now() - created;
        } else return 2;
    }
    uint64_t elapsed = now() - start;
    printf("MICRO mode=%s n=%u mb=%u elapsed_ns=%llu ns_per_op=%.2f fork_ns=%llu wait_ns=%llu\n",
           mode, n, mb, (unsigned long long)elapsed, (double)elapsed / n,
           (unsigned long long)fork_ns, (unsigned long long)wait_ns);
    if (mapping) munmap((void *)mapping, (size_t)mb << 20);
    return 0;
}
