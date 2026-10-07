#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Deliberately does NOT print errno.  Most call sites are assertions or
 * `ret < 0` checks whose failure has nothing to do with errno, so printing
 * whatever errno happened to be left behind produced actively misleading
 * output: a data-mismatch failure once reported "errno=17 (EEXIST)" and sent
 * the investigation after a nonexistent address collision (docs 10.62-10.63).
 * A site that genuinely needs the errno should pass it explicitly rather than
 * relying on the ambient value. */
static int fail(const char *what)
{
    printf("MM_STRESS: FAIL %s\n", what);
    return 1;
}

#define VMA_RACE_WORKERS 8
#define VMA_RACE_ROUNDS 256
#define VMA_FORK_EXEC_WORKERS 4
#define VMA_FORK_EXEC_ROUNDS 64
#define FORK_MPROTECT_COW_ROUNDS 32

static volatile int vma_race_ready;
static volatile int vma_race_start;
static volatile int vma_fork_exec_ready;
static volatile int vma_fork_exec_start;
static volatile int vma_fork_exec_stop;
static volatile int vma_fork_exec_failed;

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* mseal(2): sealing is one-way and forbids later layout/protection changes on
 * the sealed range, while leaving unsealed neighbours fully mutable. */
static int mseal_semantics(void)
{
#ifndef SYS_mseal
#define SYS_mseal 462
#endif
    void *mem = mmap(NULL, 8 * 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return fail("mseal-mmap");

    /* Seal the middle 4 pages only. */
    if (syscall(SYS_mseal, (uintptr_t)mem + 2 * 4096, 4 * 4096, 0) != 0)
        return fail("mseal-seal");

    /* mprotect over the sealed range must be refused with EPERM. */
    errno = 0;
    if (mprotect(mem + 2 * 4096, 4 * 4096, PROT_READ) == 0 ||
        errno != EPERM)
        return fail("mseal-mprotect");

    /* munmap over the sealed range must be refused. */
    errno = 0;
    if (munmap(mem + 2 * 4096, 4096) == 0 || errno != EPERM)
        return fail("mseal-munmap");

    /* mremap (shrink/move) over the sealed range must be refused. */
    errno = 0;
    void *r = mremap(mem + 2 * 4096, 4 * 4096, 4 * 4096, MREMAP_MAYMOVE);
    if (r != MAP_FAILED || errno != EPERM)
        return fail("mseal-mremap");

    /* MAP_FIXED overwrite of the sealed range must be refused. */
    errno = 0;
    void *ov = mmap(mem + 2 * 4096, 4096, PROT_READ,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (ov != MAP_FAILED || errno != EPERM)
        return fail("mseal-mapfixed");

    /* madvise DONTNEED over the sealed range must be refused. */
    errno = 0;
    if (madvise(mem + 2 * 4096, 4096, MADV_DONTNEED) == 0 || errno != EPERM)
        return fail("mseal-madvise");

    /* Unsealed neighbours stay fully mutable. */
    if (mprotect(mem, 4096, PROT_READ) < 0)
        return fail("mseal-neighbour-mprotect");
    if (munmap(mem, 4096) < 0)
        return fail("mseal-neighbour-munmap");

    /* The remaining unsealed tail can still be released. */
    if (munmap(mem + 6 * 4096, 2 * 4096) < 0)
        return fail("mseal-tail-munmap");

    /* Sealing is inherited by fork children. */
    void *cmem = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cmem == MAP_FAILED)
        return fail("mseal-fork-mmap");
    if (syscall(SYS_mseal, (uintptr_t)cmem, 4 * 4096, 0) != 0)
        return fail("mseal-fork-seal");

    pid_t pid = fork();
    if (pid < 0)
        return fail("mseal-fork");
    if (pid == 0) {
        errno = 0;
        int r = mprotect(cmem, 4 * 4096, PROT_READ);
        if (r == 0 || errno != EPERM)
            _exit(1);
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        return fail("mseal-fork-inherit");
    /* The seal persists in the parent too: munmap stays refused.  The VMA is
     * released by exit teardown, which is not blocked by seals. */
    errno = 0;
    if (munmap(cmem, 4 * 4096) == 0 || errno != EPERM)
        return fail("mseal-fork-parent-still-sealed");
    return 0;
}

static void *vma_deferred_race_worker(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&vma_race_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&vma_race_start, __ATOMIC_ACQUIRE))
        sched_yield();

    for (int round = 0; round < VMA_RACE_ROUNDS; round++) {
        char *mem = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED)
            return (void *)(intptr_t)1;

        /* Splitting the middle page and restoring the original protection
         * merges the three adjacent VMAs again.  The discarded split nodes
         * exercise the per-mm deferred release list. */
        if (mprotect(mem + 4096, 4096, PROT_READ) < 0 ||
            mprotect(mem, 3 * 4096, PROT_READ | PROT_WRITE) < 0 ||
            munmap(mem, 3 * 4096) < 0)
            return (void *)(intptr_t)1;
    }
    return NULL;
}

