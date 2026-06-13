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

static int shared_file_read_after_mmap_write(void)
{
    /* Coherence: writes through a MAP_SHARED mapping must be visible to
     * subsequent read() calls on the same file via the page cache. */
    const char *path = "/tmp/mm_stress_mmap_to_read";
    unsigned long base_pins = read_page_cache_pinned();
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return fail("m2r-open");

    char initial[4096];
    memset(initial, 'A', sizeof(initial));
    if (write(fd, initial, sizeof(initial)) != (ssize_t)sizeof(initial)) {
        close(fd);
        unlink(path);
        return fail("m2r-write-initial");
    }

    char *mem = mmap(NULL, sizeof(initial), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        unlink(path);
        return fail("m2r-mmap");
    }

    for (size_t i = 0; i < sizeof(initial); i++)
        mem[i] = (char)('0' + (i % 10));

    /* No msync here: the point is that the dirty page-cache page itself is
     * coherent with read(). */
    if (lseek(fd, 0, SEEK_SET) < 0) {
        munmap(mem, sizeof(initial));
        close(fd);
        unlink(path);
        return fail("m2r-lseek");
    }

    char readback[4096];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(readback)) {
        ssize_t n = read(fd, readback + total, sizeof(readback) - total);
        if (n < 0) {
            munmap(mem, sizeof(initial));
            close(fd);
            unlink(path);
            return fail("m2r-read");
        }
        if (n == 0)
            break;
        total += n;
    }

    int ok = (total == (ssize_t)sizeof(readback) &&
              memcmp(mem, readback, sizeof(readback)) == 0);
    if (!ok) {
        munmap(mem, sizeof(initial));
        close(fd);
        unlink(path);
        return fail("m2r-compare");
    }

    if (munmap(mem, sizeof(initial)) < 0) {
        close(fd);
        unlink(path);
        return fail("m2r-munmap");
    }
    if (read_page_cache_pinned() != base_pins) {
        close(fd);
        unlink(path);
        return fail("m2r-pinned-leak");
    }
    close(fd);
    unlink(path);
    return 0;
}

static int shared_file_mmap_after_write(void)
{
    /* Coherence: writes through write() must be visible to a later MAP_SHARED
     * mmap fault. write() invalidates the affected page-cache range. */
    const char *path = "/tmp/mm_stress_write_to_mmap";
    unsigned long base_pins = read_page_cache_pinned();
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return fail("w2m-open");

    char buf[4096];
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (char)('0' + (i % 10));
    if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
        close(fd);
        unlink(path);
        return fail("w2m-write");
    }

    char *mem = mmap(NULL, sizeof(buf), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        close(fd);
        unlink(path);
        return fail("w2m-mmap");
    }

    if (memcmp(mem, buf, sizeof(buf)) != 0) {
        munmap(mem, sizeof(buf));
        close(fd);
        unlink(path);
        return fail("w2m-compare");
    }

    if (munmap(mem, sizeof(buf)) < 0) {
        close(fd);
        unlink(path);
        return fail("w2m-munmap");
    }
    if (read_page_cache_pinned() != base_pins) {
        close(fd);
        unlink(path);
        return fail("w2m-pinned-leak");
    }
    close(fd);
    unlink(path);
    return 0;
}

