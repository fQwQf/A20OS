/* Parallel TCP loopback load, used to generate real g_lwip_lock contention.
 *
 * The existing net tests are functional and run one at a time, so they put no
 * parallel traffic through the stack and cannot show whether sharding
 * g_lwip_lock would buy anything.  This runs WORKERS independent transfers
 * concurrently, each a forked server/client pair on its own port, so the
 * per-CPU lock traffic overlaps.
 *
 * Integrity is checked with an additive checksum, not just a byte count, so a
 * transfer that completes with the wrong bytes fails instead of passing.
 *
 * The one thing this test is NOT is a tier-agnostic pass/fail.  Each worker
 * needs three sockets alive at once -- the forked server's listener, the child
 * it accepts, and the client's connect -- so the workload's floor is
 * WORKERS * 3.  On a profile whose socket table is smaller than that floor the
 * run cannot succeed no matter what the stack does, and the honest output is a
 * SKIP naming the two numbers rather than a FAIL that reads like a stack bug
 * or a PASS earned by luck when the workers happen not to peak together.  The
 * ceiling is read from the kernel rather than compiled in: the userland build
 * is not rebuilt per NET_PROFILE (Makefile's USER_BUILD_ID does not contain
 * it), so a compiled-in constant would silently go stale. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define WORKERS   4
#define ROUNDS    4
#define CHUNK     (32 * 1024)
#define ROUND_LEN (1024 * 1024)
#define PORT_BASE 23400

/* Concurrent sockets the workload needs: listener + accepted child + client,
 * per worker.  Spelled as a product of the two so the SKIP line and the
 * derivation cannot drift apart. */
#define SOCKETS_PER_WORKER 3
#define SOCKETS_NEEDED     (WORKERS * SOCKETS_PER_WORKER)

/*
 * The kernel's socket-table ceiling, from "syscall-sockets: ... max=N" on
 * /proc/net/status.  Returns 0 when it cannot be read -- an older kernel, a
 * procfs that is not mounted, or a read that failed -- and the caller then runs
 * the test anyway rather than skipping on absent evidence.  Skipping because a
 * number could not be fetched would turn a working test into a silent no-op.
 */
static int socket_table_ceiling(void)
{
    static char buf[8192];
    char *p;
    int fd = open("/proc/net/status", O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';

    p = strstr(buf, "syscall-sockets:");
    if (!p)
        return 0;
    p = strstr(p, " max=");
    if (!p)
        return 0;
    return (int)strtol(p + 5, NULL, 10);
}

static unsigned long chunk_tag(size_t off, unsigned long seed)
{
    return seed + (unsigned long)(off * 2654435761UL);
}

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int run_server(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }

    int c = accept(fd, NULL, NULL);
    close(fd);
    if (c < 0)
        return -1;

    unsigned long sum = 0;
    size_t total = (size_t)ROUNDS * ROUND_LEN;
    char *buf = malloc(CHUNK);
    if (!buf) {
        close(c);
        return -1;
    }

    for (size_t got = 0; got < total; ) {
        size_t want = total - got;
        if (want > CHUNK)
            want = CHUNK;
        if (read_all(c, buf, want) < 0)
            goto fail;
        for (size_t i = 0; i < want; i++)
            sum += (unsigned char)buf[i];
        got += want;
    }
    free(buf);

    /* Report the checksum we observed so the sender can verify it. */
    if (write_all(c, &sum, sizeof(sum)) < 0) {
        close(c);
        return -1;
    }
    close(c);
    return 0;

fail:
    free(buf);
    close(c);
    return -1;
}

static int run_worker(int idx)
{
    int port = PORT_BASE + idx;
    unsigned long sum = 0;
    pid_t srv = fork();
    if (srv < 0)
        return -1;

    if (srv == 0) {
        _exit(run_server(port) == 0 ? 0 : 1);
    }

    int fd = -1;
    for (int tries = 0; tries < 200; tries++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((uint16_t)port);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            break;
        close(fd);
        fd = -1;
        usleep(5000);
    }
    if (fd < 0) {
        waitpid(srv, NULL, 0);
        return -1;
    }

    char *buf = malloc(CHUNK);
    if (!buf) {
        close(fd);
        waitpid(srv, NULL, 0);
        return -1;
    }

    size_t total = (size_t)ROUNDS * ROUND_LEN;
    for (size_t sent = 0; sent < total; ) {
        size_t want = total - sent;
        if (want > CHUNK)
            want = CHUNK;
        for (size_t i = 0; i < want; i++)
            buf[i] = (char)(chunk_tag(sent + i, 0) & 0xFF);
        if (write_all(fd, buf, want) < 0) {
            free(buf);
            close(fd);
            waitpid(srv, NULL, 0);
            return -1;
        }
        for (size_t i = 0; i < want; i++)
            sum += (unsigned char)buf[i];
        sent += want;
    }
    free(buf);

    unsigned long remote = 0;
    int bad = (read_all(fd, &remote, sizeof(remote)) < 0) || (remote != sum);
    close(fd);

    int status;
    waitpid(srv, &status, 0);
    if (bad)
        return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    return (sum == 0) ? -1 : 0;
}

int main(void)
{
    pid_t kids[WORKERS];
    int started = 0;
    int ceiling = socket_table_ceiling();

    /* Not a failure and not a pass: the profile cannot hold the workload, and
     * it never could.  Exit status 0 because nothing was broken and nothing
     * was proven -- every gate that wants a verdict keys on the literal
     * 'NET_STRESS_TEST: PASS', which this line deliberately is not, so a tier
     * too small for the test fails those gates instead of being read as
     * tested. */
    if (ceiling > 0 && ceiling < SOCKETS_NEEDED) {
        printf("NET_STRESS_TEST: SKIP (socket table ceiling %d < %d concurrent "
               "sockets required: %d workers x %d (listener + accepted child + "
               "client)); tier too small for this workload, upper limits left "
               "as configured\n",
               ceiling, SOCKETS_NEEDED, WORKERS, SOCKETS_PER_WORKER);
        return 0;
    }

    for (int i = 0; i < WORKERS; i++) {
        kids[i] = fork();
        if (kids[i] < 0)
            break;
        if (kids[i] == 0)
            _exit(run_worker(i) == 0 ? 0 : 1);
        started++;
    }

    int failed = (started != WORKERS);
    for (int i = 0; i < started; i++) {
        int status;
        if (waitpid(kids[i], &status, 0) < 0 ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0)
            failed = 1;
    }

    if (failed) {
        printf("NET_STRESS_TEST: FAIL\n");
        return 1;
    }
    printf("NET_STRESS_TEST: PASS (%d parallel transfers, %d rounds x %d B)\n",
           WORKERS, ROUNDS, ROUND_LEN);
    return 0;
}
