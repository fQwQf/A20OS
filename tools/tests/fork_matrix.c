/* Identical Linux/A20 probe: report fork/wait separately from setup and total.
 * Fresh-map cases revoke page-table proofs before EACH measured first fork. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static void die(const char *s) {
    perror(s);
    exit(1);
}
static uint64_t now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        die("clock");
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void io(int fd, void *p, size_t n, int output) {
    while (n) {
        ssize_t r = output ? write(fd, p, n) : read(fd, p, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            die("pipe");
        p = (char *)p + r;
        n -= r;
    }
}
static void reap(pid_t p) {
    int s;
    if (p < 0 || waitpid(p, &s, 0) != p || !WIFEXITED(s) || WEXITSTATUS(s))
        die("child");
}
struct result {
    uint64_t fork_ns, wait_ns, setup_ns, total_ns;
};
static struct result work(const char *mode, int rounds, int scale, int pattern,
                          int ready, int gate) {
    int file = !strncmp(mode, "file", 4), fresh = !strcmp(mode, "file-first");
    int writable = !strcmp(mode, "file-rw"),
        sparse = !strcmp(mode, "anon-sparse");
    int shared = !strcmp(mode, "shared"),
        fragment = !strcmp(mode, "fragmented"), fds = !strcmp(mode, "fd");
    int burst = !strncmp(mode, "burst", 5), exec_child = !strcmp(mode, "exec");
    size_t len =
        (fds || burst || exec_child) ? 4096 : (size_t)scale * 1024 * 1024;
    int fd = -1, *extra = NULL;
    if (file) {
        char name[] = "/tmp/fork-matrix-XXXXXX";
        fd = mkstemp(name);
        if (fd < 0 || unlink(name) || ftruncate(fd, len))
            die("file");
    }
    if (fds) {
        struct rlimit limit = {65536, 65536};
        if (setrlimit(RLIMIT_NOFILE, &limit))
            die("fd limit");
        extra = calloc(scale, sizeof(int));
        if (!extra)
            die("fds");
        for (int i = 0; i < scale; i++)
            if ((extra[i] = open("/dev/null", O_RDONLY)) < 0)
                die("open fd");
    }
    int prot = file && !writable ? PROT_READ : PROT_READ | PROT_WRITE;
    int flags = file ? MAP_PRIVATE
                     : (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS;
    volatile unsigned char *map = mmap(NULL, len, prot, flags, fd, 0);
    if (map == MAP_FAILED)
        die("mmap");
    unsigned char value = file && !writable ? 0 : 7;
    if (!sparse)
        for (size_t i = 0; i < len; i += 4096) {
            if (value)
                map[i] = value;
            else if (map[i])
                die("file content");
        }
    if (fragment)
        for (size_t i = 0; i < len; i += 8192)
            if (mprotect((void *)(map + i), 4096, PROT_READ))
                die("fragment");
    if (fresh) {
        if (munmap((void *)map, len))
            die("fresh initial unmap");
        map = NULL;
    }
    if (ready >= 0) {
        char c = 0;
        io(ready, &c, 1, 1);
        io(gate, &c, 1, 0);
    }
    struct result r = {0};
    uint64_t total = now();
    for (int k = 0; k < rounds; k++) {
        if (burst) {
            pid_t children[128];
            uint64_t begin = now();
            for (int j = 0; j < scale; j++) {
                children[j] = fork();
                if (children[j] < 0)
                    die("burst fork");
                if (!children[j])
                    _exit(0);
            }
            uint64_t after = now();
            for (int j = 0; j < scale; j++) {
                if (!strcmp(mode, "burst-pid"))
                    reap(children[j]);
                else {
                    int status;
                    if (waitpid(-1, &status, 0) < 0 || !WIFEXITED(status) ||
                        WEXITSTATUS(status))
                        die("wait any");
                }
            }
            r.fork_ns += after - begin;
            r.wait_ns += now() - after;
            continue;
        }
        uint64_t setup = now();
        if (fresh) {
            map = mmap(NULL, len, prot, flags, fd, 0);
            if (map == MAP_FAILED)
                die("fresh mmap");
            for (size_t i = 0; i < len; i += 4096)
                if (map[i])
                    die("fresh content");
        }
        r.setup_ns += now() - setup;
        uint64_t begin = now();
        pid_t child = fork();
        if (!child) {
            if (exec_child) {
                execl("/bin/true", "true", (char *)NULL);
                _exit(127);
            }
            size_t stride = pattern == 2 ? 1024 * 1024 : 4096;
            unsigned char expected = sparse ? 0 : value;
            if (pattern)
                for (size_t i = 0; i < len; i += stride) {
                    if (map[i] != expected)
                        _exit(20);
                    if (pattern == 3)
                        map[i] = 9;
                }
            _exit(0);
        }
        uint64_t after = now();
        reap(child);
        uint64_t done = now();
        r.fork_ns += after - begin;
        r.wait_ns += done - after;
        if (pattern == 3) {
            unsigned char expected = shared ? 9 : value;
            if (map[0] != expected || map[len - 4096] != expected)
                die("COW isolation");
            if (shared)
                for (size_t i = 0; i < len; i += 4096)
                    map[i] = value;
        }
        if (fresh) {
            setup = now();
            if (munmap((void *)map, len))
                die("fresh unmap");
            r.setup_ns += now() - setup;
            map = NULL;
        }
    }
    r.total_ns = now() - total;
    if (map && munmap((void *)map, len))
        die("unmap");
    if (fd >= 0)
        close(fd);
    if (extra) {
        for (int i = 0; i < scale; i++)
            close(extra[i]);
        free(extra);
    }
    return r;
}
int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr,
                "usage: %s "
                "file|file-first|file-rw|anon|anon-sparse|shared|fragmented|fd "
                "rounds scale child-pattern workers\n",
                argv[0]);
        return 2;
    }
    alarm(90);
    const char *valid[] = {"file",        "file-first", "file-rw",    "anon",
                           "anon-sparse", "shared",     "fragmented", "fd",
                           "burst-any",   "burst-pid",  "exec"};
    int known = 0;
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
        known |= !strcmp(argv[1], valid[i]);
    if (!known)
        return 2;
    int n = atoi(argv[2]), scale = atoi(argv[3]), pattern = atoi(argv[4]),
        workers = atoi(argv[5]);
    if (n < 1 || scale < 1 || scale > 4096 || pattern < 0 || pattern > 3 ||
        workers < 1 || workers > 8)
        return 2;
    if (strcmp(argv[1], "fd") && scale > 256)
        return 2;
    if (!strncmp(argv[1], "burst", 5) && (scale > 128 || pattern))
        return 2;
    if (pattern == 3 &&
        (!strcmp(argv[1], "file") || !strcmp(argv[1], "file-first") ||
         !strcmp(argv[1], "fragmented") || !strcmp(argv[1], "anon-sparse")))
        return 2;
    struct result sum = {0};
    uint64_t wall = 0;
    if (workers == 1)
        sum = work(argv[1], n, scale, pattern, -1, -1);
    else {
        int ready[2], gate[2], results[2];
        pid_t children[8];
        if (pipe(ready) || pipe(gate) || pipe(results))
            die("pipe");
        for (int w = 0; w < workers; w++) {
            children[w] = fork();
            if (children[w] < 0)
                die("worker fork");
            if (!children[w]) {
                close(ready[0]);
                close(gate[1]);
                close(results[0]);
                struct result r =
                    work(argv[1], n, scale, pattern, ready[1], gate[0]);
                io(results[1], &r, sizeof(r), 1);
                _exit(0);
            }
        }
        close(ready[1]);
        close(gate[0]);
        close(results[1]);
        char tokens[8] = {0};
        io(ready[0], tokens, workers, 0);
        wall = now();
        io(gate[1], tokens, workers, 1);
        for (int w = 0; w < workers; w++) {
            struct result r;
            io(results[0], &r, sizeof(r), 0);
            sum.fork_ns += r.fork_ns;
            sum.wait_ns += r.wait_ns;
            sum.setup_ns += r.setup_ns;
            sum.total_ns += r.total_ns;
        }
        for (int w = 0; w < workers; w++)
            reap(children[w]);
        wall = now() - wall;
    }
    if (!wall)
        wall = sum.total_ns;
    if (!strncmp(argv[1], "burst", 5))
        n *= scale;
    printf("mode=%s workers=%d n=%d scale=%d pattern=%d ns_per_op=%.3f "
           "fork_ns=%llu wait_ns=%llu setup_ns=%llu total_ns=%llu "
           "throughput=%.3f\n",
           argv[1], workers, n * workers, scale, pattern,
           (double)(sum.fork_ns + sum.wait_ns) / (n * workers),
           (unsigned long long)sum.fork_ns, (unsigned long long)sum.wait_ns,
           (unsigned long long)sum.setup_ns, (unsigned long long)sum.total_ns,
           (double)n * workers * 1e9 / wall);
    return 0;
}