/* Wide-cursor collision.
 *
 * mm_pt_provision_anon() is the only source of a cursor whose covering level is
 * > 0, and it needs an anonymous private mapping larger than one leaf table
 * (512 pages) while staying at or below MM_ANON_PROVISION_MAX_PAGES.  4 MiB is
 * 1024 pages, inside both bounds.  Provisioning walks that range with ONE wide
 * cursor; every later fault inside it uses a narrow single-page cursor over the
 * same leaf tables.  Those two must exclude each other, which is exactly what
 * the per-operation leaf lock is for, and nothing else in this file can produce
 * the pair.
 *
 * The puncher also unmaps a slice from under the other threads, so the retire
 * path (mm_pt_retire_table) runs concurrently with cursor descent and the audit
 * at the end has something to catch if the two disagree.
 */
#define WIDE_CURSOR_BYTES  (4 * 1024 * 1024)
/* Slice count must divide WIDE_CURSOR_BYTES into page-aligned pieces: a slice
 * that is not a multiple of PAGE_SIZE makes both munmap and mmap fail EINVAL.
 * 8 gives 512 KiB (128 pages) each; six punchers take slices 0-5, the unmapper
 * takes slice 6, and slice 7 is left alone so a puncher never shares the slice
 * the unmapper tears down. */
#define WIDE_CURSOR_SLICES 8
#define WIDE_CURSOR_WORKERS 6
#define WIDE_CURSOR_ROUNDS 64

static volatile int wide_cursor_ready;
static volatile int wide_cursor_start;
static char *wide_cursor_base;

static void *wide_cursor_punch(void *arg)
{
    long id = (long)(intptr_t)arg;
    size_t slice = WIDE_CURSOR_BYTES / WIDE_CURSOR_SLICES;

    __atomic_add_fetch(&wide_cursor_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&wide_cursor_start, __ATOMIC_ACQUIRE))
        sched_yield();

    for (int round = 0; round < WIDE_CURSOR_ROUNDS; round++) {
        char *p = wide_cursor_base + (size_t)id * slice;
        for (size_t off = 0; off < slice; off += 4096)
            p[off] = (char)(round + (int)off + (int)id);
        for (size_t off = 0; off < slice; off += 4096)
            if (p[off] != (char)(round + (int)off + (int)id))
                return (void *)(intptr_t)1;
    }
    return NULL;
}

static void *wide_cursor_unmapper(void *arg)
{
    (void)arg;
    size_t slice = WIDE_CURSOR_BYTES / WIDE_CURSOR_SLICES;

    __atomic_add_fetch(&wide_cursor_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&wide_cursor_start, __ATOMIC_ACQUIRE))
        sched_yield();

    /* Drop the reserved slice, then put it back, repeatedly.  munmap tears the
     * page-table subtree down under the punching threads, which is the side of
     * the collision the per-page fault path never produces on its own. */
    char *tail = wide_cursor_base + (size_t)WIDE_CURSOR_WORKERS * slice;
    for (int round = 0; round < WIDE_CURSOR_ROUNDS; round++) {
        if (munmap(tail, slice) < 0)
            return (void *)(intptr_t)1;
        char *again = mmap(tail, slice, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (again == MAP_FAILED)
            return (void *)(intptr_t)1;
        if (again != tail)
            return (void *)(intptr_t)1;
    }
    return NULL;
}

