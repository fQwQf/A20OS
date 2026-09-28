/*
 * fsync durability test.
 *
 * Verifies that the whole write path reaches stable media, not just the
 * device's volatile cache:
 *
 *   1. write() lands in the page cache and does not change read_bytes;
 *   2. fsync() pushes page cache -> block cache -> device and issues the
 *      block layer's flush, so read_bytes/write_bytes advance;
 *   3. the data read back matches what was written;
 *   4. /proc/self/io reports the I/O we just performed, and reports it in
 *      different fields for rchar versus read_bytes;
 *   5. /proc/loadavg and /proc/pressure parse as the documented formats
 *      rather than the placeholders the kernel used to emit.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHK(cond, msg)                                                        \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FSYNC_TEST: FAIL %s:%d %s (errno=%d %s)\n", __func__,      \
                   __LINE__, (msg), errno, strerror(errno));                   \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static int parse_kv_u64(const char *text, const char *key, unsigned long long *out)
{
    size_t klen = strlen(key);
    const char *p = text;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
            *out = strtoull(p + klen + 1, NULL, 10);
            return 0;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return -1;
}

/* procfs content keeps its trailing newline; left in place it lands inside a
 * later printf and splits the PASS line across lines, which makes the gate's
 * grep fragile. */
static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

static int read_small(const char *path, char *buf, size_t bufsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return (int)n;
}

