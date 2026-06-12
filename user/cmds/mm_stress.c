#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("MM_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static unsigned long read_page_cache_pinned(void)
{
    int fd = open("/proc/a20/page_cache", O_RDONLY);
    if (fd < 0)
        return (unsigned long)-1;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return (unsigned long)-1;
    buf[n] = '\0';
    const char *p = strstr(buf, "pinned:");
    if (!p)
        return (unsigned long)-1;
    return strtoul(p + 7, NULL, 10);
}

static int shared_file_writeback(void)
{
    const char *path = "/tmp/mm_stress_shared";
    unsigned long base_pins = read_page_cache_pinned();
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return fail("shared-open");

    char initial[4096];
    memset(initial, 'A', sizeof(initial));
    if (write(fd, initial, sizeof(initial)) != (ssize_t)sizeof(initial)) {
        close(fd);
        unlink(path);
        return fail("shared-write-initial");
    }

    char *mem = mmap(NULL, sizeof(initial), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        unlink(path);
        return fail("shared-mmap");
    }

    for (size_t i = 0; i < sizeof(initial); i++)
        mem[i] = (char)('0' + (i % 10));

    if (msync(mem, sizeof(initial), MS_SYNC) < 0) {
        munmap(mem, sizeof(initial));
        close(fd);
        unlink(path);
        return fail("shared-msync");
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        munmap(mem, sizeof(initial));
        close(fd);
        unlink(path);
        return fail("shared-lseek");
    }

    char readback[4096];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(readback)) {
        ssize_t n = read(fd, readback + total, sizeof(readback) - total);
        if (n < 0) {
            munmap(mem, sizeof(initial));
            close(fd);
            unlink(path);
            return fail("shared-read");
        }
        if (n == 0)
            break;
        total += n;
    }
    if (total != (ssize_t)sizeof(readback) ||
        memcmp(mem, readback, sizeof(readback)) != 0) {
        munmap(mem, sizeof(initial));
        close(fd);
        unlink(path);
        return fail("shared-compare");
    }

    if (munmap(mem, sizeof(initial)) < 0) {
        close(fd);
        unlink(path);
        return fail("shared-munmap");
    }
    if (read_page_cache_pinned() != base_pins) {
        close(fd);
        unlink(path);
        return fail("shared-pinned-leak");
    }
    close(fd);
    unlink(path);
    return 0;
}

static int anonymous_fault_and_unmap(void)
{
    char *mem = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return fail("anon-mmap");

    for (int i = 0; i < 8192; i += 512)
        mem[i] = (char)(i / 512 + 1);
    for (int i = 0; i < 8192; i += 512) {
        if (mem[i] != (char)(i / 512 + 1))
            return fail("anon-compare");
    }
    if (munmap(mem, 8192) < 0)
        return fail("anon-munmap");
    return 0;
}

