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
#include <sys/syscall.h>
#include <sys/uio.h>
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
static char *usercopy_race_base;
static volatile int usercopy_race_ready;
static volatile int usercopy_race_start;
static volatile int usercopy_race_unmaps;
static volatile int usercopy_race_unmapped;
static volatile int usercopy_race_resume;
static volatile int usercopy_race_remapped;
static volatile int usercopy_race_stress;
static volatile int usercopy_race_failed;

static int process_vm_race_pin_cpu(unsigned cpu)
{
    unsigned char mask[8] = {0};
    if (cpu >= sizeof(mask) * 8)
        return -1;
    mask[cpu / 8] = (unsigned char)(1U << (cpu % 8));
    for (int attempt = 0; attempt < 10000; attempt++) {
        if (syscall(SYS_sched_setaffinity, 0, sizeof(mask), mask) == 0)
            return 0;
        if (errno != EINVAL && errno != ESRCH)
            return -1;
        syscall(SYS_sched_yield);
    }
    return -1;
}

static int process_vm_race_wait_child(pid_t pid)
{
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

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

static long self_rss_pages(void)
{
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0) return -1;
    char buf[96];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    unsigned long total = 0, rss = 0;
    if (sscanf(buf, "%lu %lu", &total, &rss) != 2) return -1;
    return (long)rss;
}

/* Prime the parent-side fork/wait and procfs paths before RSS accounting.
 * A deeper stack page first touched by waitpid() after the measurements begin
 * would otherwise look like a page that mremap failed to release.  Requiring
 * the second cycle to hold the same RSS also detects a per-cycle residue. */
