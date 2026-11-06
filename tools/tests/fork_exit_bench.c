/* Static musl workload shared verbatim by the Linux/A20 benchmark guests. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static void die(const char *s) { perror(s); exit(1); }
static uint64_t now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) die("clock");
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s file|shared-sparse|shared-dense rounds MiB child-read (0=none, 1=all, 2=1MiB-stride, 3=first-page)\n", argv[0]); return 2; }
    int file = !strcmp(argv[1], "file"), sparse = !strcmp(argv[1], "shared-sparse");
    if (!file && !sparse && strcmp(argv[1], "shared-dense")) return 2;
    int n = atoi(argv[2]), read_child = atoi(argv[4]);
    size_t len = strtoul(argv[3], NULL, 10) * 1024 * 1024;
    if (n <= 0 || read_child < 0 || read_child > 3 || !len || len > 512ul * 1024 * 1024) return 2;
    int fd = -1;
    if (file) {
        char name[] = "/tmp/a20-fork-bench-XXXXXX";
        fd = mkstemp(name);
        if (fd < 0 || unlink(name) || ftruncate(fd, len)) die("file");
    }
    volatile unsigned char *p = mmap(NULL, len, file ? PROT_READ : PROT_READ | PROT_WRITE,
        file ? MAP_PRIVATE : MAP_SHARED | MAP_ANONYMOUS, fd, 0);
    if (p == MAP_FAILED) die("mmap");
    if (file) close(fd);
    size_t read_stride = read_child == 2 ? 1024 * 1024 : 4096;
    size_t read_len = read_child == 3 ? 4096 : len;
    unsigned char expected = 0;
    if (!sparse) for (size_t i = 0; i < len; i += 4096) {
        if (!file) p[i] = (unsigned char)(i / 4096 % 251);
        unsigned char value = p[i]; /* Prefault every parent page. */
        if (i < read_len && i % read_stride == 0) expected ^= value;
    }
    uint64_t fork_ns = 0, wait_ns = 0, begin = now();
    for (int i = 0; i < n; i++) {
        uint64_t before = now();
        pid_t child = fork();
        if (!child) {
            unsigned char got = 0;
            if (read_child) for (size_t j = 0; j < read_len; j += read_stride) got ^= p[j];
            _exit(read_child && got != expected ? 7 : 0);
        }
        uint64_t fork_end = now();
        int status;
        if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) die("fork/wait");
        fork_ns += fork_end - before; wait_ns += now() - fork_end;
    }
    uint64_t elapsed = now() - begin;
    printf("mode=%s child_read=%d n=%d bytes=%zu ns_per_op=%.3f fork_ns=%llu wait_ns=%llu\n",
        argv[1], read_child, n, len, (double)elapsed/n,
        (unsigned long long)fork_ns, (unsigned long long)wait_ns);
    if (munmap((void *)p, len)) die("munmap");
    return 0;
}
