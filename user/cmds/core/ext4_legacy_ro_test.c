/* Verify the explicit read-only downgrade for a legacy (v1/v2) JBD2 journal.
 *
 * The ext4 image on /dev/vdb has a journal, but mke2fs's legacy journal
 * superblock: no descriptor, commit or data checksums.  A kernel that cannot
 * tell a complete transaction from a torn one must not write through such a
 * log, so:
 *
 *   - a clean legacy image may be mounted READ-ONLY, through the explicit
 *     downgrade ("clean legacy checksum journal; explicit read-only
 *     downgrade"), and reads work;
 *   - a writable mount of the same image is refused (no replay/write
 *     support), rather than silently writing without crash protection.
 *
 * Exits 2 when the device is absent so a harness can distinguish "not
 * attached" from "behaved wrongly".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define LEGACY_MP "/tmp/legacyj"

static int fail(const char *why)
{
    printf("LEGACY_JOURNAL: FAIL %s errno=%d\n", why, errno);
    return 1;
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/vdb";
    int dirty = argc > 2 && strcmp(argv[2], "dirty") == 0;

    mkdir(LEGACY_MP, 0755);

    if (dirty) {
        /* A legacy log with pending transactions cannot be validated, so even
         * a read-only mount must be refused rather than replayed. */
        errno = 0;
        if (mount(dev, LEGACY_MP, "ext4", MS_RDONLY, "") == 0) {
            umount2(LEGACY_MP, 0);
            return fail("dirty legacy journal was admitted");
        }
        printf("LEGACY_JOURNAL: PASS dirty-rejected errno=%d\n", errno);
        rmdir(LEGACY_MP);
        return 0;
    }

    /* Read-only admission is the downgrade path.  The block device is selected
     * by the "/dev/vdX" spelling the mount path parser understands; there is
     * no devfs node to stat first. */
    errno = 0;
    if (mount(dev, LEGACY_MP, "ext4", MS_RDONLY, "") != 0) {
        int saved = errno;
        if (saved == ENODEV || saved == ENOENT) {
            printf("LEGACY_JOURNAL: SKIP no block device (%d)\n", saved);
            return 2;
        }
        printf("LEGACY_JOURNAL: FAIL ro-mount errno=%d\n", saved);
        return 1;
    }
    printf("LEGACY_JOURNAL: read-only mount admitted\n");

    /* The filesystem must be usable for reads. */
    char buf[64] = {0};
    int fd = open(LEGACY_MP "/hello.txt", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0)
            return fail("read hello.txt");
        printf("LEGACY_JOURNAL: read %ld bytes\n", (long)n);
    }

    /* Writes must fail: the mount is read-only, and the log has no checksums. */
    errno = 0;
    fd = open(LEGACY_MP "/writable.txt", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        close(fd);
        umount2(LEGACY_MP, 0);
        return fail("write opened on a read-only downgrade");
    }
    if (errno != EROFS) {
        umount2(LEGACY_MP, 0);
        return fail("write errno");
    }

    if (umount2(LEGACY_MP, 0) != 0)
        return fail("ro-umount");

    /* A writable mount has no replay support for a legacy log, so it must be
     * refused outright instead of running unprotected. */
    errno = 0;
    if (mount(dev, LEGACY_MP, "ext4", 0, "") == 0) {
        umount2(LEGACY_MP, 0);
        return fail("writable mount of a legacy journal was admitted");
    }
    printf("LEGACY_JOURNAL: writable mount refused (errno=%d)\n", errno);

    rmdir(LEGACY_MP);
    printf("LEGACY_JOURNAL: PASS ro-admitted rw-refused write=EROFS\n");
    return 0;
}
