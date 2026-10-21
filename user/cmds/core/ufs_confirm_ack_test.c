/* Client side of the lost-ACK drill for the UFSD close-token protocol.
 *
 * The gate boots a UFSD built with the close-token test fault, which drops the
 * reply to the first accepted CLOSE and to the first accepted CONFIRM.  A
 * plain open/write/close then drives the whole contract:
 *
 *   CLOSE reply lost  -> the same token is retried and the service replays the
 *                        accepted outcome without closing the backend twice
 *   CONFIRM reply lost-> the confirmation is ambiguous; the accepted result
 *                        stays in the bounded tombstone, and the close is not
 *                        redone
 *
 * What this binary asserts is that neither loss is visible to the caller: the
 * close succeeds, and the file it wrote is intact afterwards.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MP "/ufs"
#define PATH MP "/ACK.TXT"

static const char payload[] = "confirm-ack\n";

static int fail(const char *what)
{
    printf("CONFIRM_ACK: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int wait_mount(void)
{
    struct stat st;
    for (int i = 0; i < 500; i++) {
        if (stat(MP "/HELLO.TXT", &st) == 0)
            return 0;
        usleep(20000);
    }
    return -1;
}

int main(void)
{
    if (wait_mount() != 0)
        return fail("service mount");

    unlink(PATH);
    int fd = open(PATH, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return fail("create");
    if (write(fd, payload, sizeof(payload) - 1) != (ssize_t)(sizeof(payload) - 1)) {
        close(fd);
        return fail("write");
    }

    /* With close tokens negotiated this is where CLOSE and CONFIRM run.  Both
     * of their first replies are dropped by the service. */
    errno = 0;
    if (close(fd) != 0)
        return fail("close across the lost ACKs");

    /* The close must not have been re-executed or left half-done: the data is
     * still readable and a second open/close cycle works normally. */
    char buf[32] = {0};
    fd = open(PATH, O_RDONLY);
    if (fd < 0)
        return fail("reopen");
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n != (ssize_t)(sizeof(payload) - 1) ||
        memcmp(buf, payload, sizeof(payload) - 1) != 0)
        return fail("content after lost ACKs");

    fd = open(PATH, O_RDWR);
    if (fd < 0)
        return fail("second open");
    if (close(fd) != 0)
        return fail("second close");

    unlink(PATH);
    printf("CONFIRM_ACK: PASS close survived lost CLOSE and CONFIRM replies\n");
    return 0;
}