static int shared_file_truncate_smaller(void)
{
    /* Truncation must discard page-cache pages beyond the new size so that
     * reads return EOF and stale data is never visible. */
    const char *path = "/tmp/mm_stress_trunc_small";
    unsigned long base_pins = read_page_cache_pinned();
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return fail("trunc-small-open");

    char buf[8192];
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (char)('A' + (i % 26));
    if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
        close(fd);
        unlink(path);
        return fail("trunc-small-write");
    }

    if (ftruncate(fd, 4096) < 0) {
        close(fd);
        unlink(path);
        return fail("trunc-small-truncate");
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        unlink(path);
        return fail("trunc-small-lseek");
    }

    char readback[8192];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(readback)) {
        ssize_t n = read(fd, readback + total, sizeof(readback) - total);
        if (n < 0) {
            close(fd);
            unlink(path);
            return fail("trunc-small-read");
        }
        if (n == 0)
            break;
        total += n;
    }

    int ok = (total == 4096 && memcmp(readback, buf, 4096) == 0);
    if (!ok) {
        close(fd);
        unlink(path);
        return fail("trunc-small-compare");
    }

    if (read_page_cache_pinned() != base_pins) {
        close(fd);
        unlink(path);
        return fail("trunc-small-pinned-leak");
    }
    close(fd);
    unlink(path);
    return 0;
}

static int shared_file_truncate_larger(void)
{
    /* Extending a file must expose zeros in the newly created region. */
    const char *path = "/tmp/mm_stress_trunc_large";
    unsigned long base_pins = read_page_cache_pinned();
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return fail("trunc-large-open");

    char buf[4096];
    memset(buf, 'X', sizeof(buf));
    if (write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
        close(fd);
        unlink(path);
        return fail("trunc-large-write");
    }

    if (ftruncate(fd, 8192) < 0) {
        close(fd);
        unlink(path);
        return fail("trunc-large-truncate");
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        unlink(path);
        return fail("trunc-large-lseek");
    }

    char readback[8192];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(readback)) {
        ssize_t n = read(fd, readback + total, sizeof(readback) - total);
        if (n < 0) {
            close(fd);
            unlink(path);
            return fail("trunc-large-read");
        }
        if (n == 0)
            break;
        total += n;
    }

    char zeros[4096];
    memset(zeros, 0, sizeof(zeros));
    int ok = (total == (ssize_t)sizeof(readback) &&
              memcmp(readback, buf, 4096) == 0 &&
              memcmp(readback + 4096, zeros, 4096) == 0);
    if (!ok) {
        close(fd);
        unlink(path);
        return fail("trunc-large-compare");
    }

    if (read_page_cache_pinned() != base_pins) {
        close(fd);
        unlink(path);
        return fail("trunc-large-pinned-leak");
    }
    close(fd);
    unlink(path);
    return 0;
}