static int concurrent_wide_cursor_faults(void)
{
    pthread_t workers[WIDE_CURSOR_WORKERS + 1];
    wide_cursor_ready = 0;
    wide_cursor_start = 0;

    wide_cursor_base = mmap(NULL, WIDE_CURSOR_BYTES, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (wide_cursor_base == MAP_FAILED)
        return fail("wide-cursor-mmap");

    int created = 0;
    for (; created < WIDE_CURSOR_WORKERS; created++) {
        int r = pthread_create(&workers[created], NULL, wide_cursor_punch,
                               (void *)(intptr_t)created);
        if (r != 0) {
            __atomic_store_n(&wide_cursor_start, 1, __ATOMIC_RELEASE);
            for (int i = 0; i < created; i++)
                pthread_join(workers[i], NULL);
            munmap(wide_cursor_base, WIDE_CURSOR_BYTES);
            errno = r;
            return fail("wide-cursor-pthread-create");
        }
    }
    int r = pthread_create(&workers[created], NULL, wide_cursor_unmapper, NULL);
    if (r != 0) {
        __atomic_store_n(&wide_cursor_start, 1, __ATOMIC_RELEASE);
        for (int i = 0; i < created; i++)
            pthread_join(workers[i], NULL);
        munmap(wide_cursor_base, WIDE_CURSOR_BYTES);
        errno = r;
        return fail("wide-cursor-unmapper-create");
    }
    created++;

    while (__atomic_load_n(&wide_cursor_ready, __ATOMIC_ACQUIRE) != created)
        sched_yield();
    __atomic_store_n(&wide_cursor_start, 1, __ATOMIC_RELEASE);

    int bad = 0;
    for (int i = 0; i < created; i++) {
        void *result = NULL;
        if (pthread_join(workers[i], &result) != 0)
            bad = 1;
        else if (result)
            bad = 1;
    }

    munmap(wide_cursor_base, WIDE_CURSOR_BYTES);
    if (bad)
        return fail("wide-cursor-worker");
    printf("MM_WIDE_CURSOR: PASS\n");
    return 0;
}

static int concurrent_vma_deferred_flush(void)
{
    pthread_t workers[VMA_RACE_WORKERS];
    vma_race_ready = 0;
    vma_race_start = 0;

    int created = 0;
    for (; created < VMA_RACE_WORKERS; created++) {
        int r = pthread_create(&workers[created], NULL,
                               vma_deferred_race_worker, NULL);
        if (r != 0) {
            __atomic_store_n(&vma_race_start, 1, __ATOMIC_RELEASE);
            for (int i = 0; i < created; i++)
                pthread_join(workers[i], NULL);
            errno = r;
            return fail("vma-race-pthread-create");
        }
    }

    while (__atomic_load_n(&vma_race_ready, __ATOMIC_ACQUIRE) !=
           VMA_RACE_WORKERS)
        sched_yield();
    __atomic_store_n(&vma_race_start, 1, __ATOMIC_RELEASE);

    for (int i = 0; i < VMA_RACE_WORKERS; i++) {
        void *result = NULL;
        int r = pthread_join(workers[i], &result);
        if (r != 0 || result != NULL) {
            errno = r;
            return fail("vma-race-worker");
        }
    }
    return 0;
}

/*
 * Regression for MM_FORK_DEFERRED_STATE_REGRESSION_GUARD.  A VMA writer
 * queues split/merge victims on mm->deferred_vma, drops mm->lock, and then
 * reacquires the lock to detach the queue.  A fork running on another CPU can
 * win that reacquisition race.  The child must clone only persistent address
 * space state; inheriting the transient queue lets the parent free the nodes
 * before the child destroys its pre-exec mm, causing a stale second free that
 * can remove a live VMA from the freshly loaded executable.
 */
static void *vma_fork_exec_writer(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&vma_fork_exec_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&vma_fork_exec_start, __ATOMIC_ACQUIRE))
        sched_yield();

    while (!__atomic_load_n(&vma_fork_exec_stop, __ATOMIC_ACQUIRE)) {
        char *mem = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED ||
            mprotect(mem + 4096, 2 * 4096, PROT_READ) < 0 ||
            mprotect(mem, 4 * 4096, PROT_READ | PROT_WRITE) < 0 ||
            munmap(mem, 4 * 4096) < 0) {
            if (mem != MAP_FAILED)
                (void)munmap(mem, 4 * 4096);
            __atomic_store_n(&vma_fork_exec_failed, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&vma_fork_exec_stop, 1, __ATOMIC_RELEASE);
            return (void *)(intptr_t)1;
        }
    }
    return NULL;
}

