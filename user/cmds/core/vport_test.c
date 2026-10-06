/*
 * vport_test -- the guest half of the smoke-virtio-console gate.
 *
 * The host probe (tools/vport_host_probe.py) connects to the QEMU chardev
 * socket and writes 64 bytes only after this program prints its READY marker,
 * so the two halves cannot race the guest boot.  This program then
 *
 *   1. opens /dev/vport0 (the virtio-console port 0 char device),
 *   2. detaches the console input mirror, so the probe's bytes reach this
 *      program only and are not also injected into the shell,
 *   3. reads exactly 64 bytes,
 *   4. writes the same 64 bytes back, which is what the host asserts on.
 *
 * Verdicts are printed as "VPORT_TEST: ..." lines for the gate to grep.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <drivers/char/virtio_console.h>

#define VPORT_TEST_BYTES   64U
#define VPORT_READ_TRIES   400U   /* ~8 s at 20 ms per try */

static void sleep_ms(long ms)
{
    struct timespec ts = { .tv_sec = ms / 1000,
                           .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int read_exact(int fd, uint8_t *buf, size_t want)
{
    size_t got = 0;
    for (unsigned tries = 0; tries < VPORT_READ_TRIES && got < want; tries++) {
        ssize_t n = read(fd, buf + got, want - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            printf("VPORT_TEST: FAIL read errno=%d\n", errno);
            return -1;
        }
        sleep_ms(20);
    }
    if (got != want) {
        printf("VPORT_TEST: FAIL short read got=%zu want=%zu\n", got, want);
        return -1;
    }
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    int fd = open("/dev/vport0", O_RDWR);
    if (fd < 0) {
        printf("VPORT_TEST: FAIL open /dev/vport0 errno=%d\n", errno);
        return 1;
    }

    int32_t detach = 0;
    if (ioctl(fd, VPORT_IOCTL_SET_CONSOLE, &detach) < 0) {
        printf("VPORT_TEST: FAIL ioctl SET_CONSOLE errno=%d\n", errno);
        close(fd);
        return 1;
    }

    printf("VPORT_TEST: READY\n");

    uint8_t buf[VPORT_TEST_BYTES];
    if (read_exact(fd, buf, sizeof(buf)) < 0) {
        close(fd);
        return 1;
    }

    /* The host probe sends a fixed, non-repeating pattern; anything else means
     * the receive path delivered the wrong bytes, which is worth failing on
     * rather than echoing back and letting the host's own check carry it. */
    for (size_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] != (uint8_t)('A' + (i % 26))) {
            printf("VPORT_TEST: FAIL payload byte %zu is 0x%02x\n",
                   i, (unsigned)buf[i]);
            close(fd);
            return 1;
        }
    }
    printf("VPORT_TEST: RX64\n");

    size_t sent = 0;
    while (sent < sizeof(buf)) {
        ssize_t n = write(fd, buf + sent, sizeof(buf) - sent);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            sleep_ms(20);
            continue;
        }
        printf("VPORT_TEST: FAIL write errno=%d after %zu bytes\n", errno, sent);
        close(fd);
        return 1;
    }

    struct vport_stats st;
    memset(&st, 0, sizeof(st));
    if (ioctl(fd, VPORT_IOCTL_GET_STATS, &st) == 0)
        printf("VPORT_TEST: rx=%u tx=%u dropped=%u timeouts=%u mirror=%u\n",
               st.rx_bytes, st.tx_bytes, st.rx_dropped, st.tx_timeouts,
               st.console_mirror);

    printf("VPORT_TEST: PASS\n");
    close(fd);
    return 0;
}
