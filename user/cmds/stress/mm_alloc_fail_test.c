/* MM-04 allocation-rollback gate: drive one injected allocation failure per
 * build and prove the failing operation both reports the failure and gives
 * every resource back.
 *
 * The kernel image is compiled with CONFIG_MM_FAIL_STAGE=<n>, so exactly one
 * allocation stage fails once per boot.  The stage is named on the command
 * line (1..3); the workload sets up the operation that reaches that stage,
 * runs it, and asserts:
 *
 *   * the operation fails with the errno the real allocator would produce
 *     (stage 3 has no errno contract -- swap-out is best-effort -- so it
 *     asserts the equivalent observable: no page left, no slot consumed);
 *   * /proc/meminfo MemFree and /proc/self/statm RSS are back at the
 *     pre-operation values (a leaked frame or page-table page shows up here);
 *   * for stage 2 a huge leaf was really installed, and for stage 3 no swap
 *     slot was consumed.
 *
 * The kernel's shutdown [MM-ASM] audit line is asserted by the gate, not here.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#ifndef SYS_swapon
#define SYS_swapon 224
#endif
#ifndef SYS_swapoff
#define SYS_swapoff 225
#endif
#ifndef SYS_mkswap
#define SYS_mkswap 1020
#endif
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define LOOP_SET_FD       0x4C00
#define LOOP_CTL_GET_FREE 0x4C82
#define PAGE 4096UL
#define HPSIZE (2 * 1024 * 1024)
#define SWAP_BACKING "/tmp/mm_alloc_fail_swap.bin"
#define SWAP_BACKING_SIZE (16UL * 1024 * 1024)

static int g_stage;

static int fail(const char *what)
{
    printf("MM_ALLOC_FAIL_TEST: stage=%d FAIL at %s (errno=%d)\n",
           g_stage, what, errno);
    return 1;
}

/* Two consecutive equal reads, so a transient kernel allocation that is
 * freed again cannot make the baseline (or the post-operation sample) wrong. */
static long stable_memfree_pages(void)
{
    long previous = -1;
    for (int i = 0; i < 32; i++) {
        int fd = open("/proc/meminfo", O_RDONLY);
        if (fd < 0)
            return -1;
        char buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            return -1;
        buf[n] = '\0';
        const char *p = strstr(buf, "MemFree:");
        if (!p)
            return -1;
        p += 8;
        while (*p == ' ')
            p++;
        long kb = strtol(p, NULL, 10);
        long pages = kb / 4; /* kB -> 4 KiB pages */
        if (pages == previous)
            return pages;
        previous = pages;
    }
    return previous;
}

static long stable_rss_pages(void)
{
    long previous = -1;
    for (int i = 0; i < 32; i++) {
        int fd = open("/proc/self/statm", O_RDONLY);
        if (fd < 0)
            return -1;
        char buf[128];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            return -1;
        buf[n] = '\0';
        unsigned long total = 0, rss = 0;
        if (sscanf(buf, "%lu %lu", &total, &rss) != 2)
            return -1;
        if ((long)rss == previous)
            return (long)rss;
        previous = (long)rss;
    }
    return previous;
}

/* Two benign transactions: the first drains any deferred frame or page-table
 * release left by earlier work and leaves one of its own, the second drains
 * that one.  Without this the baseline can be taken with a release still
 * pending, and the operation under test then appears to free an extra page
 * when its own transaction drains the leftover. */
static int settle_accounting(void)
{
    for (int round = 0; round < 2; round++) {
        char *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            return -1;
        if (round == 0)
            p[0] = 1;
        if (munmap(p, PAGE) < 0)
            return -1;
    }
    return 0;
}

static long swap_used_kb(const char *path)
{
    FILE *file = fopen("/proc/swaps", "r");
    if (!file)
        return -1;
    char line[256], name[128], type[32];
    unsigned long size, used;
    long result = -1;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "%127s %31s %lu %lu", name, type, &size, &used) == 4 &&
            strcmp(name, path) == 0) {
            result = (long)used;
            break;
        }
    }
    fclose(file);
    return result;
}