static int shared_file_eviction_pressure(void)
{
    /* Force page-cache eviction by accessing a file larger than the cache,
     * then verify that re-reads still return correct data from disk. */
    const char *path = "/bin/mm_stress_evict";
    unsigned long base_pins = read_page_cache_pinned();
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return fail("evict-open");

    const size_t page_cache_pages = 2048;
    const size_t total_pages = page_cache_pages + 256; /* 9 MiB, forces eviction */
    const size_t chunk_pages = 256;
    const size_t file_size = total_pages * 4096;
    const size_t chunk_size = chunk_pages * 4096;
    char *chunk = malloc(chunk_size);
    if (!chunk) {
        close(fd);
        unlink(path);
        return fail("evict-malloc");
    }

    for (size_t idx = 0; idx < total_pages; idx += chunk_pages) {
        for (size_t p = 0; p < chunk_pages; p++) {
            char *page = chunk + p * 4096;
            size_t page_idx = idx + p;
            for (size_t i = 0; i < 4096; i++)
                page[i] = (char)((page_idx + i) % 256);
        }
        size_t to_write = chunk_size;
        if (idx + chunk_pages > total_pages)
            to_write = (total_pages - idx) * 4096;
        size_t written = 0;
        while (written < to_write) {
            ssize_t n = write(fd, chunk + written, to_write - written);
            if (n <= 0) {
                free(chunk);
                close(fd);
                unlink(path);
                return fail("evict-write");
            }
            written += (size_t)n;
        }
    }
    free(chunk);

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        unlink(path);
        return fail("evict-fstat-write");
    }

    if (fsync(fd) < 0) {
        close(fd);
        unlink(path);
        return fail("evict-fsync");
    }

    chunk = malloc(chunk_size);
    if (!chunk) {
        close(fd);
        unlink(path);
        return fail("evict-malloc-pressure");
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        free(chunk);
        close(fd);
        unlink(path);
        return fail("evict-seek-pressure");
    }
    size_t read_so_far = 0;
    while (read_so_far < file_size) {
        ssize_t n = read(fd, chunk, chunk_size);
        if (n <= 0) {
            free(chunk);
            close(fd);
            unlink(path);
            return fail("evict-read-pressure");
        }
        read_so_far += (size_t)n;
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        free(chunk);
        close(fd);
        unlink(path);
        return fail("evict-seek-verify");
    }
    for (size_t idx = 0; idx < total_pages; idx += chunk_pages) {
        size_t to_read = chunk_size;
        if (idx + chunk_pages > total_pages)
            to_read = (total_pages - idx) * 4096;
        size_t got = 0;
        while (got < to_read) {
            ssize_t n = read(fd, chunk + got, to_read - got);
            if (n <= 0) {
                free(chunk);
                close(fd);
                unlink(path);
                return fail("evict-read-verify");
            }
            got += (size_t)n;
        }
        for (size_t p = 0; p < chunk_pages; p++) {
            size_t page_idx = idx + p;
            if (page_idx >= total_pages)
                break;
            const char *page = chunk + p * 4096;
            for (size_t i = 0; i < 4096; i++) {
                if (page[i] != (char)((page_idx + i) % 256)) {
                    free(chunk);
                    close(fd);
                    unlink(path);
                    return fail("evict-verify");
                }
            }
        }
    }

    free(chunk);
    if (read_page_cache_pinned() != base_pins) {
        close(fd);
        unlink(path);
        return fail("evict-pinned-leak");
    }
    close(fd);
    unlink(path);
    return 0;
}

