/*
 * virtio-scsi completion gate.
 *
 * Two claims, in one boot, on one device:
 *
 *   1. The data plane still reads, writes and flushes correctly after the
 *      driver moved command completion onto the used-ring interrupt.  This is
 *      the regression half: an interrupt that fires for the wrong ring, or a
 *      completion path that skips the response DMA sync, would still let a
 *      polling fallback return the right bytes on this machine, so the loop
 *      below is what says the transfer itself is intact.
 *
 *   2. Completions really arrived as interrupts.  The driver answers
 *      A20_BLK_IOCTL_GET_STATS with irq_count -- a counter only the interrupt
 *      handler bumps -- and irq_mode, which distinguishes "asked for an
 *      interrupt and settled on one" from "fell back to polling".  Reading a
 *      correct sector back proves neither: a driver that silently regressed
 *      to polling passes assertion 1 and fails this one.
 *
 * The device is found by probing /dev/diskN for the stats ioctl rather than by
 * index, so the test does not have to agree with the driver about which slot
 * the controller got.  Only virtio-scsi implements it; everything else returns
 * -ENOTTY.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <uapi/a20/block.h>

#define MAX_DISKS 8
/* 64 sectors: enough to be a real multi-block WRITE(10)/READ(10) rather than a
 * single-sector special case, small enough that the gate's QEMU timeout is
 * dominated by boot, not by the transfer. */
#define TEST_SECTORS 64
#define SECTOR_SIZE 512
#define TEST_BYTES (TEST_SECTORS * SECTOR_SIZE)

#define CHK(cond, msg)                                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("VIRTIO_SCSI_TEST: FAIL %s:%d %s (errno=%d %s)\n",          \
                   __func__, __LINE__, (msg), errno, strerror(errno));         \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static void fill_pattern(unsigned char *buf, size_t len, uint64_t seed) {
    for (size_t i = 0; i < len; i++)
        buf[i] = (unsigned char)((i * 31u + seed * 7u + (i >> 8)) & 0xffu);
}

static int find_scsi_disk(int *fd_out, uint64_t *size_out) {
    for (int i = 0; i < MAX_DISKS; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/disk%d", i);
        int fd = open(path, O_RDWR);
        if (fd < 0)
            continue;
        a20_blk_stats_t st;
        memset(&st, 0, sizeof(st));
        if (ioctl(fd, A20_BLK_IOCTL_GET_STATS, &st) != 0 ||
            st.version != A20_BLK_STATS_VERSION) {
            close(fd);
            continue;
        }
        off_t end = lseek(fd, 0, SEEK_END);
        if (end < SECTOR_SIZE * TEST_SECTORS) {
            close(fd);
            continue;
        }
        *fd_out = fd;
        *size_out = (uint64_t)end;
        printf("VIRTIO_SCSI_TEST: %s capacity=%llu bytes irq_mode=%llu "
               "irq_line=%lld irq_count=%llu\n",
               path, (unsigned long long)end, (unsigned long long)st.irq_mode,
               (long long)st.irq_line, (unsigned long long)st.irq_count);
        return 0;
    }
    printf("VIRTIO_SCSI_TEST: FAIL no /dev/disk%u implements "
           "A20_BLK_IOCTL_GET_STATS (virtio-scsi not bound?)\n",
           (unsigned)MAX_DISKS - 1);
    return -1;
}

static int write_all(int fd, const void *buf, size_t len, off_t off) {
    size_t done = 0;
    while (done < len) {
        ssize_t n =
            pwrite(fd, (const char *)buf + done, len - done, off + (off_t)done);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len, off_t off) {
    size_t done = 0;
    while (done < len) {
        ssize_t n =
            pread(fd, (char *)buf + done, len - done, off + (off_t)done);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    int fd = -1;
    uint64_t size = 0;
    CHK(find_scsi_disk(&fd, &size) == 0, "locate a virtio-scsi block device");

    a20_blk_stats_t before;
    memset(&before, 0, sizeof(before));
    CHK(ioctl(fd, A20_BLK_IOCTL_GET_STATS, &before) == 0, "stats before");

    /* The driver must have settled on an interrupt, not on the polling
     * fallback, or the rest of this test proves nothing about completion. */
    CHK(before.irq_mode == A20_BLK_IRQ_INTX ||
            before.irq_mode == A20_BLK_IRQ_MSIX,
        "controller is using an interrupt, not the polling fallback");
    CHK(before.timeouts == 0, "no command timed out during probe");

    unsigned char *out = malloc(TEST_BYTES);
    unsigned char *in = malloc(TEST_BYTES);
    CHK(out && in, "allocate test buffers");

    /* The last 64 KiB of the medium: past anything the boot wrote, so the
     * comparison below is against sectors this test itself owns. */
    off_t base = (off_t)(size - TEST_BYTES);

    /* Loop 1: write -> flush -> read back must match.  Two more rounds on top
     * so a completion that only happens to work for the first command after
     * DRIVER_OK -- the state probe leaves the controller in -- shows up. */
    for (int round = 0; round < 3; round++) {
        fill_pattern(out, TEST_BYTES, (uint64_t)round);
        CHK(write_all(fd, out, TEST_BYTES, base) == 0,
            "write loopback payload");
        CHK(ioctl(fd, A20_BLK_IOCTL_SYNC) == 0, "flush after write");
        memset(in, 0, TEST_BYTES);
        CHK(read_all(fd, in, TEST_BYTES, base) == 0, "read back after flush");
        CHK(memcmp(in, out, TEST_BYTES) == 0, "loopback payload round trip");

        /* Overwrite in place: the read must not be served from anything the
         * driver kept in memory, and a stale buffer would show up here. */
        fill_pattern(out, TEST_BYTES, (uint64_t)round + 100u);
        CHK(write_all(fd, out, TEST_BYTES, base) == 0,
            "rewrite loopback payload");
        CHK(ioctl(fd, A20_BLK_IOCTL_SYNC) == 0, "flush after rewrite");
        memset(in, 0, TEST_BYTES);
        CHK(read_all(fd, in, TEST_BYTES, base) == 0, "read back after rewrite");
        CHK(memcmp(in, out, TEST_BYTES) == 0, "in-place loopback payload");
    }

    a20_blk_stats_t after;
    memset(&after, 0, sizeof(after));
    CHK(ioctl(fd, A20_BLK_IOCTL_GET_STATS, &after) == 0, "stats after");

    printf("VIRTIO_SCSI_TEST: stats commands=%llu flushes=%llu irq_count=%llu "
           "(+%llu) irq_completions=%llu spin_completions=%llu timeouts=%llu\n",
           (unsigned long long)after.commands,
           (unsigned long long)after.flushes,
           (unsigned long long)after.irq_count,
           (unsigned long long)(after.irq_count - before.irq_count),
           (unsigned long long)after.irq_completions,
           (unsigned long long)after.spin_completions,
           (unsigned long long)after.timeouts);

    CHK(after.flushes > before.flushes, "the flush ioctl reached the driver");
    CHK(after.commands > before.commands, "commands reached the driver");
    CHK(after.timeouts == before.timeouts, "no command timed out under I/O");

    /* The assertion the whole gate exists for: this counter is bumped only by
     * the interrupt handler, so a non-zero delta means the controller raised
     * the line and the platform dispatched it.  A polling fallback cannot
     * produce it no matter how many commands it completes. */
    CHK(after.irq_count > before.irq_count,
        "interrupt handler ran during the test (irq_count grew)");

    close(fd);
    free(out);
    free(in);
    printf("VIRTIO_SCSI_TEST: PASS\n");
    return 0;
}