static int dontunmap_warm_rss_path(void)
{
    volatile unsigned char stack_warm[8 * 4096];
    for (size_t i = 0; i < sizeof(stack_warm); i += 4096)
        stack_warm[i] = (unsigned char)i;
    (void)stack_warm[0];

    long previous = self_rss_pages();
    if (previous < 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        pid_t pid = fork();
        if (pid < 0)
            return -1;
        if (pid == 0)
            _exit(0);
        int status = 0;
        if (waitpid(pid, &status, 0) != pid ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return -1;
        long current = self_rss_pages();
        if (current < 0 || (i != 0 && current != previous))
            return -1;
        previous = current;
    }
    return 0;
}

/* Linux MREMAP_DONTUNMAP keeps the original VMA but drops its PTEs.  The
 * destination retains the populated pages; the original range faults back
 * independently, and fork/COW must preserve the same split. */
static int private_anon_mremap_dontunmap(void)
{
    const size_t len = 2 * 4096;
    /* Touch the test's deepest userspace stack page before taking RSS
     * baselines so later helper calls do not look like leaked resident pages. */
    volatile unsigned char rss_stack_warm[8 * 4096];
    for (size_t i = 0; i < sizeof(rss_stack_warm); i += 4096)
        rss_stack_warm[i] = (unsigned char)i;
    (void)rss_stack_warm[0];
    if (dontunmap_warm_rss_path() < 0)
        return fail("dontunmap-rss-warm-path");
    unsigned char *mem = mmap(NULL, len, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return fail("dontunmap-anon-mmap");
    long rss_base = self_rss_pages();
    if (rss_base < 0) {
        munmap(mem, len);
        return fail("dontunmap-rss-read-before");
    }
    mem[0] = 0x21;
    mem[4095] = 0x22;
    mem[4096] = 0x31;
    mem[8191] = 0x32;
    long rss_source = self_rss_pages();
    if (rss_source < rss_base + 2) {
        munmap(mem, len);
        return fail("dontunmap-rss-source-populated");
    }

    errno = 0;
    if (mremap(mem, len, len, MREMAP_DONTUNMAP) != MAP_FAILED || errno != EINVAL) {
        munmap(mem, len);
        return fail("dontunmap-requires-maymove");
    }
    errno = 0;
    if (mremap(mem, len, len + 4096, MREMAP_MAYMOVE | MREMAP_DONTUNMAP) != MAP_FAILED ||
        errno != EINVAL) {
        munmap(mem, len);
        return fail("dontunmap-equal-length-required");
    }
    errno = 0;
    if (mremap(mem, len, len, MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP,
               mem + 4096) != MAP_FAILED || errno != EINVAL) {
        munmap(mem, len);
        return fail("dontunmap-overlap-rejected");
    }
    if (mem[0] != 0x21 || mem[4096] != 0x31) {
        munmap(mem, len);
        return fail("dontunmap-invalid-request-preserved-source");
    }

    unsigned char *moved = mremap(mem, len, len,
                                 MREMAP_MAYMOVE | MREMAP_DONTUNMAP);
    if (moved == MAP_FAILED || moved == mem) {
        munmap(mem, len);
        return fail("dontunmap-anon-remap");
    }
    if (moved[0] != 0x21 || moved[4095] != 0x22 ||
        moved[4096] != 0x31 || moved[8191] != 0x32) {
        munmap(moved, len); munmap(mem, len);
        return fail("dontunmap-anon-destination-retains-data");
    }
    long rss_after_move = self_rss_pages();
    /* Reads are the first access to the source after remap and therefore
     * exercise the real fault path. */
    int source_zero = mem[0] == 0 && mem[4095] == 0 &&
                      mem[4096] == 0 && mem[8191] == 0;
    int rss_conserved = rss_after_move == rss_source;
    if (!source_zero || !rss_conserved) {
        printf("MM_DONTUNMAP_ANON: source_zero=%d rss_source=%ld rss_after_move=%ld\n",
               source_zero, rss_source, rss_after_move);
        munmap(moved, len); munmap(mem, len);
        return fail("dontunmap-source-refault-and-rss");
    }
    if (moved[0] != 0x21 || moved[4096] != 0x31) {
        munmap(moved, len); munmap(mem, len);
        return fail("dontunmap-source-refault-independent");
    }
    long rss_after_refault = self_rss_pages();
    if (rss_after_refault != rss_after_move + 2) {
        printf("MM_DONTUNMAP_ANON: refault_rss source=%ld moved=%ld refault=%ld\n",
               rss_source, rss_after_move, rss_after_refault);
        munmap(moved, len); munmap(mem, len);
        return fail("dontunmap-rss-after-source-refault");
    }
    mem[0] = 0x44;
    moved[4096] = 0x55;
    if (mem[4096] != 0 || moved[0] != 0x21 || moved[4096] != 0x55) {
        munmap(moved, len); munmap(mem, len);
        return fail("dontunmap-write-independence");
    }

    pid_t pid = fork();
    if (pid < 0) {
        munmap(moved, len); munmap(mem, len);
        return fail("dontunmap-fork");
    }
    if (pid == 0) {
        int ok = mem[0] == 0x44 && mem[4096] == 0 &&
                 moved[0] == 0x21 && moved[4096] == 0x55;
        mem[0] = 0x66;
        moved[0] = 0x77;
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        mem[0] != 0x44 || moved[0] != 0x21) {
        munmap(moved, len); munmap(mem, len);
        return fail("dontunmap-fork-cow");
    }

    long rss_after_child = self_rss_pages();
    if (rss_after_child < 0 || rss_after_child != rss_after_refault) {
        printf("MM_DONTUNMAP_ANON: child_rss refault=%ld after_child=%ld\n",
               rss_after_refault, rss_after_child);
        munmap(moved, len); munmap(mem, len);
        return fail("dontunmap-child-rss-stable");
    }

    if (munmap(mem, len) < 0 || munmap(moved, len) < 0)
        return fail("dontunmap-anon-unmap");
    long rss_end = self_rss_pages();
    if (rss_end < 0 || rss_end != rss_after_refault - 4) {
        printf("MM_DONTUNMAP_ANON: cleanup_rss refault=%ld end=%ld\n",
               rss_after_refault, rss_end);
        return fail("dontunmap-rss-cleanup");
    }
    printf("MM_DONTUNMAP_ANON: PASS rss_pages=%ld->%ld->%ld->%ld->%ld->%ld\n",
           rss_base, rss_source, rss_after_move, rss_after_refault,
           rss_after_child, rss_end);
    return 0;
}

/* A fixed target immediately before the source can be merged with that source
 * by mmap insertion. The transaction must reacquire the live source VMA after
 * splitting destination boundaries instead of carrying a potentially retired
 * source-record pointer across destination allocation. */
static int dontunmap_adjacent_destination_before_source(void)
{
    const size_t len = 2 * 4096;
    unsigned char *region = mmap(NULL, 2 * len, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED)
        return fail("dontunmap-adjacent-reserve");
    unsigned char *source = region + len;
    source[0] = 0x41;
    source[4096] = 0x52;
    source[len - 1] = 0x63;
    if (munmap(region, len) < 0) {
        munmap(region, 2 * len);
        return fail("dontunmap-adjacent-target-release");
    }

    unsigned char *destination = mremap(source, len, len,
        MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, region);
    if (destination != region) {
        if (destination != MAP_FAILED) munmap(destination, len);
        munmap(source, len);
        return fail("dontunmap-adjacent-remap");
    }
    if (destination[0] != 0x41 || destination[4096] != 0x52 ||
        destination[len - 1] != 0x63 || source[0] != 0 ||
        source[4096] != 0 || source[len - 1] != 0) {
        munmap(source, len);
        munmap(destination, len);
        return fail("dontunmap-adjacent-source-destination");
    }
    if (munmap(source, len) < 0 || munmap(destination, len) < 0)
        return fail("dontunmap-adjacent-cleanup");
    printf("MM_DONTUNMAP_ADJACENT: PASS source_record_reacquired=1\n");
    return 0;
}

static int private_anon_mremap_dontunmap_split_failure(unsigned int split_call)
{
    const size_t len = 2 * 4096;
    const size_t region_len = split_call >= 5 ? 8 * len : 4 * len;
    unsigned char *region = mmap(NULL, region_len, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED)
        return fail("dontunmap-split-region-mmap");

    /* Keep same-attribute neighbors on both sides so destination insertion
     * can coalesce VMAs. In the observed merge order, calls 3 and 4 are
     * already-boundary no-ops in both layouts; calls 1/2 and the source
     * pre-splits 5/6 are the allocation sites. */
    size_t source_off = split_call == 4 ? 2 * len : len;
    size_t target_off = split_call == 4 ? len : 2 * len;
    if (split_call >= 5) {
        source_off = 2 * len;
        target_off = 5 * len;
    }
    unsigned char *source = region + source_off;
    unsigned char *target = region + target_off;
    for (size_t off = 0; off < region_len; off += 4096)
        region[off] = (unsigned char)(0x20 + (off / 4096));
    unsigned char expected0 = source[0];
    unsigned char expected1 = source[4096];
    unsigned char expected_last = source[len - 1];

    if (munmap(target, len) < 0) {
        munmap(region, region_len);
        return fail("dontunmap-split-target-unmap");
    }
    volatile unsigned char rss_stack_warm[8 * 4096];
    for (size_t i = 0; i < sizeof(rss_stack_warm); i += 4096)
        rss_stack_warm[i] = (unsigned char)i;
    (void)rss_stack_warm[0];
    long stable_rss = self_rss_pages();
    if (stable_rss < 0) {
        munmap(region, region_len);
        return fail("dontunmap-split-rss-warm");
    }
    int stable_samples = 0;
    for (int i = 0; i < 8 && stable_samples < 2; i++) {
        long sample = self_rss_pages();
        if (sample < 0) {
            munmap(region, region_len);
            return fail("dontunmap-split-rss-warm-repeat");
        }
        if (sample == stable_rss)
            stable_samples++;
        else {
            stable_rss = sample;
            stable_samples = 0;
        }
    }
    if (stable_samples < 2) {
        munmap(region, region_len);
        return fail("dontunmap-split-rss-never-stabilized");
    }
    long rss_before = self_rss_pages();
    if (rss_before < 0) {
        munmap(region, region_len);
        return fail("dontunmap-split-rss-before");
    }

    /* The source pre-splits plus destination boundary splits leave source
     * start/end as no-ops in these layouts. Keep explicit negative controls:
     * the hook must report would_allocate=0, must not consume the one-shot
     * injection, and the ordinary transaction must still succeed. */
    if (split_call == 3 || split_call == 4) {
        unsigned char *moved = mremap(source, len, len,
            MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, target);
        if (moved != target || moved[0] != expected0 ||
            moved[4096] != expected1 || moved[len - 1] != expected_last ||
            source[0] != 0 || source[4096] != 0 || source[len - 1] != 0) {
            if (moved != MAP_FAILED) munmap(moved, len);
            munmap(region, region_len);
            return fail("dontunmap-split-noop-source-start");
        }
        if (munmap(region, region_len) < 0)
            return fail("dontunmap-split-noop-cleanup");
        printf("MM_DONTUNMAP_SPLIT_NOOP: PASS split=%u allocation_not_reached=1\n",
               split_call);
        return 0;
    }

    errno = 0;
    void *result = mremap(source, len, len,
        MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, target);
    if (result != MAP_FAILED || errno != ENOMEM) {
        if (result != MAP_FAILED) munmap(result, len);
        munmap(region, region_len);
        return fail("dontunmap-split-injected-ENOMEM");
    }
    if (source[0] != expected0 || source[4096] != expected1 ||
        source[len - 1] != expected_last) {
        munmap(region, region_len);
        return fail("dontunmap-split-source-data");
    }
    unsigned char writable0 = (unsigned char)(expected0 ^ 0x5a);
    unsigned char writable1 = (unsigned char)(expected1 ^ 0xa5);
    source[0] = writable0;
    source[4096] = writable1;
    long rss_after = self_rss_pages();
    if (rss_after != rss_before) {
        printf("MM_DONTUNMAP_SPLIT_FAILURE: rss_before=%ld rss_after=%ld split=%u\n",
               rss_before, rss_after, split_call);
        munmap(region, region_len);
        return fail("dontunmap-split-source-rss");
    }

    void *probe = mmap(target, len, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (probe != target) {
        if (probe != MAP_FAILED) munmap(probe, len);
        munmap(region, region_len);
        return fail("dontunmap-split-destination-leaked");
    }
    if (munmap(probe, len) < 0) {
        munmap(region, region_len);
        return fail("dontunmap-split-destination-probe-unmap");
    }

    printf("MM_DONTUNMAP_SPLIT_RETRY: begin split=%u\n", split_call);
    unsigned char *moved = mremap(source, len, len,
        MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP, target);
    if (moved != target || moved[0] != writable0 ||
        moved[4096] != writable1 || moved[len - 1] != expected_last ||
        source[0] != 0 || source[4096] != 0 || source[len - 1] != 0) {
        if (moved != MAP_FAILED) munmap(moved, len);
        munmap(region, region_len);
        return fail("dontunmap-split-same-process-retry");
    }
    for (size_t off = 0; off < region_len; off += 4096) {
        if (off >= target_off && off < target_off + len)
            continue;
        if (off >= source_off && off < source_off + len)
            continue;
        if (region[off] != (unsigned char)(0x20 + (off / 4096))) {
            munmap(region, region_len);
            return fail("dontunmap-split-neighbor-data");
        }
    }
    if (munmap(region, region_len) < 0)
        return fail("dontunmap-split-cleanup");
    printf("MM_DONTUNMAP_SPLIT_FAILURE: PASS split=%u source_preserved=1 destination_cleared=1 retry=1 rss=%ld\n",
           split_call, rss_before);
    return 0;
}

/* Exercise usercopy's public syscall-visible partial-copy contract before the
 * copy loop is moved behind the Rust selector.  The first case commits one
 * 64 KiB file-write chunk, then faults halfway through the next user buffer
 * chunk; the second makes copy_to_user fault on its second page after copying
 * the first. */
static void *usercopy_unmapper(void *arg)
{
    const size_t page = 4096;
    (void)arg;
    __atomic_store_n(&usercopy_race_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&usercopy_race_start, __ATOMIC_ACQUIRE))
        sched_yield();
    char *tail = usercopy_race_base + page;
    if (munmap(tail, page) < 0) {
        __atomic_store_n(&usercopy_race_failed, 1, __ATOMIC_RELEASE);
        return (void *)(intptr_t)1;
    }
    __atomic_store_n(&usercopy_race_unmapped, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&usercopy_race_resume, __ATOMIC_ACQUIRE))
        sched_yield();
    void *again = mmap(tail, page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (again == MAP_FAILED || again != tail) {
        __atomic_store_n(&usercopy_race_failed, 1, __ATOMIC_RELEASE);
        return (void *)(intptr_t)1;
    }
    memset(again, 0, page);
    __atomic_add_fetch(&usercopy_race_unmaps, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&usercopy_race_remapped, 1, __ATOMIC_RELEASE);
    /* Keep this restored mapping stable until the main thread proves a full
     * usercopy succeeds; only then begin the probabilistic stress rounds. */
    while (!__atomic_load_n(&usercopy_race_stress, __ATOMIC_ACQUIRE))
        sched_yield();

    puts("DIAG: worker-loop-begin"); fflush(stdout);
    for (int i = 1; i < 128; i++) {
        if (munmap(tail, page) < 0) {
            __atomic_store_n(&usercopy_race_failed, 1, __ATOMIC_RELEASE);
            return (void *)(intptr_t)1;
        }
        /* Keep an observable interval where copy_from_user has to fail or
         * where the retry path must wait for the next mapping. */
        sched_yield();
        again = mmap(tail, page, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (again == MAP_FAILED || again != tail) {
            __atomic_store_n(&usercopy_race_failed, 1, __ATOMIC_RELEASE);
            return (void *)(intptr_t)1;
        }
        memset(again, i, page);
        __atomic_add_fetch(&usercopy_race_unmaps, 1, __ATOMIC_RELEASE);
        if ((i % 32) == 0) { printf("DIAG: worker-unmap-%d\n", i); fflush(stdout); }
    }
    puts("DIAG: worker-loop-done"); fflush(stdout);
    return NULL;
}

/* Exercise the string helpers through syscall entry points: pathname copy
 * reaches user_strncpy(), and exec argument admission reaches user_strnlen(). */
static int usercopy_strings_syscall_contract(void)
{
    const size_t page = 4096;
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || close(fd) < 0)
        return fail("usercopy-strings-pathname-copy");

    char *path_fault = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (path_fault == MAP_FAILED)
        return fail("usercopy-strings-path-fault-mmap");
    memset(path_fault, 'p', 2 * page);
    if (mprotect(path_fault + page, page, PROT_NONE) < 0) {
        munmap(path_fault, 2 * page);
        return fail("usercopy-strings-path-fault-protect");
    }
    errno = 0;
    fd = open(path_fault + page - 8, O_RDONLY);
    int path_errno = errno;
    if (fd >= 0) close(fd);
    if (munmap(path_fault, 2 * page) < 0 || fd >= 0 || path_errno != EFAULT)
        return fail("usercopy-strings-path-fault-boundary");

    /* A NUL in the last mapped byte must let exec argument length scanning
     * stop without touching the protected following page. */
    char *arg = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arg == MAP_FAILED)
        return fail("usercopy-strings-page-end-mmap");
    memset(arg, 'a', page);
    arg[page - 1] = '\0';
    if (mprotect(arg + page, page, PROT_NONE) < 0) {
        munmap(arg, 2 * page);
        return fail("usercopy-strings-page-end-protect");
    }
    pid_t child = fork();
    if (child < 0) {
        munmap(arg, 2 * page);
        return fail("usercopy-strings-page-end-fork");
    }
    if (child == 0) {
        char *argv[] = {arg, NULL};
        char *envp[] = {NULL};
        execve("/bin/echo", argv, envp);
        _exit(100 + (errno & 0x3f));
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 || munmap(arg, 2 * page) < 0)
        return fail("usercopy-strings-page-end-exec");

    /* exec_copy_args scans at most 128KiB-1 bytes. Arrange the rejected next
     * byte at a protected page boundary; E2BIG proves it was not probed. */
    const size_t arg_limit = 128 * 1024;
    char *large = mmap(NULL, arg_limit + page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (large == MAP_FAILED)
        return fail("usercopy-strings-limit-mmap");
    memset(large, 'b', arg_limit + page);
    char *bounded = large + 1;
    if (mprotect(large + arg_limit, page, PROT_NONE) < 0) {
        munmap(large, arg_limit + page);
        return fail("usercopy-strings-limit-protect");
    }
    child = fork();
    if (child < 0) {
        munmap(large, arg_limit + page);
        return fail("usercopy-strings-limit-fork");
    }
    if (child == 0) {
        char *argv[] = {bounded, NULL};
        char *envp[] = {NULL};
        execve("/bin/echo", argv, envp);
        _exit(errno == E2BIG ? 42 : 100 + (errno & 0x3f));
    }
    status = 0;
    int wait_ok = waitpid(child, &status, 0) == child;
    int cleanup_ok = munmap(large, arg_limit + page) == 0;
    if (!wait_ok || !WIFEXITED(status) || WEXITSTATUS(status) != 42 || !cleanup_ok)
        return fail("usercopy-strings-limit-no-overread");

    puts("MM_USERCOPY_STRINGS: PASS pathname=1 page_end_nul=1 arg_limit=E2BIG");
    return 0;
}

static int usercopy_partial_contract(void)
{
    puts("DIAG: partial-begin"); fflush(stdout);
    const size_t page = 4096;
    const size_t io_chunk = 65536;
    const size_t src_len = io_chunk + 2 * page;
    char path[96];
    snprintf(path, sizeof(path), "/tmp/mm-usercopy-contract-%ld", (long)getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0)
        return fail("usercopy-contract-open");

    unsigned char *src = mmap(NULL, src_len, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (src == MAP_FAILED) {
        close(fd); unlink(path);
        return fail("usercopy-contract-source-mmap");
    }
    for (size_t i = 0; i < src_len; i++)
        src[i] = (unsigned char)(i * 37u + 11u);
    if (mprotect(src + io_chunk + page, page, PROT_NONE) < 0) {
        munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-source-protect");
    }
    errno = 0;
    ssize_t written = write(fd, src, src_len);
    struct stat st;
    if (written != (ssize_t)io_chunk || fstat(fd, &st) < 0 ||
        st.st_size != (off_t)io_chunk) {
        munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-from-user-partial-write");
    }

    unsigned char *verify = mmap(NULL, io_chunk, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (verify == MAP_FAILED || pread(fd, verify, io_chunk, 0) != (ssize_t)io_chunk) {
        if (verify != MAP_FAILED) munmap(verify, io_chunk);
        munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-write-prefix-readback");
    }
    for (size_t i = 0; i < io_chunk; i++) {
        if (verify[i] != (unsigned char)(i * 37u + 11u)) {
            munmap(verify, io_chunk); munmap(src, src_len); close(fd); unlink(path);
            return fail("usercopy-contract-write-prefix-data");
        }
    }
    munmap(verify, io_chunk);
    puts("DIAG: partial-write-readback-done"); fflush(stdout);

    if (mprotect(src + io_chunk + page, page, PROT_READ | PROT_WRITE) < 0 ||
        ftruncate(fd, (off_t)(2 * page)) < 0 ||
        pwrite(fd, src, 2 * page, 0) != (ssize_t)(2 * page)) {
        munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-copyto-setup");
    }
    unsigned char *dst = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (dst == MAP_FAILED) {
        munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-destination-mmap");
    }
    memset(dst, 0xcc, 2 * page);
    if (mprotect(dst + page, page, PROT_NONE) < 0) {
        munmap(dst, 2 * page); munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-destination-protect");
    }
    errno = 0;
    ssize_t read_result = pread(fd, dst, 2 * page, 0);
    if (read_result != -1 || errno != EFAULT) {
        munmap(dst, 2 * page); munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-to-user-partial-status");
    }
    for (size_t i = 0; i < page; i++) {
        if (dst[i] != (unsigned char)(i * 37u + 11u)) {
            munmap(dst, 2 * page); munmap(src, src_len); close(fd); unlink(path);
            return fail("usercopy-contract-to-user-prefix-data");
        }
    }
    if (mprotect(dst + page, page, PROT_READ | PROT_WRITE) < 0) {
        munmap(dst, 2 * page); munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-destination-unprotect");
    }
    for (size_t i = page; i < 2 * page; i++) {
        if (dst[i] != 0xcc) {
            munmap(dst, 2 * page); munmap(src, src_len); close(fd); unlink(path);
            return fail("usercopy-contract-to-user-no-later-write");
        }
    }
    errno = 0;
    read_result = pread(fd, (void *)(uintptr_t)(UINTPTR_MAX - 3), 8, 0);
    if (read_result != -1 || errno != EFAULT) {
        munmap(dst, 2 * page); munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-range-overflow");
    }

    /* Untouched anonymous destination: pread must fault it in through
     * copy_to_user, release the MM lock, resolve, and retry. */
    unsigned char *demand = mmap(NULL, page, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (demand == MAP_FAILED || pread(fd, demand, page, 0) != (ssize_t)page) {
        if (demand != MAP_FAILED) munmap(demand, page);
        munmap(dst, 2 * page); munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-demand-page");
    }
    for (size_t i = 0; i < page; i++) {
        if (demand[i] != (unsigned char)(i * 37u + 11u)) {
            munmap(demand, page); munmap(dst, 2 * page);
            munmap(src, src_len); close(fd); unlink(path);
            return fail("usercopy-contract-demand-page-data");
        }
    }

    puts("DIAG: demand-page-done"); fflush(stdout);

    /* Fork leaves a writable anonymous page COW-shared. The child's pread
     * takes the copy_to_user write-fault path; the parent's byte pattern must
     * remain unchanged after the child exits. */
    unsigned char *cow = mmap(NULL, page, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cow == MAP_FAILED) {
        munmap(demand, page); munmap(dst, 2 * page);
        munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-cow-mmap");
    }
    memset(cow, 0x55, page);
    pid_t child = fork();
    if (child < 0) {
        munmap(cow, page); munmap(demand, page); munmap(dst, 2 * page);
        munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-cow-fork");
    }
    if (child == 0) {
        if (pread(fd, cow, page, 0) != (ssize_t)page)
            _exit(1);
        for (size_t i = 0; i < page; i++)
            if (cow[i] != (unsigned char)(i * 37u + 11u))
                _exit(2);
        _exit(0);
    }
    int child_status = 0;
    if (waitpid(child, &child_status, 0) != child || !WIFEXITED(child_status) ||
        WEXITSTATUS(child_status) != 0) {
        munmap(cow, page); munmap(demand, page); munmap(dst, 2 * page);
        munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-cow-child");
    }
    for (size_t i = 0; i < page; i++) {
        if (cow[i] != 0x55) {
            munmap(cow, page); munmap(demand, page); munmap(dst, 2 * page);
            munmap(src, src_len); close(fd); unlink(path);
            return fail("usercopy-contract-cow-parent-isolation");
        }
    }

    puts("DIAG: fork-cow-done"); fflush(stdout);

    /* Race real copy_from_user traffic against same-mm unmap/remap of the
     * second page. A request may finish before unmap or fail EFAULT while the
     * range is absent; other return values violate the existing contract. */
    puts("DIAG: unmap-race-begin"); fflush(stdout);
    usercopy_race_base = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    puts("DIAG: race-mmap-returned"); fflush(stdout);
    if (usercopy_race_base == MAP_FAILED) {
        munmap(cow, page); munmap(demand, page); munmap(dst, 2 * page);
        munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-race-mmap");
    }
    memset(usercopy_race_base, 0x31, 2 * page);
    puts("DIAG: race-memset-done"); fflush(stdout);
    usercopy_race_ready = usercopy_race_start = usercopy_race_unmaps = 0;
    usercopy_race_unmapped = usercopy_race_resume = 0;
    usercopy_race_remapped = usercopy_race_stress = usercopy_race_failed = 0;
    pthread_t unmapper;
    int thread_error = pthread_create(&unmapper, NULL, usercopy_unmapper, NULL);
    puts("DIAG: pthread-create-returned"); fflush(stdout);
    if (thread_error != 0) {
        errno = thread_error;
        munmap(usercopy_race_base, 2 * page); munmap(cow, page);
        munmap(demand, page); munmap(dst, 2 * page);
        munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-race-thread");
    }
    while (!__atomic_load_n(&usercopy_race_ready, __ATOMIC_ACQUIRE))
        sched_yield();
    puts("DIAG: worker-ready"); fflush(stdout);
    __atomic_store_n(&usercopy_race_start, 1, __ATOMIC_RELEASE);
    puts("DIAG: worker-started"); fflush(stdout);
    while (!__atomic_load_n(&usercopy_race_unmapped, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&usercopy_race_failed, __ATOMIC_ACQUIRE))
        sched_yield();
    puts("DIAG: first-unmap-observed"); fflush(stdout);
    errno = 0;
    ssize_t absent_result = pwrite(fd, usercopy_race_base, 2 * page, 0);
    puts("DIAG: absent-pwrite-returned"); fflush(stdout);
    int absent_errno = errno;
    __atomic_store_n(&usercopy_race_resume, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&usercopy_race_remapped, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&usercopy_race_failed, __ATOMIC_ACQUIRE))
        sched_yield();
    ssize_t restored_result = pwrite(fd, usercopy_race_base, 2 * page, 0);
    puts("DIAG: restored-pwrite-returned"); fflush(stdout);
    __atomic_store_n(&usercopy_race_stress, 1, __ATOMIC_RELEASE);
    puts("DIAG: stress-released"); fflush(stdout);
    if (absent_result != -1 || absent_errno != EFAULT ||
        restored_result != (ssize_t)(2 * page)) {
        pthread_join(unmapper, NULL);
        munmap(usercopy_race_base, 2 * page); munmap(cow, page);
        munmap(demand, page); munmap(dst, 2 * page);
        munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-race-handshake");
    }
    unsigned full = 1, fault = 1;
    for (unsigned i = 0; i < 128; i++) {
        errno = 0;
        if (i == 0 || (i % 32) == 0 || i == 127) { printf("DIAG: pwrite-%u-before\n", i); fflush(stdout); }
        ssize_t copied = pwrite(fd, usercopy_race_base, 2 * page, 0);
        if (i == 0 || (i % 32) == 0 || i == 127) { printf("DIAG: pwrite-%u-after\n", i); fflush(stdout); }
        if (copied == (ssize_t)(2 * page))
            full++;
        else if (copied < 0 && errno == EFAULT)
            fault++;
        else {
            pthread_join(unmapper, NULL);
            munmap(usercopy_race_base, 2 * page); munmap(cow, page);
            munmap(demand, page); munmap(dst, 2 * page);
            munmap(src, src_len); close(fd); unlink(path);
            return fail("usercopy-contract-race-result");
        }
    }
    void *unmapper_result = NULL;
    puts("DIAG: pwrite-loop-done"); fflush(stdout);
    puts("DIAG: worker-join-begin"); fflush(stdout);
    if (pthread_join(unmapper, &unmapper_result) != 0 || unmapper_result != NULL ||
        usercopy_race_unmaps != 128) {
        munmap(usercopy_race_base, 2 * page); munmap(cow, page);
        munmap(demand, page); munmap(dst, 2 * page);
        munmap(src, src_len); close(fd); unlink(path);
        return fail("usercopy-contract-race-join");
    }
    puts("DIAG: worker-join-done"); fflush(stdout);

    puts("DIAG: unmap-race-done"); fflush(stdout);
    if (munmap(usercopy_race_base, 2 * page) < 0 || munmap(cow, page) < 0 ||
        munmap(demand, page) < 0 || munmap(dst, 2 * page) < 0 ||
        munmap(src, src_len) < 0 ||
        close(fd) < 0 || unlink(path) < 0)
        return fail("usercopy-contract-cleanup");
    puts("DIAG: string-contract-begin"); fflush(stdout);
    if (usercopy_strings_syscall_contract() != 0)
        return 1;
    puts("DIAG: string-contract-done"); fflush(stdout);
    printf("MM_USERCOPY_CONTRACT: PASS partial_return=EFAULT copied_prefix=1 range_overflow=EFAULT demand_retry=1 cow_retry=1 cow_isolation=1 concurrent_unmap=128 full=%u fault=%u\n",
           full, fault);
    return 0;
}

static int process_vm_contract(void)
{
    puts("DIAG: process-vm-begin"); fflush(stdout);
    const size_t page = 4096;
    unsigned char *remote = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *local = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (remote == MAP_FAILED || local == MAP_FAILED)
        return fail("process-vm-mmap");
    memset(remote, 0x39, page);
    memset(local, 0, 2 * page);
    if (mprotect(remote + page, page, PROT_NONE) < 0)
        return fail("process-vm-protect");

    /* The mismatched iovec boundaries force the implementation to advance
     * both sides independently. The inaccessible second remote range should
     * return the four bytes already transferred across two local iovecs. */
    struct iovec local_read[2] = {{local, 2}, {local + 2, 6}};
    struct iovec remote_read[2] = {{remote, 4}, {remote + page, 4}};
    long n = syscall(SYS_process_vm_readv, getpid(), local_read, 2,
                     remote_read, 2, 0);
    if (n != 4 || memcmp(local, remote, 4) != 0)
        return fail("process-vm-readv-partial-iovec");

    const unsigned char input[8] = {0x91, 0x82, 0x73, 0x64, 0x55, 0x46, 0x37, 0x28};
    struct iovec local_write[2] = {{(void *)input, 2}, {(void *)(input + 2), 6}};
    struct iovec remote_write[2] = {{remote, 4}, {remote + page, 4}};
    n = syscall(SYS_process_vm_writev, getpid(), local_write, 2,
                remote_write, 2, 0);
    if (n != 4 || memcmp(remote, input, 4) != 0)
        return fail("process-vm-writev-partial-iovec");

    munmap(local, 2 * page);
    munmap(remote, 2 * page);
    puts("DIAG: process-vm-done"); fflush(stdout);
    puts("MM_PROCESS_VM: PASS readv=partial writev=partial iovec=advance");
    return 0;
}

static int process_vm_exec_cow_race(void)
{
    unsigned char *remote = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (remote == MAP_FAILED)
        return fail("process-vm-exec-cow-mmap");
    remote[0] = 0x39;

    int gate[2], ready[2], exec_note[2], release[2];
    if (pipe(gate) || pipe(ready) || pipe(exec_note) || pipe(release))
        return fail("process-vm-exec-cow-pipe");
    unsigned char cpu0[8] = {1};
    if (syscall(SYS_sched_setaffinity, 0, sizeof(cpu0), cpu0) < 0)
        return fail("process-vm-exec-cow-pin-parent");

    pid_t child = fork();
    if (child < 0)
        return fail("process-vm-exec-cow-fork");
    if (child == 0) {
        close(gate[1]);
        close(ready[0]);
        close(exec_note[0]);
        close(release[1]);
        if (process_vm_race_pin_cpu(1) < 0 || write(ready[1], "R", 1) != 1)
            _exit(90);
        char start;
        if (read(gate[0], &start, 1) != 1)
            _exit(91);
        char ready_fd[16], release_fd[16];
        snprintf(ready_fd, sizeof(ready_fd), "%d", exec_note[1]);
        snprintf(release_fd, sizeof(release_fd), "%d", release[0]);
        char *child_argv[] = {"/bin/mm_stress", "--process-vm-exec-cow-child",
                              ready_fd, release_fd, NULL};
        execv("/bin/mm_stress", child_argv);
        _exit(92);
    }

    close(gate[0]);
    close(ready[1]);
    close(exec_note[1]);
    close(release[0]);
    char byte;
    if (read(ready[0], &byte, 1) != 1 || byte != 'R')
        return fail("process-vm-exec-cow-child-ready");
    if (write(gate[1], "G", 1) != 1)
        return fail("process-vm-exec-cow-release-child");

    const unsigned char value = 0xa7;
    struct iovec local = {(void *)&value, 1};
    struct iovec remote_iov = {remote, 1};
    long n = syscall(SYS_process_vm_writev, child, &local, 1,
                     &remote_iov, 1, 0);
    if (n != 1 || remote[0] != 0x39)
        return fail("process-vm-exec-cow-pinned-mm-isolation");
    if (read(exec_note[0], &byte, 1) != 1 || byte != 'E')
        return fail("process-vm-exec-cow-exec-notification");
    if (write(release[1], "X", 1) != 1 ||
        process_vm_race_wait_child(child) < 0)
        return fail("process-vm-exec-cow-child-reap");

    close(gate[1]); close(ready[0]); close(exec_note[0]); close(release[1]);
    if (munmap(remote, 4096) < 0)
        return fail("process-vm-exec-cow-cleanup");
    puts("MM_PROCESS_VM_EXEC_COW: PASS pinned_old_mm=1 exec_switch=1 parent_isolation=1");
    return 0;
}

static int process_vm_exec_cow_child(int ready_fd, int release_fd)
{
    char value;
    if (write(ready_fd, "E", 1) != 1 || read(release_fd, &value, 1) != 1)
        return fail("process-vm-exec-cow-child-hold");
    return 0;
}

static int private_anon_mremap_dontunmap_rollback(void)
{
    const size_t len = 2 * 4096;
    volatile unsigned char rss_stack_warm[3 * 4096];
    for (size_t i = 0; i < sizeof(rss_stack_warm); i += 4096)
        rss_stack_warm[i] = (unsigned char)i;
    (void)rss_stack_warm[0];

    /* Warm the ordinary destination-copy/unmap call path before measuring
     * rollback accounting.  Its mapping does not arm/consume the test hook. */
    unsigned char *warm_src = mmap(NULL, len, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (warm_src == MAP_FAILED) return fail("dontunmap-rollback-warm-source");
    warm_src[0] = 0x11;
    warm_src[4096] = 0x22;
    void *warm_target = mmap(NULL, len, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (warm_target == MAP_FAILED) {
        munmap(warm_src, len);
        return fail("dontunmap-rollback-warm-target");
    }
    if (munmap(warm_target, len) < 0) {
        munmap(warm_src, len);
        return fail("dontunmap-rollback-warm-target-release");
    }
    unsigned char *warm_moved = mremap(warm_src, len, len,
                                        MREMAP_MAYMOVE | MREMAP_FIXED,
                                        warm_target);
    if (warm_moved != warm_target || warm_moved[0] != 0x11 ||
        warm_moved[4096] != 0x22) {
        if (warm_moved != MAP_FAILED) munmap(warm_moved, len);
        else munmap(warm_src, len);
        return fail("dontunmap-rollback-warm-remap");
    }
    if (munmap(warm_moved, len) < 0)
        return fail("dontunmap-rollback-warm-cleanup");

    long stable_rss = self_rss_pages();
    if (stable_rss < 0) return fail("dontunmap-rollback-warm-rss");
    int stable_samples = 0;
    for (int i = 0; i < 8 && stable_samples < 2; i++) {
        long sample = self_rss_pages();
        if (sample < 0) return fail("dontunmap-rollback-warm-rss-repeat");
        if (sample == stable_rss)
            stable_samples++;
        else {
            stable_rss = sample;
            stable_samples = 0;
        }
    }
    if (stable_samples < 2) return fail("dontunmap-rollback-rss-never-stabilized");

    unsigned char *mem = mmap(NULL, len, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return fail("dontunmap-rollback-source-mmap");
    mem[0] = 0x5a;
    mem[4096] = 0xa5;

    void *target = mmap(NULL, len, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (target == MAP_FAILED) {
        munmap(mem, len);
        return fail("dontunmap-rollback-target-reserve");
    }
    if (munmap(target, len) < 0) {
        munmap(mem, len);
        return fail("dontunmap-rollback-target-release");
    }
    long rss_before = self_rss_pages();
    if (rss_before < 0) {
        munmap(mem, len);
        return fail("dontunmap-rollback-rss-before");
    }

    errno = 0;
    void *result = mremap(mem, len, len,
                          MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP,
                          target);
    if (result != MAP_FAILED || errno != ENOMEM) {
        if (result != MAP_FAILED) munmap(result, len);
        munmap(mem, len);
        return fail("dontunmap-rollback-injected-error");
    }
    if (mem[0] != 0x5a || mem[4096] != 0xa5) {
        munmap(mem, len);
        return fail("dontunmap-rollback-source-preserved");
    }
    mem[0] = 0x5b; /* The source must retain its original writable permission. */
    long rss_after = self_rss_pages();
    if (rss_after != rss_before) {
        printf("MM_DONTUNMAP_ROLLBACK: rss_before=%ld rss_after=%ld\n",
               rss_before, rss_after);
        munmap(mem, len);
        return fail("dontunmap-rollback-rss-preserved");
    }
    void *probe = mmap(target, len, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                       -1, 0);
    if (probe != target) {
        if (probe != MAP_FAILED) munmap(probe, len);
        munmap(mem, len);
        return fail("dontunmap-rollback-destination-cleared");
    }
    munmap(probe, len);
    printf("MM_DONTUNMAP_ROLLBACK_RETRY: begin\n");
    unsigned char *moved = mremap(mem, len, len,
                                 MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP,
                                 target);
    if (moved != target) {
        munmap(mem, len);
        return fail("dontunmap-rollback-retry");
    }
    if (moved[0] != 0x5b || moved[4096] != 0xa5 ||
        mem[0] != 0 || mem[4096] != 0) {
        munmap(mem, len); munmap(moved, len);
        return fail("dontunmap-rollback-retry-source-destination");
    }
    munmap(mem, len); munmap(moved, len);
    printf("MM_DONTUNMAP_ROLLBACK: PASS source_preserved=1 destination_cleared=1 retry=1 rss=%ld\n",
           rss_before);
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
        printf("MM_STRESS: huge-fork-child-status raw=0x%x exited=%d exit=%d signaled=%d signal=%d\n",
               status, WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
               WIFSIGNALED(status), WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        munmap(mem, len);
        return fail("huge-fork-child");
    }

    for (size_t i = 0; i < HPSIZE; i++) {
        if (mem[i] != (char)((i * 17) % 251)) {
            munmap(mem, len);
            return fail("huge-fork-verify");
        }
    }

    /* After the child exits, the parent is the sole owner of its COW block.
     * Its first write must upgrade the existing huge leaf in place. */
    mem[0] = (char)0x5a;
    mem[HPSIZE - 1] = (char)0xa5;
    if (mem[0] != (char)0x5a || mem[HPSIZE - 1] != (char)0xa5) {
        munmap(mem, len);
        return fail("huge-fork-exclusive-upgrade");
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


static volatile int diag_yield_ready, diag_yield_start, diag_yield_done;
static void *diag_yield_worker(void *unused)
{
    (void)unused;
    __atomic_store_n(&diag_yield_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&diag_yield_start, __ATOMIC_ACQUIRE)) sched_yield();
    for (unsigned i = 0; i < 10000; ++i) sched_yield();
    __atomic_store_n(&diag_yield_done, 1, __ATOMIC_RELEASE);
    return NULL;
}
static int diag_yield_pair(void)
{
    pthread_t t;
    diag_yield_ready = diag_yield_start = diag_yield_done = 0;
    int e = pthread_create(&t, NULL, diag_yield_worker, NULL);
    if (e) return fail("diag-yield-create");
    while (!__atomic_load_n(&diag_yield_ready, __ATOMIC_ACQUIRE)) sched_yield();
    __atomic_store_n(&diag_yield_start, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&diag_yield_done, __ATOMIC_ACQUIRE)) sched_yield();
    if (pthread_join(t, NULL)) return fail("diag-yield-join");
    puts("DIAG: YIELD_PAIR PASS sched_yield=10000 pthread_join=1");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--yield-pair-only") == 0)
        return diag_yield_pair();
    if (argc == 4 && strcmp(argv[1], "--process-vm-exec-cow-child") == 0)
        return process_vm_exec_cow_child(atoi(argv[2]), atoi(argv[3]));
    if (argc == 2 && strcmp(argv[1], "--process-vm-exec-cow-only") == 0)
        return process_vm_exec_cow_race();
    if (argc == 2 && strcmp(argv[1], "--dontunmap-only") == 0) {
        printf("MM_DONTUNMAP_ANON: start pages=2\n");
        if (private_anon_mremap_dontunmap() != 0 ||
            dontunmap_adjacent_destination_before_source() != 0)
            return 1;
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--dontunmap-rollback-only") == 0)
        return private_anon_mremap_dontunmap_rollback() == 0 ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--usercopy-contract") == 0)
        return usercopy_partial_contract() == 0 && process_vm_contract() == 0 ? 0 : 1;
    if (argc == 3 && strcmp(argv[1], "--dontunmap-split-failure") == 0) {
        unsigned long split_call = strtoul(argv[2], NULL, 10);
        if (split_call < 1 || split_call > 6)
            return fail("dontunmap-split-invalid-call");
        return private_anon_mremap_dontunmap_split_failure(
            (unsigned int)split_call) == 0 ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "--huge-fork-only") == 0) {
        printf("MM_STRESS: huge-fork start\n");
        if (huge_page_fork_cow() != 0)
            return 1;
        printf("MM_STRESS: huge-fork PASS\n");
        return 0;
    }
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
    if (dontunmap_adjacent_destination_before_source() != 0)
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
