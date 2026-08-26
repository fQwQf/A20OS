/* tcp_edge_test -- mandatory TCP edge-semantics coverage for the network suite.
 *
 * Scenarios (all required; no skip path):
 *   1. partial-io : stream integrity under short reads -- client sends 8 KiB
 *      in irregular chunks, server recvs through a tiny 64-byte buffer until
 *      the full payload arrives, then verifies content byte-for-byte.
 *   2. refused    : connect() to a listener-less loopback port must fail
 *      (bounded by alarm so a black-holing stack fails instead of hanging).
 *   3. epipe      : peer closes without reading; sender with SIGPIPE ignored
 *      must eventually observe send() == -1/EPIPE (bounded by alarm).
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/in.h>

#define TOTAL_LEN 8192
#define CHUNK_MAX 1024
#define PORT_PARTIAL 12401
#define PORT_REFUSED 12399
#define PORT_EPIPE   12402

static void fill_pattern(unsigned char *buf, size_t off, size_t len) {
    for (size_t i = 0; i < len; i++)
        buf[i] = (unsigned char)((off + i) & 0xff);
}

static int bind_listen(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* --- scenario 1: partial reads ------------------------------------- */

static int partial_server(void) {
    int fd = bind_listen(PORT_PARTIAL);
    if (fd < 0)
        _exit(1);
    int c = accept(fd, NULL, NULL);
    close(fd);
    if (c < 0)
        _exit(1);

    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    unsigned char expect[TOTAL_LEN];
    fill_pattern(expect, 0, TOTAL_LEN);

    unsigned char rbuf[64];
    unsigned char acc[TOTAL_LEN];
    size_t got = 0;
    while (got < TOTAL_LEN) {
        ssize_t n = recv(c, rbuf, sizeof(rbuf), 0);
        if (n <= 0)
            break;
        memcpy(acc + got, rbuf, (size_t)n);
        got += (size_t)n;
    }
    close(c);
    _exit(got == TOTAL_LEN && memcmp(acc, expect, TOTAL_LEN) == 0 ? 0 : 1);
}

static int partial_client(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(PORT_PARTIAL);

    /* retry while the child listener comes up */
    for (int i = 0;; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            break;
        if (i >= 200) { /* ~2s */
            close(fd);
            return -1;
        }
        usleep(10000);
    }

    unsigned char sbuf[CHUNK_MAX];
    size_t sent = 0;
    /* deliberately irregular chunk sizes to defeat any fixed-size framing */
    static const size_t sizes[] = { 1024, 333, 777, 512, 2048, 129, 961,
                                    1024, 640, 752 };
    size_t idx = 0;
    while (sent < TOTAL_LEN) {
        size_t want = sizes[idx++ % (sizeof(sizes) / sizeof(sizes[0]))];
        if (want > sizeof(sbuf))
            want = sizeof(sbuf);
        if (want > TOTAL_LEN - sent)
            want = TOTAL_LEN - sent;
        fill_pattern(sbuf, sent, want);
        size_t done = 0;
        while (done < want) { /* send() short writes are legal: loop */
            ssize_t n = send(fd, sbuf + done, want - done, 0);
            if (n <= 0) {
                close(fd);
                return -1;
            }
            done += (size_t)n;
        }
        sent += want;
    }
    close(fd);
    return 0;
}

static int scenario_partial_io(int *child_status) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0)
        partial_server();
    usleep(100000);
    int rc = partial_client();
    if (waitpid(pid, child_status, 0) < 0)
        return -1;
    if (rc != 0)
        return -1;
    if (!WIFEXITED(*child_status) || WEXITSTATUS(*child_status) != 0)
        return -1;
    return 0;
}

/* --- scenario 2: connection refused -------------------------------- */

static void on_alarm(int signo) {
    (void)signo;
    _exit(42); /* distinguishable timeout marker */
}

static int scenario_refused(int *child_status) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        alarm(5);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            _exit(1);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(PORT_REFUSED); /* nothing listens here */
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            _exit(1); /* must NOT succeed */
        _exit(0);
    }
    if (waitpid(pid, child_status, 0) < 0)
        return -1;
    if (!WIFEXITED(*child_status))
        return -1;
    if (WEXITSTATUS(*child_status) == 42)
        return -1; /* stack swallowed the refusal -> timed out */
    if (WEXITSTATUS(*child_status) != 0)
        return -1;
    return 0;
}

/* --- scenario 3: EPIPE after peer close ----------------------------- */

static int epipe_server(void) {
    int fd = bind_listen(PORT_EPIPE);
    if (fd < 0)
        _exit(1);
    int c = accept(fd, NULL, NULL);
    close(fd);
    if (c < 0)
        _exit(1);
    close(c); /* close WITHOUT reading -> RST to the sender */
    _exit(0);
}

static int scenario_epipe(int *child_status) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0)
        epipe_server();
    usleep(100000);

    int rc = -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(PORT_EPIPE);

        int connected = 0;
        for (int i = 0; i < 200; i++) {
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                connected = 1;
                break;
            }
            usleep(10000);
        }
        if (!connected) {
            printf("TCP_EDGE_TEST: epipe connect failed errno=%d\n", errno);
        } else {
            signal(SIGPIPE, SIG_IGN);
            alarm(8);
            unsigned char junk[512];
            memset(junk, 0x5a, sizeof(junk));
            int errored = 0;
            for (int i = 0; i < 512; i++) { /* push until the RST lands */
                ssize_t n = send(fd, junk, sizeof(junk), 0);
                if (n < 0) {
                    printf("TCP_EDGE_TEST: epipe send errno=%d (%s)\n",
                           errno, strerror(errno));
                    rc = (errno == EPIPE) ? 0 : -1;
                    errored = 1;
                    break;
                }
                usleep(10000); /* let the peer RST arrive */
            }
            if (!errored)
                printf("TCP_EDGE_TEST: epipe no error after %d sends\n", 512);
            alarm(0);
        }
        close(fd);
    }
    int st;
    waitpid(pid, &st, 0);
    *child_status = st;
    return rc;
}

int main(void) {
    int cs;

    if (scenario_partial_io(&cs) != 0) {
        printf("TCP_EDGE_TEST: FAIL (partial-io)\n");
        return 1;
    }
    if (scenario_refused(&cs) != 0) {
        printf("TCP_EDGE_TEST: FAIL (refused)\n");
        return 1;
    }
    if (scenario_epipe(&cs) != 0) {
        printf("TCP_EDGE_TEST: FAIL (epipe)\n");
        return 1;
    }

    printf("TCP_EDGE_TEST: PASS\n");
    return 0;
}
