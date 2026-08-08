/*
 * ubd_fs_test — filesystem-level proof + throughput for the user-space
 * virtio-blk driver (M4, mainstream hybrid form).
 *
 * Spawns ubd-rv.a20drv (which attaches its ring to the kernel block proxy and
 * causes /ubd to be mounted as FAT32), then:
 *   1. verifies /ubd/big.bin exists (filesystem read through the driver);
 *   2. reads it 5x, reporting MB/s — the first read is cold (through the
 *      user driver), later reads should be page-cache warm (the kernel
 *      page cache absorbs the user-driver path, the mainstream claim).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#define BIG_PATH  "/ubd/big.bin"
#define BIG_SIZE  (4u * 1024 * 1024)
#define READS     5
#define CHUNK     (1024u * 1024)

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(void)
{
    /* 1. The unified driver manager auto-spawns ubd when the user-reserved
     *    virtio-blk slot is present (the driver's device).  If /ubd is not
     *    mounted yet, fall back to spawning the driver on demand. */
    int mounted = 0;
    for (int i = 0; i < 500; i++) {
        struct stat st;
        if (stat(BIG_PATH, &st) == 0) { mounted = 1; break; }
        usleep(20000);
    }
    if (!mounted) {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/ubd-rv.a20drv", "ubd-rv.a20drv", (char *)0);
            _exit(90);
        }
        for (int i = 0; i < 500; i++) {
            struct stat st;
            if (stat(BIG_PATH, &st) == 0) { mounted = 1; break; }
            usleep(20000);
        }
    }

    /* 2. /ubd must be mounted after the driver attaches. */
    if (!mounted) {
        printf("UBD_FS: FAIL /ubd/big.bin not visible\n");
        return 1;
    }
    printf("UBD_FS: /ubd mounted, big.bin present (size=%u)\n", BIG_SIZE);

    /* 3. Read it repeatedly, timing each pass. */
    static char buf[CHUNK];
    uint64_t best = ~0ull;
    for (int pass = 0; pass < READS; pass++) {
        int fd = open(BIG_PATH, O_RDONLY);
        if (fd < 0) {
            printf("UBD_FS: FAIL open\n");
            return 2;
        }
        uint64_t t0 = now_ns();
        uint64_t total = 0;
        int ok = 1;
        while (total < BIG_SIZE) {
            ssize_t n = read(fd, buf, CHUNK);
            if (n <= 0) { ok = 0; break; }
            total += (uint64_t)n;
        }
        close(fd);
        uint64_t dt = now_ns() - t0;
        uint64_t mb_s = dt ? (total / 1000000) * 1000000000ull / dt : 0;
        if (ok && dt < best) best = dt;
        printf("UBD_FS: read#%d %llu MiB in %llu ms (%llu MiB/s)%s\n",
               pass + 1,
               (unsigned long long)(total >> 20),
               (unsigned long long)(dt / 1000000),
               (unsigned long long)mb_s,
               ok ? "" : " ERROR");
        if (!ok) return 3;
    }
    if (best == ~0ull) return 4;

    printf("UBD_FS: PASS (best full read %llu ms)\n",
           (unsigned long long)(best / 1000000));
    return 0;
}