static int verify_vma_fork_exec_child(void)
{
    char *mem = mmap(NULL, 8 * 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return 1;
    for (size_t i = 0; i < 8 * 4096; i += 4096)
        mem[i] = (char)(i / 4096 + 1);
    if (mprotect(mem + 2 * 4096, 4 * 4096, PROT_READ) < 0 ||
        mprotect(mem, 8 * 4096, PROT_READ | PROT_WRITE) < 0) {
        (void)munmap(mem, 8 * 4096);
        return 1;
    }
    for (size_t i = 0; i < 8 * 4096; i += 4096) {
        if (mem[i] != (char)(i / 4096 + 1)) {
            (void)munmap(mem, 8 * 4096);
            return 1;
        }
    }
    return munmap(mem, 8 * 4096) == 0 ? 0 : 1;
}

static int concurrent_vma_fork_exec(void)
{
    pthread_t workers[VMA_FORK_EXEC_WORKERS];
    vma_fork_exec_ready = 0;
    vma_fork_exec_start = 0;
    vma_fork_exec_stop = 0;
    vma_fork_exec_failed = 0;

    int created = 0;
    for (; created < VMA_FORK_EXEC_WORKERS; created++) {
        int r = pthread_create(&workers[created], NULL,
                               vma_fork_exec_writer, NULL);
        if (r != 0) {
            __atomic_store_n(&vma_fork_exec_start, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&vma_fork_exec_stop, 1, __ATOMIC_RELEASE);
            for (int i = 0; i < created; i++)
                pthread_join(workers[i], NULL);
            errno = r;
            return fail("vma-fork-exec-pthread-create");
        }
    }

    while (__atomic_load_n(&vma_fork_exec_ready, __ATOMIC_ACQUIRE) !=
           VMA_FORK_EXEC_WORKERS)
        sched_yield();
    __atomic_store_n(&vma_fork_exec_start, 1, __ATOMIC_RELEASE);

    int result = 0;
    for (int round = 0; round < VMA_FORK_EXEC_ROUNDS; round++) {
        if (__atomic_load_n(&vma_fork_exec_failed, __ATOMIC_ACQUIRE)) {
            result = fail("vma-fork-exec-writer");
            break;
        }

        pid_t pid = fork();
        if (pid < 0) {
            result = fail("vma-fork-exec-fork");
            break;
        }
        if (pid == 0) {
            char *child_argv[] = {
                "mm_stress", "--verify-vma-fork-exec-child", NULL,
            };
            char *child_envp[] = {"PATH=/bin", NULL};
            execve("/bin/mm_stress", child_argv, child_envp);
            _exit(127);
        }

        int status = 0;
        if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            result = fail("vma-fork-exec-child");
            break;
        }
    }

    __atomic_store_n(&vma_fork_exec_stop, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < created; i++) {
        void *worker_result = NULL;
        int r = pthread_join(workers[i], &worker_result);
        if (r != 0 || worker_result != NULL) {
            errno = r;
            result = fail("vma-fork-exec-worker");
        }
    }
    return result;
}

/* A private page stays shared after fork until a writer faults.  Cycling the
 * child through read-only and writable protections must not erase that COW
 * obligation and let its store modify the parent's frame. */
