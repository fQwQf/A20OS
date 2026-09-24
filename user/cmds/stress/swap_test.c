/* Swap gate: drive the loop-device swap path end to end and assert the
 * kernel's swap accounting and error paths.
 *
 * Scenario: create a backing file on the writable root ramfs, bind it to a
 * free loop device (LOOP_CTL_GET_FREE + LOOP_SET_FD), then mkswap -> swapon
 * and require /proc/swaps plus sysinfo totalswap to reflect the new device.
 * A second swapon of the same device must fail with EBUSY; after touching a
 * few MiB of anonymous memory with swap enabled, swapoff must succeed and
 * totalswap must return to zero.
 *
 * Boundary: this does NOT drive the swap_out_victim_pages() reclaim path.
 * Real write-out only happens when global free frames drop below
 * OOM_MIN_FREE_PAGES (kernel/mm/oom.c), and reclaim swaps at most
 * MAX_SWAP_RECLAIM=8 pages per 2s cooldown window, so forcing meaningful
 * swap-out inside a 1 GiB QEMU smoke is not practical.  swap_read_page /
 * swap_write_page are still exercised partially: mkswap and swapon perform
 * real header/badmap I/O through the loop block adapter.  A dedicated
 * low-memory instance would be needed to gate the actual reclaim path.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

/* Request numbers match kernel/drivers/block/loop.c.  Note LOOP_SET_FD takes
 * a pointer to the backing fd (the kernel does copy_from_user on arg), unlike
 * Linux where the fd is passed by value. */
#define LOOP_SET_FD       0x4C00
#define LOOP_CLR_FD       0x4C01
#define LOOP_CTL_GET_FREE 0x4C82

#define BACKING_PATH "/tmp/swap_backing.bin"
#define BACKING_SIZE (16UL * 1024 * 1024)
#define TOUCH_SIZE   (8UL * 1024 * 1024)

static int fail(const char *what)
{
    printf("SWAP_TEST: FAIL at %s (errno=%d)\n", what, errno);
    return 1;
}

static long totalswap(void)
{
    struct sysinfo si;
    if (sysinfo(&si) < 0)
        return -1;
    return (long)(si.totalswap * si.mem_unit);
}

static int write_backing_file(void)
{
    int fd = open(BACKING_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    static char zeros[65536];
    size_t left = BACKING_SIZE;
    while (left > 0) {
        size_t chunk = left < sizeof(zeros) ? left : sizeof(zeros);
        ssize_t w = write(fd, zeros, chunk);
        if (w != (ssize_t)chunk) {
            close(fd);
            return -1;
        }
        left -= chunk;
    }
    /* loop_set_fd sizes the device via lseek(SEEK_END); keep it open. */
    return fd;
}

static int proc_swaps_lists(const char *path)
{
    int fd = open("/proc/swaps", O_RDONLY);
    if (fd < 0)
        return -1;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    if (!strstr(buf, path) || !strstr(buf, "partition")) {
        printf("SWAP_TEST: /proc/swaps content: [%s]\n", buf);
        return -1;
    }
    return 0;
}

int main(void)
{
    if (totalswap() != 0)
        return fail("baseline-totalswap-not-zero");

    int bfd = write_backing_file();
    if (bfd < 0)
        return fail("backing-file-create");

    int cfd = open("/dev/loop-control", O_RDWR);
    if (cfd < 0)
        return fail("loop-control-open");
    int idx = ioctl(cfd, LOOP_CTL_GET_FREE, 0);
    close(cfd);
    if (idx < 0 || idx > 7)
        return fail("loop-ctl-get-free");

    char loop_path[32];
    snprintf(loop_path, sizeof(loop_path), "/dev/loop%d", idx);
    int lfd = open(loop_path, O_RDWR);
    if (lfd < 0)
        return fail("loop-open");
    if (ioctl(lfd, LOOP_SET_FD, &bfd) < 0)
        return fail("loop-set-fd");

    if (syscall(SYS_mkswap, loop_path, 0) < 0)
        return fail("mkswap");
    if (syscall(SYS_swapon, loop_path, 0) < 0)
        return fail("swapon");

    long total = totalswap();
    if (total <= 0)
        return fail("totalswap-after-swapon");
    if (proc_swaps_lists(loop_path) < 0)
        return fail("proc-swaps-entry");
    printf("SWAP_TEST: swapon ok, totalswap=%ld\n", total);

    errno = 0;
    if (syscall(SYS_swapon, loop_path, 0) != -1 || errno != EBUSY)
        return fail("swapon-duplicate-not-EBUSY");

    /* Prove anonymous memory still works with an active swap device. */
    volatile char *p = mmap(NULL, TOUCH_SIZE, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return fail("anon-mmap");
    for (size_t off = 0; off < TOUCH_SIZE; off += 4096)
        p[off] = (char)(off >> 12);
    for (size_t off = 0; off < TOUCH_SIZE; off += 4096) {
        if (p[off] != (char)(off >> 12))
            return fail("anon-verify");
    }
    munmap((void *)p, TOUCH_SIZE);

    if (syscall(SYS_swapoff, loop_path) < 0)
        return fail("swapoff");
    if (totalswap() != 0)
        return fail("totalswap-after-swapoff");

    if (ioctl(lfd, LOOP_CLR_FD, 0) < 0)
        return fail("loop-clr-fd");
    close(lfd);
    close(bfd);
    unlink(BACKING_PATH);

    printf("SWAP_TEST: PASS\n");
    return 0;
}
