/*
 * HWRNG_TEST -- read /dev/hwrng (the virtio-rng CHAR class device) and check
 * the bytes actually look like entropy rather than a stuck buffer.
 *
 * The three checks the gate asks for are deliberately weak on purpose: a
 * driver that returns a buffer it never filled passes a "not all zero" test
 * only if the memory behind it happens to be dirty, and one that leaves the
 * device buffers mapped can hand back the same 256 bytes forever.  So the test
 * reads at least HWRNG_MIN_BYTES in a loop (the driver returns whatever the
 * request queue produced, which is not a fixed size), and then requires that
 * the block is not uniformly 0x00, not uniformly 0xff, and not a single
 * repeated byte.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define HWRNG_MIN_BYTES 256
/* The driver's own bounded read wait is 500 ms; allow a few of them so a slow
 * boot (module load after the shell starts) does not look like a failure. */
#define HWRNG_MAX_TRIES 20

static int fail(const char *what)
{
    printf("HWRNG_TEST: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int all_equal(const uint8_t *buf, size_t len)
{
    for (size_t i = 1; i < len; i++)
        if (buf[i] != buf[0])
            return 0;
    return 1;
}

int main(void)
{
    int fd = open("/dev/hwrng", O_RDONLY);
    if (fd < 0)
        return fail("open /dev/hwrng");

    uint8_t buf[HWRNG_MIN_BYTES];
    memset(buf, 0, sizeof(buf));

    size_t got = 0;
    int tries;
    for (tries = 0; tries < HWRNG_MAX_TRIES && got < sizeof(buf); tries++) {
        ssize_t n = read(fd, buf + got, sizeof(buf) - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return fail("read /dev/hwrng");
        }
        if (n == 0)
            break; /* end of stream: nothing more will arrive */
        got += (size_t)n;
    }
    close(fd);

    if (got < HWRNG_MIN_BYTES) {
        printf("HWRNG_TEST: FAIL short read %zu/%d bytes after %d tries\n",
               got, HWRNG_MIN_BYTES, tries);
        return 1;
    }

    /* A device that never wrote its buffers leaves whatever the coherent
     * allocation already held; a driver that posts nothing leaves zeros. */
    int zeros = 1, ones = 1;
    for (size_t i = 0; i < got; i++) {
        if (buf[i] != 0x00)
            zeros = 0;
        if (buf[i] != 0xff)
            ones = 0;
        if (!zeros && !ones)
            break;
    }
    if (zeros) {
        printf("HWRNG_TEST: FAIL %zu bytes are all 0x00\n", got);
        return 1;
    }
    if (ones) {
        printf("HWRNG_TEST: FAIL %zu bytes are all 0xff\n", got);
        return 1;
    }
    if (all_equal(buf, got)) {
        printf("HWRNG_TEST: FAIL single repeated byte 0x%02x\n", buf[0]);
        return 1;
    }

    printf("HWRNG_TEST: PASS bytes=%zu first=0x%02x second=0x%02x tries=%d\n",
           got, buf[0], buf[1], tries);
    return 0;
}