/* Stage 1: the intermediate page-table node pt_walk() allocates for a
 * DONTUNMAP destination leaf.  The destination must sit in a page-table
 * subtree that does not exist yet, and mmap hands out addresses from the same
 * 2 MiB window as the source, so a plain reservation would share the source's
 * node.  An untouched large reservation has a VMA but no page-table node (a
 * node appears only when a page is faulted), so an interior window is a fresh
 * subtree; MREMAP_FIXED replaces that window itself. */
static int stage1_pt_node(void)
{
    const size_t len = 2 * PAGE;
    const size_t region_len = 8 * 1024 * 1024;
    char *region = mmap(NULL, region_len, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED)
        return fail("stage1-region-mmap");
    char *target = region + 2 * 1024 * 1024;

    unsigned char *src = mmap(NULL, len, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (src == MAP_FAILED) {
        munmap(region, region_len);
        return fail("stage1-source-mmap");
    }
    src[0] = 0x5a;
    src[PAGE] = 0xa5;

    if (settle_accounting() < 0) {
        munmap(src, len);
        munmap(region, region_len);
        return fail("stage1-settle");
    }
    long rss_base = stable_rss_pages();
    long free_base = stable_memfree_pages();
    if (rss_base < 0 || free_base < 0) {
        munmap(src, len);
        munmap(region, region_len);
        return fail("stage1-baseline");
    }

    errno = 0;
    void *moved = mremap(src, len, len,
                         MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP,
                         target);
    if (moved != MAP_FAILED || errno != ENOMEM) {
        if (moved != MAP_FAILED)
            munmap(moved, len);
        munmap(src, len);
        munmap(region, region_len);
        return fail("stage1-injected-error");
    }

    if (src[0] != 0x5a || src[PAGE] != 0xa5) {
        munmap(src, len);
        munmap(region, region_len);
        return fail("stage1-source-preserved");
    }
    src[0] = 0x11; /* the source must still be writable */

    void *probe = mmap(target, len, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                       -1, 0);
    if (probe != target) {
        if (probe != MAP_FAILED)
            munmap(probe, len);
        munmap(src, len);
        munmap(region, region_len);
        return fail("stage1-destination-cleared");
    }
    munmap(probe, len);

    long rss_after = stable_rss_pages();
    long free_after = stable_memfree_pages();
    /* The order-0 allocator serves frames from a per-CPU batch that is already
     * off the free list, so one alloc/free cycle can move MemFree by a page.
     * The allowance is one page; a rollback that leaks the destination's
     * page-table nodes would move it by more. */
    if (rss_after != rss_base || free_after < free_base - 1) {
        printf("MM_ALLOC_FAIL_TEST: stage=1 accounting rss=%ld/%ld free=%ld/%ld\n",
               rss_base, rss_after, free_base, free_after);
        munmap(src, len);
        munmap(region, region_len);
        return fail("stage1-accounting");
    }

    printf("MM_ALLOC_FAIL_TEST: stage=1 PASS errno=%d memfree_delta=%ld rss_delta=%ld\n",
           ENOMEM, free_after - free_base, rss_after - rss_base);
    munmap(src, len);
    munmap(region, region_len);
    return 0;
}

/* Map twice the huge size and take the aligned 2 MiB window, so the THP fault
 * path really installs a huge leaf regardless of the address mmap returns. */
static char *huge_window(size_t *map_len_out, char **base_out)
{
    size_t len = 2 * HPSIZE;
    char *base = mmap(NULL, len, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (base == MAP_FAILED)
        return MAP_FAILED;
    char *win = (char *)(((uintptr_t)base + HPSIZE - 1) & ~(uintptr_t)(HPSIZE - 1));
    *map_len_out = len;
    *base_out = base;
    return win;
}

static int huge_pattern_ok(const char *win)
{
    for (size_t i = 0; i < HPSIZE; i++)
        if ((unsigned char)win[i] != (unsigned char)((i * 13) % 251))
            return 0;
    return 1;
}

/* Stage 2: the per-page frames mm_demote_huge_page() allocates.  A partial
 * munmap of a huge leaf forces demotion; the injection fails it half way
 * through the copy, so the frames already copied must be given back and the
 * huge leaf must survive. */
static int stage2_huge_demote(void)
{
    size_t map_len = 0;
    char *base = NULL;
    char *win = huge_window(&map_len, &base);
    if (win == MAP_FAILED)
        return fail("stage2-huge-mmap");

    for (size_t i = 0; i < HPSIZE; i++)
        win[i] = (char)((i * 13) % 251);
    if (!huge_pattern_ok(win)) {
        munmap(base, map_len);
        return fail("stage2-huge-install");
    }

    /* The huge-leaf install defers a frame release to the next TLB
     * transaction; settle it before the baseline. */
    if (settle_accounting() < 0) {
        munmap(base, map_len);
        return fail("stage2-settle");
    }

    long rss_base = stable_rss_pages();
    long free_base = stable_memfree_pages();
    if (rss_base < 0 || free_base < 0) {
        munmap(base, map_len);
        return fail("stage2-baseline");
    }

    errno = 0;
    if (munmap(win + PAGE, PAGE) != -1 || errno != ENOMEM) {
        munmap(base, map_len);
        return fail("stage2-injected-error");
    }
    if (!huge_pattern_ok(win)) {
        munmap(base, map_len);
        return fail("stage2-huge-preserved");
    }

    long rss_after = stable_rss_pages();
    long free_after = stable_memfree_pages();
    /* The buddy allocator hands order-0 frames out of a per-CPU batch whose
     * pages are already off the free list, so an alloc/free cycle can shift
     * MemFree by up to one page.  The partial-free under test is 256 pages, so
     * a one-page allowance still cannot hide a rollback that leaks. */
    if (rss_after != rss_base || free_after < free_base - 1) {
        printf("MM_ALLOC_FAIL_TEST: stage=2 accounting rss=%ld/%ld free=%ld/%ld\n",
               rss_base, rss_after, free_base, free_after);
        munmap(base, map_len);
        return fail("stage2-accounting");
    }

    printf("MM_ALLOC_FAIL_TEST: stage=2 PASS errno=%d memfree_delta=%ld rss_delta=%ld\n",
           ENOMEM, free_after - free_base, rss_after - rss_base);
    munmap(base, map_len);
    return 0;
}

static int swap_backing_create(void)
{
    int fd = open(SWAP_BACKING, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    static char zeros[65536];
    size_t left = SWAP_BACKING_SIZE;
    while (left > 0) {
        size_t chunk = left < sizeof(zeros) ? left : sizeof(zeros);
        if (write(fd, zeros, chunk) != (ssize_t)chunk) {
            close(fd);
            return -1;
        }
        left -= chunk;
    }
    return fd;
}

/* Stage 3: the swap slot get_swap_page_owned() hands out during swap-out.
 * MADV_PAGEOUT drives a real write-out; with the slot allocation failed no
 * page may leave and no slot may be consumed.  This stage has no errno
 * contract (reclaim is best-effort), so the observable is the residency and
 * the /proc/swaps in-use count. */
static int stage3_swap_slot(void)
{
    int bfd = swap_backing_create();
    if (bfd < 0)
        return fail("stage3-backing");

    int cfd = open("/dev/loop-control", O_RDWR);
    if (cfd < 0) {
        close(bfd);
        return fail("stage3-loop-control");
    }
    int idx = ioctl(cfd, LOOP_CTL_GET_FREE, 0);
    close(cfd);
    if (idx < 0 || idx > 7) {
        close(bfd);
        return fail("stage3-loop-get-free");
    }
    char loop_path[32];
    snprintf(loop_path, sizeof(loop_path), "/dev/loop%d", idx);
    int lfd = open(loop_path, O_RDWR);
    if (lfd < 0) {
        close(bfd);
        return fail("stage3-loop-open");
    }
    if (ioctl(lfd, LOOP_SET_FD, &bfd) < 0) {
        close(lfd); close(bfd);
        return fail("stage3-loop-set-fd");
    }
    if (syscall(SYS_mkswap, loop_path, 0) < 0) {
        close(lfd); close(bfd);
        return fail("stage3-mkswap");
    }
    if (syscall(SYS_swapon, loop_path, 0) < 0) {
        close(lfd); close(bfd);
        return fail("stage3-swapon");
    }

    const size_t len = 4 * PAGE;
    unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        syscall(SYS_swapoff, loop_path);
        close(lfd); close(bfd);
        return fail("stage3-mmap");
    }
    for (size_t i = 0; i < len; i++)
        p[i] = (unsigned char)(i * 7 + 1);

    if (settle_accounting() < 0) {
        munmap(p, len);
        syscall(SYS_swapoff, loop_path);
        close(lfd); close(bfd);
        return fail("stage3-settle");
    }
    long used_base = swap_used_kb(loop_path);
    long rss_base = stable_rss_pages();
    long free_base = stable_memfree_pages();
    if (used_base != 0 || rss_base < 0 || free_base < 0) {
        printf("MM_ALLOC_FAIL_TEST: stage=3 baseline used=%ld\n", used_base);
        munmap(p, len);
        syscall(SYS_swapoff, loop_path);
        close(lfd); close(bfd);
        return fail("stage3-baseline");
    }

    if (madvise(p, len, MADV_PAGEOUT) < 0) {
        munmap(p, len);
        syscall(SYS_swapoff, loop_path);
        close(lfd); close(bfd);
        return fail("stage3-pageout");
    }

    unsigned char resident[4];
    if (mincore(p, len, resident) < 0) {
        munmap(p, len);
        syscall(SYS_swapoff, loop_path);
        close(lfd); close(bfd);
        return fail("stage3-mincore");
    }
    int resident_count = 0;
    for (int i = 0; i < 4; i++)
        resident_count += resident[i] & 1;

    long used_after = swap_used_kb(loop_path);
    long rss_after = stable_rss_pages();
    long free_after = stable_memfree_pages();
    /* resident/used are the load-bearing stage-3 assertions: the failed slot
     * allocation must leave every page in RAM and consume no slot.  MemFree is
     * only a supplementary leak check, with the per-CPU batch's one-page
     * allowance (see stage 1). */
    if (resident_count != 4 || used_after != 0 ||
        rss_after != rss_base || free_after < free_base - 1) {
        printf("MM_ALLOC_FAIL_TEST: stage=3 accounting resident=%d used=%ld rss=%ld/%ld free=%ld/%ld\n",
               resident_count, used_after, rss_base, rss_after, free_base, free_after);
        munmap(p, len);
        syscall(SYS_swapoff, loop_path);
        close(lfd); close(bfd);
        return fail("stage3-accounting");
    }

    printf("MM_ALLOC_FAIL_TEST: stage=3 PASS resident=%d swap_used_kb=%ld memfree_delta=%ld rss_delta=%ld\n",
           resident_count, used_after, free_after - free_base, rss_after - rss_base);

    munmap(p, len);
    syscall(SYS_swapoff, loop_path);
    close(lfd);
    close(bfd);
    unlink(SWAP_BACKING);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("MM_ALLOC_FAIL_TEST: usage: mm_alloc_fail_test <1|2|3>\n");
        return 2;
    }
    g_stage = atoi(argv[1]);
    int rc;
    switch (g_stage) {
    case 1:
        rc = stage1_pt_node();
        break;
    case 2:
        rc = stage2_huge_demote();
        break;
    case 3:
        rc = stage3_swap_slot();
        break;
    default:
        printf("MM_ALLOC_FAIL_TEST: unknown stage %d\n", g_stage);
        return 2;
    }
    if (rc != 0)
        return 1;
    printf("MM_ALLOC_FAIL_TEST: stage=%d DONE\n", g_stage);
    return 0;
}