static int fork_mprotect_cow(void)
{
    volatile unsigned char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED)
        return fail("fork-mprotect-cow-mmap");
    page[0] = 0x35;

    for (int round = 0; round < FORK_MPROTECT_COW_ROUNDS; round++) {
        pid_t pid = fork();
        if (pid < 0) {
            munmap((void *)page, 4096);
            return fail("fork-mprotect-cow-fork");
        }
        if (pid == 0) {
            unsigned char value = (unsigned char)(round + 1);
            if (mprotect((void *)page, 4096, PROT_READ) < 0 ||
                mprotect((void *)page, 4096, PROT_READ | PROT_WRITE) < 0)
                _exit(2);
            page[0] = value;
            _exit(page[0] == value ? 0 : 3);
        }

        int status = 0;
        if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0 || page[0] != 0x35) {
            munmap((void *)page, 4096);
            return fail("fork-mprotect-cow-isolation");
        }
    }
    if (munmap((void *)page, 4096) < 0)
        return fail("fork-mprotect-cow-munmap");
    return 0;
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
    unsigned long final_pins = read_page_cache_pinned();
    if (final_pins != base_pins) {
        printf("MM_STRESS: shared pins base=%lu final=%lu\n",
               base_pins, final_pins);
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

/* The ordered-index capacity-overflow fallback (MM_SEG_INDEX_CAPACITY in
 * kernel/include/mm/vm.h).
 *
 * Every other case here runs an address space of a few dozen mappings against a
 * capacity of 1024, so mm_seg_find()'s overflow branch -- walk the list instead
 * of binary-searching the index -- had never executed anywhere in the tree.  It
 * was not merely untested: it was wrong.  The rebuild was retried on every
 * lookup while the state said "over capacity", so each fault in such a process
 * paid a full 1024-entry rebuild (take 1024 references, discover the overflow,
 * put them all back) and *then* the list walk the rebuild was meant to replace.
 * The fix is one comparison in mm_seg_find(); this is the case that would have
 * caught it, because it is the only way to get an address space over the cap.
 *
* Defeating the merge is the load-bearing part of the setup, and getting it
 * wrong is silent: vma_can_merge() coalesces ADJACENT anonymous mappings that
 * agree on both vm_flags and pte_flags, so the obvious layouts all yield a
 * couple of records instead of a thousand.  One big span is one record; a
 * MAP_FIXED_NOREPLACE page inside it is EEXIST because the span is already
 * mapped; a PROT_NONE reservation has the slot colliding with the reservation,
 * which is itself a mapping; and plain back-to-back one-page mmaps come back
 * from this kernel's allocator CONTIGUOUS (gap 4096), so they all coalesce too.
 *
 * The layout that works is a PROT_NONE guard between every slot.  The guard is
 * what breaks adjacency: slot(PROT_READ|PROT_WRITE) and guard(PROT_NONE)
 * cannot merge, so the count rises by two per iteration instead of by nothing.
 * It does not depend on where the allocator puts anything, and the guards are
 * never touched, so they cost nothing at fault time.
 *
 * The second thing that had to be got right is WHEN the pages are touched, and
 * the first version got it wrong in a way that made the workload look like it
 * was working.  It memset each slot as it was created, so every page was already
 * faulted by the time the address space went over the cap -- the read-back loop
 * then touched nothing, no demand fault happened while over capacity, and the
 * gate's own non-zero assertion passed on a workload that had not actually
 * exercised the path it exists for.  Nothing is written here: the first touch of
 * every page happens in the read-back loop below, once all the mappings exist. */
#define MM_SEG_INDEX_CAPACITY_GUESS 1024
#define SEGOVF_TARGET (MM_SEG_INDEX_CAPACITY_GUESS + 64)
#define SEGOVF_PAGE_SIZE 4096

static int seg_index_overflow_workload(void)
{
    char *slot[SEGOVF_TARGET];
    for (int i = 0; i < SEGOVF_TARGET; i++) {
        char *got = mmap(NULL, SEGOVF_PAGE_SIZE, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (got == MAP_FAILED) {
            printf("MM_STRESS: segovf mmap %d: %s\n", i, strerror(errno));
            return fail("segovf-slot-mmap");
        }
        slot[i] = got;
        /* The guard goes immediately after this slot and before the next one is
         * allocated, so it is adjacent to both and separates both pairs. */
        void *guard = mmap((char *)got + SEGOVF_PAGE_SIZE, SEGOVF_PAGE_SIZE,
                           PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS
                                    | MAP_FIXED_NOREPLACE, -1, 0);
        if (guard == MAP_FAILED) {
            printf("MM_STRESS: segovf guard %d: %s\n", i, strerror(errno));
            return fail("segovf-guard-mmap");
        }
    }

    /* Now, and only now, touch every page.  The address space is already over
     * MM_SEG_INDEX_CAPACITY, so each first touch below is a demand fault whose
     * address has to be resolved through mm_seg_find()'s overflow path.
     *
     * A distinct value per mapping is what makes this a correctness check and
     * not just an exercise: if the over-capacity lookup ever resolved an address
     * to the WRONG record, the neighbouring guard is not what saves it, the
     * value read back belongs to a different mapping.  Two distinct bytes per
     * page, because a read-back that only ever sees one value would also pass
     * on a lookup that returned the same record every time. */
    for (int i = 0; i < SEGOVF_TARGET; i++) {
        memset(slot[i], 0, SEGOVF_PAGE_SIZE);
        slot[i][0] = (char)(i + 1);
        slot[i][SEGOVF_PAGE_SIZE - 1] = (char)(0xA0 + (i & 0x0F));
    }
    for (int i = 0; i < SEGOVF_TARGET; i++) {
        if (slot[i][0] != (char)(i + 1) ||
            slot[i][SEGOVF_PAGE_SIZE - 1] != (char)(0xA0 + (i & 0x0F))) {
            printf("MM_STRESS: segovf readback %d got %d/%d\n", i,
                   (int)(unsigned char)slot[i][0],
                   (int)(unsigned char)slot[i][SEGOVF_PAGE_SIZE - 1]);
            return fail("segovf-readback");
        }
    }

    /* Drop every other slot AND its guard, so the address space falls back
     * under the cap.  This is what makes the fix's memo sound rather than
     * merely fast: an address space that is over capacity stays over it, so the
     * rebuild is skipped until something invalidates the index -- and a munmap
     * is exactly that.  If the memo were keyed on "still over capacity" instead
     * of "not dirty", the index would never come back and every later lookup
     * would keep paying the list walk.
     *
     * Both halves matter.  The survivors must still resolve to their own
     * mapping (a lookup using a stale index would hand back one that no longer
     * exists), and the gap left by the removed pairs must not be resolved to
     * anything at all. */
    for (int i = 0; i < SEGOVF_TARGET; i += 2) {
        if (munmap(slot[i], 2 * SEGOVF_PAGE_SIZE) < 0)
            return fail("segovf-half-munmap");
    }

    /* The hole where a pair used to be: slot and guard are gone together, so
     * this is a two-page gap and must resolve to nothing. */
    for (int i = 0; i < SEGOVF_TARGET; i += 2) {
        char *probe = slot[i];
        if (mprotect(probe, 2 * SEGOVF_PAGE_SIZE, PROT_READ | PROT_WRITE) == 0)
            return fail("segovf-hole-still-mapped");
    }

    for (int i = 1; i < SEGOVF_TARGET; i += 2) {
        if (slot[i][0] != (char)(i + 1) ||
            slot[i][SEGOVF_PAGE_SIZE - 1] != (char)(0xA0 + (i & 0x0F)))
            return fail("segovf-survivor-readback");
        /* Drop and re-touch: a NEW demand fault on a mapping created before the
         * shrink, so the lookup has to resolve it again -- this time with the
         * index rebuilt rather than over capacity. */
        if (mprotect(slot[i], SEGOVF_PAGE_SIZE, PROT_READ) < 0)
            return fail("segovf-survivor-prot");
        if (mprotect(slot[i], SEGOVF_PAGE_SIZE,
                     PROT_READ | PROT_WRITE) < 0)
            return fail("segovf-survivor-unprot");
        slot[i][0] = (char)(i + 1);
        if (slot[i][0] != (char)(i + 1))
            return fail("segovf-survivor-rewrite");
    }

    for (int i = 1; i < SEGOVF_TARGET; i += 2)
        if (munmap(slot[i], 2 * SEGOVF_PAGE_SIZE) < 0)
            return fail("segovf-tail-munmap");
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
        size_t mismatch = 0;
        while (mismatch < sizeof(readback) &&
               mem[mismatch] == readback[mismatch])
            mismatch++;
        printf("MM_STRESS: m2r mismatch total=%ld index=%lu mapped=0x%x read=0x%x\n",
               (long)total, (unsigned long)mismatch,
               mismatch < sizeof(readback) ? (unsigned char)mem[mismatch] : 0,
               mismatch < sizeof(readback) ? (unsigned char)readback[mismatch] : 0);
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

/* mmap() hands out page-aligned addresses, and the kernel's THP fault only
 * fires when the 2 MiB window [hbase, hbase + 2 MiB) lies entirely inside
 * the VMA -- an unaligned 2 MiB mapping satisfies that for NO hbase, so
 * every huge phase this file used to have passed on 4K pages while the
 * huge-leaf path never executed (mm_huge_faults read 0 in every gate log).
 * Map TWICE the size and derive an aligned window from whatever base the
 * allocator returned, so the huge-leaf paths run regardless of its luck. */
static char *huge_aligned_map(size_t *len_out)
{
    size_t len = 2 * HPSIZE;
    char *mem = mmap(NULL, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (mem == MAP_FAILED)
        return MAP_FAILED;
    uintptr_t win = ((uintptr_t)mem + HPSIZE - 1) & ~(uintptr_t)(HPSIZE - 1);
    *len_out = len;
    return (char *)win;
}

static int huge_page_basic(void)
{
    /* Allocate a huge page, write a pattern, and verify readback. */
    unsigned long base_pins = read_page_cache_pinned();
    size_t len = 0;
    char *mem = huge_aligned_map(&len);
    if (mem == MAP_FAILED)
        return fail("huge-basic-mmap");

    for (size_t i = 0; i < HPSIZE; i++)
        mem[i] = (char)((i * 13) % 251);
    for (size_t i = 0; i < HPSIZE; i++) {
        if (mem[i] != (char)((i * 13) % 251)) {
            munmap(mem, len);
            return fail("huge-basic-verify");
        }
    }

    if (munmap(mem, len) < 0)
        return fail("huge-basic-munmap");
    if (read_page_cache_pinned() != base_pins)
        return fail("huge-basic-pinned-leak");
    return 0;
}

static int huge_page_fork_cow(void)
{
    /* Fork with a huge page mapping; child writes must not affect parent. */
    unsigned long base_pins = read_page_cache_pinned();
    size_t len = 0;
    char *mem = huge_aligned_map(&len);
    if (mem == MAP_FAILED)
        return fail("huge-fork-mmap");

    for (size_t i = 0; i < HPSIZE; i++)
        mem[i] = (char)((i * 17) % 251);

    pid_t pid = fork();
    if (pid < 0) {
        munmap(mem, len);
        return fail("huge-fork");
    }
    if (pid == 0) {
        for (size_t i = 0; i < HPSIZE; i++)
            mem[i] = (char)(~mem[i]);
        _exit(0);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        munmap(mem, len);
        return fail("huge-fork-wait");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        munmap(mem, len);
        return fail("huge-fork-child");
    }

    for (size_t i = 0; i < HPSIZE; i++) {
        if (mem[i] != (char)((i * 17) % 251)) {
            munmap(mem, len);
            return fail("huge-fork-verify");
        }
    }

    if (munmap(mem, len) < 0)
        return fail("huge-fork-munmap");
    if (read_page_cache_pinned() != base_pins)
        return fail("huge-fork-pinned-leak");
    return 0;
}

static int huge_page_partial_munmap(void)
{
    /* Partial munmap of a huge page forces demotion; remaining halves stay mapped. */
    unsigned long base_pins = read_page_cache_pinned();
    size_t len = 0;
    char *mem = huge_aligned_map(&len);
    if (mem == MAP_FAILED)
        return fail("huge-partial-mmap");

    for (size_t i = 0; i < HPSIZE; i++)
        mem[i] = (char)((i * 19) % 251);

    if (munmap(mem + HPSIZE / 2, 4096) < 0) {
        munmap(mem, len);
        return fail("huge-partial-munmap-middle");
    }

    for (size_t i = 0; i < HPSIZE / 2; i++) {
        if (mem[i] != (char)((i * 19) % 251)) {
            munmap(mem, len);
            return fail("huge-partial-verify-lo");
        }
    }
    for (size_t i = HPSIZE / 2 + 4096; i < HPSIZE; i++) {
        if (mem[i] != (char)((i * 19) % 251)) {
            munmap(mem, len);
            return fail("huge-partial-verify-hi");
        }
    }

    if (munmap(mem, HPSIZE / 2) < 0 ||
        munmap(mem + HPSIZE / 2 + 4096, len - HPSIZE / 2 - 4096) < 0) {
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
    size_t len = 0;
    char *mem = huge_aligned_map(&len);
    if (mem == MAP_FAILED)
        return fail("huge-prot-mmap");

    for (size_t i = 0; i < HPSIZE; i++)
        mem[i] = (char)((i * 23) % 251);

    if (mprotect(mem, HPSIZE, PROT_READ) < 0) {
        munmap(mem, len);
        return fail("huge-prot-mprotect");
    }

    for (size_t i = 0; i < HPSIZE; i++) {
        if (mem[i] != (char)((i * 23) % 251)) {
            munmap(mem, len);
            return fail("huge-prot-verify");
        }
    }

    if (munmap(mem, len) < 0)
        return fail("huge-prot-munmap");
    if (read_page_cache_pinned() != base_pins)
        return fail("huge-prot-pinned-leak");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 &&
        strcmp(argv[1], "--verify-vma-fork-exec-child") == 0)
        return verify_vma_fork_exec_child();

    if (argc == 2 && strcmp(argv[1], "--vma-fork-exec-only") == 0) {
        printf("MM_VMA_FORK_EXEC: start workers=%d rounds=%d\n",
               VMA_FORK_EXEC_WORKERS, VMA_FORK_EXEC_ROUNDS);
        if (fork_mprotect_cow() != 0 || concurrent_vma_fork_exec() != 0)
            return 1;
        printf("MM_VMA_FORK_EXEC: PASS\n");
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--seg-index-overflow-only") == 0) {
        printf("MM_SEG_INDEX_OVERFLOW: start mappings=%d\n", SEGOVF_TARGET);
        if (seg_index_overflow_workload() != 0)
            return 1;
        printf("MM_SEG_INDEX_OVERFLOW: PASS\n");
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--vma-race-only") == 0) {
        printf("MM_VMA_RACE: start workers=%d rounds=%d\n",
               VMA_RACE_WORKERS, VMA_RACE_ROUNDS);
        if (concurrent_vma_deferred_flush() != 0)
            return 1;
        printf("MM_VMA_RACE: PASS\n");
        return 0;
    }

    /* The wide-cursor workload spawns WIDE_CURSOR_WORKERS+1 threads that each
     * hold a cursor against one address range and tear a slice down underneath
     * the others.  It runs only from smoke-mm-pt-race at SMP=8, never from the
     * bare `mm_stress` run: at -smp 1 it does not finish inside the 45s budget
     * that smoke-mm-stress allows, and that regression is not worth paying for a
     * check the SMP gate already covers properly. */
    if (argc == 2 && strcmp(argv[1], "--wide-cursor-only") == 0) {
        printf("MM_WIDE_CURSOR: start workers=%d rounds=%d bytes=%d\n",
               WIDE_CURSOR_WORKERS, WIDE_CURSOR_ROUNDS, WIDE_CURSOR_BYTES);
        if (concurrent_wide_cursor_faults() != 0)
            return 1;
        return 0;
    }

    printf("MM_STRESS: start\n");
    printf("MM_STRESS: vma-deferred-race start\n");
    if (concurrent_vma_deferred_flush() != 0)
        return 1;
    printf("MM_STRESS: anon start\n");
    if (anonymous_fault_and_unmap() != 0)
        return 1;
    printf("MM_STRESS: fixed start\n");
    if (fixed_noreplace_conflict() != 0)
        return 1;
    /* In the default run too, not only behind --seg-index-overflow-only: this
     * is the only workload in the tree that puts an address space over
     * MM_SEG_INDEX_CAPACITY, so running it only in its own case would leave the
     * two big aggregate gates reading 0 for mm_seg_index_overflow and proving
     * nothing. */
    printf("MM_STRESS: seg-index-overflow start\n");
    if (seg_index_overflow_workload() != 0)
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
    printf("MM_STRESS: mseal start\n");
    if (mseal_semantics() != 0)
        return 1;
    printf("MM_STRESS: PASS\n");
    return 0;
}