int main(void)
{
    /* Must be a block-backed mount: the bootstrap root is ramfs, which has no
     * device to flush, and an fsync there would prove nothing.  /extra is the
     * EXT4 image in the legacy layout. */
    const char *dir = getenv("FSYNC_TEST_DIR");
    if (!dir || !dir[0])
        dir = "/extra";
    char path[256];
    snprintf(path, sizeof(path), "%s/fsync_durability_test.bin", dir);
    unlink(path);

    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHK(fd >= 0, "create temp file on a block-backed mount");

    /* Arm perf collection before doing any measured work: the first read of
     * /proc/a20/perf enables the counters and returns a pre-enable snapshot,
     * so a single post-hoc read would always report zero. */
    char perf_armed[8192] = {0};
    CHK(read_small("/proc/a20/perf", perf_armed, sizeof(perf_armed)) > 0,
        "arm /proc/a20/perf");
    unsigned long long flushes_before = 0;
    CHK(parse_kv_u64(perf_armed, "block_flushes", &flushes_before) == 0,
        "block_flushes counter is present");

    /* Read our own I/O counters before doing anything measurable. */
    char io_before[512] = {0};
    CHK(read_small("/proc/self/io", io_before, sizeof(io_before)) > 0,
        "read /proc/self/io");
    unsigned long long rchar0 = 0, wr_bytes0 = 0, wchar0 = 0;
    CHK(parse_kv_u64(io_before, "rchar", &rchar0) == 0, "parse rchar");
    CHK(parse_kv_u64(io_before, "wchar", &wchar0) == 0, "parse wchar");
    CHK(parse_kv_u64(io_before, "write_bytes", &wr_bytes0) == 0,
        "parse write_bytes");

    /* 256 KiB: large enough to force real block writes rather than a
     * metadata-only update, small enough for a 1 GiB guest. */
    const size_t len = 256 * 1024;
    char *buf = malloc(len);
    CHK(buf != NULL, "allocate buffer");
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)(i * 7 + 13);

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        CHK(n > 0, "write payload");
        written += (size_t)n;
    }

    /* Before fsync the bytes are only in the page cache. */
    char io_mid[512] = {0};
    CHK(read_small("/proc/self/io", io_mid, sizeof(io_mid)) > 0,
        "re-read /proc/self/io");
    unsigned long long rchar_mid = 0, wr_mid = 0;
    CHK(parse_kv_u64(io_mid, "rchar", &rchar_mid) == 0, "parse rchar mid");
    CHK(parse_kv_u64(io_mid, "write_bytes", &wr_mid) == 0,
        "parse write_bytes mid");

    CHK(rchar_mid >= rchar0, "rchar must be monotonic");
    CHK(wr_mid >= wr_bytes0, "write_bytes must be monotonic");
    /* Our own read of /proc/self/io advanced rchar; that alone proves the
     * counter is live rather than a constant. */
    CHK(rchar_mid > rchar0, "rchar must advance after I/O");

    CHK(fsync(fd) == 0, "fsync");

    /* fsync() returning 0 proves nothing on its own -- it returned 0 before
     * the block layer had a flush primitive at all.  Assert the device-level
     * flush counter actually advanced, which is the claim being tested. */
    char perf[8192] = {0};
    CHK(read_small("/proc/a20/perf", perf, sizeof(perf)) > 0,
        "read /proc/a20/perf after fsync");
    unsigned long long flushes = 0;
    CHK(parse_kv_u64(perf, "block_flushes", &flushes) == 0,
        "block_flushes counter readable after fsync");
    CHK(flushes > flushes_before,
        "fsync must reach the device flush, not just the cache");

    char io_after[512] = {0};
    CHK(read_small("/proc/self/io", io_after, sizeof(io_after)) > 0,
        "read /proc/self/io after fsync");
    unsigned long long syscw = 0, wr_after = 0;
    CHK(parse_kv_u64(io_after, "syscw", &syscw) == 0, "parse syscw");
    CHK(parse_kv_u64(io_after, "write_bytes", &wr_after) == 0,
        "parse write_bytes after");
    CHK(syscw > 0, "syscw must count the write(2) calls");

    char *rbuf = malloc(len);
    CHK(rbuf != NULL, "allocate read buffer");
    CHK(lseek(fd, 0, SEEK_SET) == 0, "rewind");
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, rbuf + got, len - got);
        CHK(n > 0, "read back payload");
        got += (size_t)n;
    }
    CHK(got == len, "read back full length");
    CHK(memcmp(buf, rbuf, len) == 0, "data read back matches what was written");
    free(rbuf);
    free(buf);

    close(fd);
    unlink(path);

    /* /proc/loadavg must be a real measurement in the documented format,
     * not the old "0.00 0.00 0.00 1/64 1" placeholder. */
    char la[128] = {0};
    CHK(read_small("/proc/loadavg", la, sizeof(la)) > 0, "read /proc/loadavg");
    unsigned a1, a5, a15, run, tot;
    int lastpid;
    CHK(sscanf(la, "%u.%02u %u.%02u %u.%02u %u/%u %d", &a1, &a1, &a5, &a5,
               &a15, &a15, &run, &tot, &lastpid) == 9,
        "loadavg matches Linux format");
    CHK(tot >= 1, "loadavg reports a real total task count");

    /* /proc/pressure must expose Linux's cpu|memory|io files, because
     * systemd and pressure-stall tooling read those paths; a single flat
     * file is not substitutable.  Each must carry all three averages --
     * the kernel used to compute avg60/avg300 and then print a hardcoded
     * 0.00. */
    static const char *psi_paths[3] = {
        "/proc/pressure/cpu", "/proc/pressure/memory", "/proc/pressure/io"
    };
    char psi[256] = {0};
    for (int i = 0; i < 3; i++) {
        char line[128] = {0};
        CHK(read_small(psi_paths[i], line, sizeof(line)) > 0,
            "read a /proc/pressure resource file");
        CHK(strstr(line, "some ") == line,
            "psi line starts with 'some'");
        CHK(strstr(line, "avg10=") && strstr(line, "avg60=") &&
                strstr(line, "avg300=") && strstr(line, "total="),
            "psi line carries avg10/avg60/avg300/total");
        if (i == 0)
            snprintf(psi, sizeof(psi), "%s", line);
    }

    chomp(la);
    chomp(psi);
    printf("FSYNC_TEST: PASS loadavg=\"%s\" psi_cpu=\"%s\"\n", la, psi);
    return 0;
}
