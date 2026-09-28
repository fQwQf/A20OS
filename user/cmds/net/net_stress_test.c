/* Parallel TCP loopback load, used to generate real g_lwip_lock contention.
 *
 * The existing net tests are functional and run one at a time, so they put no
 * parallel traffic through the stack and cannot show whether sharding
 * g_lwip_lock would buy anything.  This runs WORKERS independent transfers
 * concurrently, each a forked server/client pair on its own port, so the
 * per-CPU lock traffic overlaps.
 *
 * Integrity is checked with an additive checksum, not just a byte count, so a
 * transfer that completes with the wrong bytes fails instead of passing. */

#include <errno.h>
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