static int shared_file_eviction_with_mmap(void)
{
    /* Verify that page-cache eviction under memory pressure keeps mapped
     * MAP_SHARED pages coherent and leaves unmapped file data recoverable. */
    const char *path_a = "/tmp/mm_stress_evict_mmap_a";
    const char *path_b = "/tmp/mm_stress_evict_mmap_b";
    unsigned long base_pins = read_page_cache_pinned();

    const size_t page_cache_pages = 2048;
    const size_t file_a_pages = page_cache_pages / 2;
    const size_t mmap_pages = page_cache_pages / 4;
    const size_t file_b_pages = page_cache_pages - mmap_pages;
    const size_t chunk_pages = 256;
    const size_t chunk_size = chunk_pages * 4096;

    size_t file_a_size = file_a_pages * 4096;
    size_t file_b_size = file_b_pages * 4096;
    size_t mmap_size = mmap_pages * 4096;

    char *chunk = malloc(chunk_size);
    if (!chunk)
        return fail("evict-mmap-malloc");

    int fd_a = open(path_a, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd_a < 0) {
        free(chunk);
        return fail("evict-mmap-open-a");
    }
    for (size_t idx = 0; idx < file_a_pages; idx += chunk_pages) {
        size_t batch = chunk_pages;
        if (idx + batch > file_a_pages)
            batch = file_a_pages - idx;
        for (size_t p = 0; p < batch; p++) {
            char *page = chunk + p * 4096;
            size_t page_idx = idx + p;
            for (size_t i = 0; i < 4096; i++)
                page[i] = (char)((page_idx * 7 + i) % 251);
        }
        size_t to_write = batch * 4096;
        size_t written = 0;
        while (written < to_write) {
            ssize_t n = write(fd_a, chunk + written, to_write - written);
            if (n <= 0) {
                free(chunk);
                close(fd_a);
                unlink(path_a);
                return fail("evict-mmap-write-a");
            }
            written += (size_t)n;
        }
    }
    if (fsync(fd_a) < 0) {
        free(chunk);
        close(fd_a);
        unlink(path_a);
        return fail("evict-mmap-fsync-a");
    }

    char *mem = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_a, 0);
    if (mem == MAP_FAILED) {
        free(chunk);
        close(fd_a);
        unlink(path_a);
        return fail("evict-mmap-mmap-a");
    }

    int fd_b = open(path_b, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd_b < 0) {
        munmap(mem, mmap_size);
        free(chunk);
        close(fd_a);
        unlink(path_a);
        return fail("evict-mmap-open-b");
    }
    for (size_t idx = 0; idx < file_b_pages; idx += chunk_pages) {
        size_t batch = chunk_pages;
        if (idx + batch > file_b_pages)
            batch = file_b_pages - idx;
        for (size_t p = 0; p < batch; p++) {
            char *page = chunk + p * 4096;
            size_t page_idx = idx + p;
            for (size_t i = 0; i < 4096; i++)
                page[i] = (char)((page_idx * 11 + i) % 253);
        }
        size_t to_write = batch * 4096;
        size_t written = 0;
        while (written < to_write) {
            ssize_t n = write(fd_b, chunk + written, to_write - written);
            if (n <= 0) {
                free(chunk);
                close(fd_b);
                unlink(path_b);
                munmap(mem, mmap_size);
                close(fd_a);
                unlink(path_a);
                return fail("evict-mmap-write-b");
            }
            written += (size_t)n;
        }
        if (fsync(fd_b) < 0) {
            free(chunk);
            close(fd_b);
            unlink(path_b);
            munmap(mem, mmap_size);
            close(fd_a);
            unlink(path_a);
            return fail("evict-mmap-fsync-b-batch");
        }
    }
    if (fsync(fd_b) < 0) {
        free(chunk);
        close(fd_b);
        unlink(path_b);
        munmap(mem, mmap_size);
        close(fd_a);
        unlink(path_a);
        return fail("evict-mmap-fsync-b");
    }

    if (lseek(fd_b, 0, SEEK_SET) < 0) {
        free(chunk);
        close(fd_b);
        unlink(path_b);
        munmap(mem, mmap_size);
        close(fd_a);
        unlink(path_a);
        return fail("evict-mmap-seek-b");
    }
    size_t read_so_far = 0;
    while (read_so_far < file_b_size) {
        ssize_t n = read(fd_b, chunk, chunk_size);
        if (n <= 0) {
            free(chunk);
            close(fd_b);
            unlink(path_b);
            munmap(mem, mmap_size);
            close(fd_a);
            unlink(path_a);
            return fail("evict-mmap-read-pressure");
        }
        read_so_far += (size_t)n;
    }

    for (size_t p = 0; p < mmap_pages; p++) {
        const char *page = mem + p * 4096;
        size_t page_idx = p;
        for (size_t i = 0; i < 4096; i++) {
            if (page[i] != (char)((page_idx * 7 + i) % 251)) {
                munmap(mem, mmap_size);
                free(chunk);
                close(fd_b);
                unlink(path_b);
                close(fd_a);
                unlink(path_a);
                return fail("evict-mmap-verify-mapped");
            }
        }
    }

    munmap(mem, mmap_size);
    close(fd_b);
    unlink(path_b);

    if (lseek(fd_a, 0, SEEK_SET) < 0) {
        free(chunk);
        close(fd_a);
        unlink(path_a);
        return fail("evict-mmap-seek-verify");
    }
    for (size_t idx = 0; idx < file_a_pages; idx += chunk_pages) {
        size_t batch = chunk_pages;
        if (idx + batch > file_a_pages)
            batch = file_a_pages - idx;
        size_t to_read = batch * 4096;
        size_t got = 0;
        while (got < to_read) {
            ssize_t n = read(fd_a, chunk + got, to_read - got);
            if (n <= 0) {
                free(chunk);
                close(fd_a);
                unlink(path_a);
                return fail("evict-mmap-read-verify");
            }
            got += (size_t)n;
        }
        for (size_t p = 0; p < batch; p++) {
            size_t page_idx = idx + p;
            const char *page = chunk + p * 4096;
            for (size_t i = 0; i < 4096; i++) {
                if (page[i] != (char)((page_idx * 7 + i) % 251)) {
                    free(chunk);
                    close(fd_a);
                    unlink(path_a);
                    return fail("evict-mmap-verify");
                }
            }
        }
    }

    free(chunk);
    if (read_page_cache_pinned() != base_pins) {
        close(fd_a);
        unlink(path_a);
        return fail("evict-mmap-pinned-leak");
    }
    close(fd_a);
    unlink(path_a);
    return 0;
}