static int fixed_noreplace_conflict(void)
{
    char *mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return fail("fixed-base-mmap");

    errno = 0;
    void *again = mmap(mem, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (again != MAP_FAILED) {
        munmap(again, 4096);
        munmap(mem, 4096);
        return fail("fixed-noreplace-overlap");
    }
    if (errno != EEXIST) {
        munmap(mem, 4096);
        return fail("fixed-noreplace-errno");
    }
    if (munmap(mem, 4096) < 0)
        return fail("fixed-munmap");
    return 0;
}

static int fork_cow_and_exit(void)
{
    char *mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return fail("cow-mmap");
    strcpy(mem, "parent-value");

    pid_t pid = fork();
    if (pid < 0)
        return fail("cow-fork");
    if (pid == 0) {
        if (strcmp(mem, "parent-value") != 0)
            _exit(2);
        strcpy(mem, "child-value");
        _exit(strcmp(mem, "child-value") == 0 ? 0 : 3);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return fail("cow-wait");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("cow-child-status");
    if (strcmp(mem, "parent-value") != 0)
        return fail("cow-parent-value");
    if (munmap(mem, 4096) < 0)
        return fail("cow-munmap");
    return 0;
}

static int shared_file_partial_munmap(void)
{
    const char *path = "/tmp/mm_stress_shared_partial";
    unsigned long base_pins = read_page_cache_pinned();
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return fail("partial-open");

    char buf[8192];
    memset(buf, 'X', sizeof(buf));
    if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
        close(fd);
        unlink(path);
        return fail("partial-write");
    }

    char *mem = mmap(NULL, sizeof(buf), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        unlink(path);
        return fail("partial-mmap");
    }

    memset(mem + 4096, 'Y', 4096);
    if (msync(mem, sizeof(buf), MS_SYNC) < 0) {
        munmap(mem, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("partial-msync");
    }

    if (munmap(mem, 4096) < 0) {
        munmap(mem + 4096, 4096);
        close(fd);
        unlink(path);
        return fail("partial-munmap-first");
    }

    if (memcmp(mem + 4096, "YYYYYYYY", 8) != 0) {
        munmap(mem + 4096, 4096);
        close(fd);
        unlink(path);
        return fail("partial-compare");
    }

    if (munmap(mem + 4096, 4096) < 0) {
        close(fd);
        unlink(path);
        return fail("partial-munmap-second");
    }
    if (read_page_cache_pinned() != base_pins) {
        close(fd);
        unlink(path);
        return fail("partial-pinned-leak");
    }
    close(fd);
    unlink(path);
    return 0;
}

static int shared_file_fork(void)
{
    const char *path = "/tmp/mm_stress_shared_fork";
    unsigned long base_pins = read_page_cache_pinned();
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return fail("fork-shared-open");

    char buf[4096];
    memset(buf, 'A', sizeof(buf));
    if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
        close(fd);
        unlink(path);
        return fail("fork-shared-write");
    }

    char *mem = mmap(NULL, sizeof(buf), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        unlink(path);
        return fail("fork-shared-mmap");
    }

    pid_t pid = fork();
    if (pid < 0) {
        munmap(mem, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("fork-shared-fork");
    }
    if (pid == 0) {
        for (size_t i = 0; i < sizeof(buf); i++)
            mem[i] = (char)('0' + (i % 10));
        if (msync(mem, sizeof(buf), MS_SYNC) < 0)
            _exit(2);
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        munmap(mem, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("fork-shared-wait");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        munmap(mem, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("fork-shared-child-status");
    }

    if (memcmp(mem, "01234567", 8) != 0) {
        munmap(mem, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("fork-shared-compare");
    }

    if (munmap(mem, sizeof(buf)) < 0) {
        close(fd);
        unlink(path);
        return fail("fork-shared-munmap");
    }
    if (read_page_cache_pinned() != base_pins) {
        close(fd);
        unlink(path);
        return fail("fork-shared-pinned-leak");
    }
    close(fd);
    unlink(path);
    return 0;
}

static int shared_file_mremap(void)
{
    const char *path = "/tmp/mm_stress_shared_mremap";
    unsigned long base_pins = read_page_cache_pinned();
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return fail("mremap-shared-open");

    char buf[8192];
    memset(buf, 'A', sizeof(buf));
    if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
        close(fd);
        unlink(path);
        return fail("mremap-shared-write");
    }

    char *mem = mmap(NULL, sizeof(buf), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        unlink(path);
        return fail("mremap-shared-mmap");
    }

    for (size_t i = 0; i < sizeof(buf); i++)
        mem[i] = (char)('0' + (i % 10));

    char *moved = mremap(mem, sizeof(buf), sizeof(buf), MREMAP_MAYMOVE);
    if (moved == MAP_FAILED) {
        munmap(mem, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("mremap-shared-move");
    }

    if (memcmp(moved, "01234567", 8) != 0 ||
        memcmp(moved + 4096, "67890123", 8) != 0) {
        munmap(moved, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("mremap-shared-compare");
    }

    moved[0] = 'Z';
    if (msync(moved, sizeof(buf), MS_SYNC) < 0) {
        munmap(moved, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("mremap-shared-msync");
    }

    if (munmap(moved, sizeof(buf)) < 0) {
        close(fd);
        unlink(path);
        return fail("mremap-shared-munmap");
    }
    if (read_page_cache_pinned() != base_pins) {
        close(fd);
        unlink(path);
        return fail("mremap-shared-pinned-leak");
    }
    close(fd);
    unlink(path);
    return 0;
}

static int shared_file_mremap_dontunmap(void)
{
    const char *path = "/tmp/mm_stress_shared_dontunmap";
    unsigned long base_pins = read_page_cache_pinned();
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return fail("dontunmap-shared-open");

    char buf[8192];
    memset(buf, 'A', sizeof(buf));
    if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
        close(fd);
        unlink(path);
        return fail("dontunmap-shared-write");
    }

    char *mem = mmap(NULL, sizeof(buf), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        unlink(path);
        return fail("dontunmap-shared-mmap");
    }

    for (size_t i = 0; i < sizeof(buf); i++)
        mem[i] = (char)('0' + (i % 10));

    char *moved = mremap(mem, sizeof(buf), sizeof(buf),
                         MREMAP_MAYMOVE | MREMAP_DONTUNMAP);
    if (moved == MAP_FAILED) {
        munmap(mem, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("dontunmap-shared-move");
    }

    if (moved == mem) {
        munmap(mem, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("dontunmap-shared-not-moved");
    }

    if (memcmp(moved, "01234567", 8) != 0 ||
        memcmp(moved + 4096, "67890123", 8) != 0) {
        munmap(moved, sizeof(buf));
        munmap(mem, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("dontunmap-shared-compare-moved");
    }

    moved[0] = 'Z';
    if (mem[0] != 'Z') {
        munmap(moved, sizeof(buf));
        munmap(mem, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("dontunmap-shared-coherence");
    }

    if (msync(moved, sizeof(buf), MS_SYNC) < 0) {
        munmap(moved, sizeof(buf));
        munmap(mem, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("dontunmap-shared-msync");
    }

    if (munmap(mem, sizeof(buf)) < 0) {
        munmap(moved, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("dontunmap-shared-munmap-src");
    }
    if (munmap(moved, sizeof(buf)) < 0) {
        close(fd);
        unlink(path);
        return fail("dontunmap-shared-munmap-dst");
    }
    if (read_page_cache_pinned() != base_pins) {
        close(fd);
        unlink(path);
        return fail("dontunmap-shared-pinned-leak");
    }
    close(fd);
    unlink(path);
    return 0;
}
static int shared_file_mremap_merge_guard(void)
{
    /* Regression: mm_mmap(MAP_ANONYMOUS) may merge the mremap destination with
     * an adjacent anonymous VMA. The kernel must split it back before
     * converting it to file-backed, otherwise the neighbor becomes file-backed
     * too and corrupts adjacent memory/file offsets. */
    const char *path = "/tmp/mm_stress_shared_merge";
    unsigned long base_pins = read_page_cache_pinned();
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return fail("merge-shared-open");

    char buf[4096];
    memset(buf, 'A', sizeof(buf));
    if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
        close(fd);
        unlink(path);
        return fail("merge-shared-write");
    }

    uintptr_t anon_addr = 0x10000000;
    char *anon = mmap((void *)anon_addr, 4096, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (anon == MAP_FAILED) {
        close(fd);
        unlink(path);
        return fail("merge-anon-mmap");
    }
    if ((uintptr_t)anon != anon_addr) {
        munmap(anon, 4096);
        close(fd);
        unlink(path);
        return fail("merge-anon-addr");
    }
    strcpy(anon, "anon");

    char *file = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (file == MAP_FAILED) {
        munmap(anon, 4096);
        close(fd);
        unlink(path);
        return fail("merge-shared-mmap");
    }
    strcpy(file, "file");

    char *moved = mremap(file, 4096, 4096,
                          MREMAP_MAYMOVE | MREMAP_FIXED, anon_addr + 4096);
    if (moved == MAP_FAILED) {
        munmap(file, 4096);
        munmap(anon, 4096);
        close(fd);
        unlink(path);
        return fail("merge-shared-remap");
    }

    if (memcmp(anon, "anon", 5) != 0) {
        munmap(moved, 4096);
        munmap(anon, 4096);
        close(fd);
        unlink(path);
        return fail("merge-anon-corrupted");
    }

    strcpy(moved, "moved");
    if (msync(moved, 4096, MS_SYNC) < 0) {
        munmap(moved, 4096);
        munmap(anon, 4096);
        close(fd);
        unlink(path);
        return fail("merge-shared-msync");
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        munmap(moved, 4096);
        munmap(anon, 4096);
        close(fd);
        unlink(path);
        return fail("merge-shared-lseek");
    }
    char readback[8];
    if (read(fd, readback, sizeof(readback)) != (ssize_t)sizeof(readback) ||
        memcmp(readback, "moved", 6) != 0) {
        munmap(moved, 4096);
        munmap(anon, 4096);
        close(fd);
        unlink(path);
        return fail("merge-shared-readback");
    }

    if (munmap(anon, 4096) < 0) {
        munmap(moved, 4096);
        close(fd);
        unlink(path);
        return fail("merge-anon-munmap");
    }
    if (munmap(moved, 4096) < 0) {
        close(fd);
        unlink(path);
        return fail("merge-shared-munmap");
    }
    if (read_page_cache_pinned() != base_pins) {
        close(fd);
        unlink(path);
        return fail("merge-shared-pinned-leak");
    }
    close(fd);
    unlink(path);
    return 0;
}

int main(void)
{
    printf("MM_STRESS: start\n");
    if (anonymous_fault_and_unmap() != 0)
        return 1;
    if (fixed_noreplace_conflict() != 0)
        return 1;
    if (fork_cow_and_exit() != 0)
        return 1;
    if (shared_file_writeback() != 0)
        return 1;
    if (shared_file_partial_munmap() != 0)
        return 1;
    if (shared_file_fork() != 0)
        return 1;
    if (shared_file_mremap() != 0)
        return 1;
    if (shared_file_mremap_dontunmap() != 0)
        return 1;
    if (shared_file_mremap_merge_guard() != 0)
        return 1;
    printf("MM_STRESS: PASS\n");
    return 0;
}
