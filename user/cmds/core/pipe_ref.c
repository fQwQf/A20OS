/*
 * pipe_ref — Linux ABI reference for the native pipe personality
 * (docs/hybrid-kernel/05-idl-and-personality.md).
 *
 * Runs the same sequence as test_native_personality's byte-stream and
 * level-triggered readiness checks on a real Linux pipe(2) and prints
 * a normalized PIPE_REF line that the smoke compares with the native
 * implementation's output.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

int main(void)
{
    int fds[2];
    if (pipe(fds) != 0)
        return 1;

    /* Byte stream across two writes, drained by two partial reads. */
    const char *seg1 = "hello ";
    const char *seg2 = "world";
    if (write(fds[1], seg1, 6) != 6 || write(fds[1], seg2, 5) != 5)
        return 2;

    char buf[16];
    ssize_t n1 = read(fds[0], buf, 6);
    if (n1 != 6 || memcmp(buf, "hello ", 6) != 0)
        return 3;
    ssize_t n2 = read(fds[0], buf, sizeof(buf));
    if (n2 != 5 || memcmp(buf, "world", 5) != 0)
        return 4;

    /* Level-triggered readiness via poll(). */
    const char *lev = "level";
    if (write(fds[1], lev, 5) != 5)
        return 5;
    struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
    if (poll(&pfd, 1, 1000) != 1)
        return 6;
    char lbuf[8];
    ssize_t m1 = read(fds[0], lbuf, 2);
    if (m1 != 2)
        return 7;
    pfd.revents = 0;
    if (poll(&pfd, 1, 1000) != 1)
        return 8; /* still readable: level semantics */
    ssize_t m2 = read(fds[0], lbuf, sizeof(lbuf));
    if (m2 != 3)
        return 9;
    pfd.revents = 0;
    if (poll(&pfd, 1, 50) != 0)
        return 10; /* drained: not readable */

    printf("PIPE_REF: partial=%ld rest=%ld joined=hello world level=ok\n",
           (long)n1, (long)n2);
    return 0;
}