#define HPSIZE (2 * 1024 * 1024)

static int huge_page_basic(void)
{
    /* Allocate a huge page, write a pattern, and verify readback. */
    unsigned long base_pins = read_page_cache_pinned();
    char *mem = mmap(NULL, HPSIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (mem == MAP_FAILED)
        return fail("huge-basic-mmap");

    for (size_t i = 0; i < HPSIZE; i++)
        mem[i] = (char)((i * 13) % 251);
    for (size_t i = 0; i < HPSIZE; i++) {
        if (mem[i] != (char)((i * 13) % 251)) {
            munmap(mem, HPSIZE);
            return fail("huge-basic-verify");
        }
    }

    if (munmap(mem, HPSIZE) < 0)
        return fail("huge-basic-munmap");
    if (read_page_cache_pinned() != base_pins)
        return fail("huge-basic-pinned-leak");
    return 0;
}

static int huge_page_fork_cow(void)
{
    /* Fork with a huge page mapping; child writes must not affect parent. */
    unsigned long base_pins = read_page_cache_pinned();
    char *mem = mmap(NULL, HPSIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (mem == MAP_FAILED)
        return fail("huge-fork-mmap");

    for (size_t i = 0; i < HPSIZE; i++)
        mem[i] = (char)((i * 17) % 251);

    pid_t pid = fork();
    if (pid < 0) {
        munmap(mem, HPSIZE);
        return fail("huge-fork");
    }
    if (pid == 0) {
        for (size_t i = 0; i < HPSIZE; i++)
            mem[i] = (char)(~mem[i]);
        _exit(0);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        munmap(mem, HPSIZE);
        return fail("huge-fork-wait");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        munmap(mem, HPSIZE);
        return fail("huge-fork-child");
    }

    for (size_t i = 0; i < HPSIZE; i++) {
        if (mem[i] != (char)((i * 17) % 251)) {
            munmap(mem, HPSIZE);
            return fail("huge-fork-verify");
        }
    }

    if (munmap(mem, HPSIZE) < 0)
        return fail("huge-fork-munmap");
    if (read_page_cache_pinned() != base_pins)
        return fail("huge-fork-pinned-leak");
    return 0;
}

static int huge_page_partial_munmap(void)
{
    /* Partial munmap of a huge page forces demotion; remaining halves stay mapped. */
    unsigned long base_pins = read_page_cache_pinned();
    char *mem = mmap(NULL, HPSIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (mem == MAP_FAILED)
        return fail("huge-partial-mmap");

    for (size_t i = 0; i < HPSIZE; i++)
        mem[i] = (char)((i * 19) % 251);

    if (munmap(mem + HPSIZE / 2, 4096) < 0) {
        munmap(mem, HPSIZE);
        return fail("huge-partial-munmap-middle");
    }

    for (size_t i = 0; i < HPSIZE / 2; i++) {
        if (mem[i] != (char)((i * 19) % 251)) {
            munmap(mem, HPSIZE);
            return fail("huge-partial-verify-lo");
        }
    }
    for (size_t i = HPSIZE / 2 + 4096; i < HPSIZE; i++) {
        if (mem[i] != (char)((i * 19) % 251)) {
            munmap(mem, HPSIZE);
            return fail("huge-partial-verify-hi");
        }
    }

    if (munmap(mem, HPSIZE / 2) < 0 ||
        munmap(mem + HPSIZE / 2 + 4096, HPSIZE / 2 - 4096) < 0) {
        return fail("huge-partial-munmap-rest");
    }
    if (read_page_cache_pinned() != base_pins)
        return fail("huge-partial-pinned-leak");
    return 0;
}

static int huge_page_mprotect(void)
{
    /* mprotect across a huge page forces demotion and preserves content. */
    unsigned long base_pins = read_page_cache_pinned();
    char *mem = mmap(NULL, HPSIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (mem == MAP_FAILED)
        return fail("huge-prot-mmap");

    for (size_t i = 0; i < HPSIZE; i++)
        mem[i] = (char)((i * 23) % 251);

    if (mprotect(mem, HPSIZE, PROT_READ) < 0) {
        munmap(mem, HPSIZE);
        return fail("huge-prot-mprotect");
    }

    for (size_t i = 0; i < HPSIZE; i++) {
        if (mem[i] != (char)((i * 23) % 251)) {
            munmap(mem, HPSIZE);
            return fail("huge-prot-verify");
        }
    }

    if (munmap(mem, HPSIZE) < 0)
        return fail("huge-prot-munmap");
    if (read_page_cache_pinned() != base_pins)
        return fail("huge-prot-pinned-leak");
    return 0;
}

int main(void)
{
    printf("MM_STRESS: start\n");
    printf("MM_STRESS: anon start\n");
    if (anonymous_fault_and_unmap() != 0)
        return 1;
    printf("MM_STRESS: fixed start\n");
    if (fixed_noreplace_conflict() != 0)
        return 1;
    printf("MM_STRESS: cow start\n");
    if (fork_cow_and_exit() != 0)
        return 1;
    printf("MM_STRESS: writeback start\n");
    if (shared_file_writeback() != 0)
        return 1;
    printf("MM_STRESS: partial start\n");
    if (shared_file_partial_munmap() != 0)
        return 1;
    printf("MM_STRESS: fork start\n");
    if (shared_file_fork() != 0)
        return 1;
    printf("MM_STRESS: mremap start\n");
    if (shared_file_mremap() != 0)
        return 1;
    printf("MM_STRESS: dontunmap start\n");
    if (shared_file_mremap_dontunmap() != 0)
        return 1;
    printf("MM_STRESS: merge start\n");
    if (shared_file_mremap_merge_guard() != 0)
        return 1;
    printf("MM_STRESS: m2r start\n");
    if (shared_file_read_after_mmap_write() != 0)
        return 1;
    printf("MM_STRESS: w2m start\n");
    if (shared_file_mmap_after_write() != 0)
        return 1;
    printf("MM_STRESS: trunc-small start\n");
    if (shared_file_truncate_smaller() != 0)
        return 1;
    printf("MM_STRESS: trunc-large start\n");
    if (shared_file_truncate_larger() != 0)
        return 1;
    printf("MM_STRESS: evict start\n");
    if (shared_file_eviction_pressure() != 0)
        return 1;
    printf("MM_STRESS: evict-mmap start\n");
    if (shared_file_eviction_with_mmap() != 0)
        return 1;
    printf("MM_STRESS: huge-basic start\n");
    if (huge_page_basic() != 0)
        return 1;
    printf("MM_STRESS: huge-fork start\n");
    if (huge_page_fork_cow() != 0)
        return 1;
    printf("MM_STRESS: huge-partial start\n");
    if (huge_page_partial_munmap() != 0)
        return 1;
    printf("MM_STRESS: huge-prot start\n");
    if (huge_page_mprotect() != 0)
        return 1;
    printf("MM_STRESS: PASS\n");
    return 0;
}
