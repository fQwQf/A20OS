/*
 * Host-driven TCP traffic for the multi-lane lwIP core probe.
 *
 * The parent starts one listener per lane-selected local port, then emits a
 * READY marker so the host harness can connect to all QEMU hostfwd rules at
 * once. Each child echoes a fixed-size stream and reports a content digest;
 * the same digest is expected from one-lane and four-lane kernels.
 */
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HOST_TEST_BYTES (64u * 1024u)
#define HOST_TEST_TIMEOUT_SEC 20
#define HOST_TEST_MAX_PORTS 8

static uint64_t digest_bytes(uint64_t hash, const unsigned char *data,
                             size_t len) {
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static unsigned char payload_byte(size_t pos) {
    return (unsigned char)((pos * 37u + (pos >> 7) + 0x5bu) & 0xffu);
}

static int set_io_timeout(int fd) {
    struct timeval tv = {.tv_sec = HOST_TEST_TIMEOUT_SEC, .tv_usec = 0};
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
                   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0
               ? 0
               : -1;
}

static int read_full(int fd, unsigned char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, buf + off, len - off);
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static int write_full(int fd, const unsigned char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static int open_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (set_io_timeout(fd) < 0)
        goto fail_fd;

    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_aton("10.0.2.15", &addr.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 1) < 0)
        goto fail_fd;
    return fd;

fail_fd:
    close(fd);
    return -1;
}

static int serve_fd(int fd, uint16_t port) {
    int peer = accept(fd, NULL, NULL);
    if (peer < 0 || set_io_timeout(peer) < 0)
        goto fail_peer;

    unsigned char buf[1024];
    uint64_t digest = UINT64_C(14695981039346656037);
    size_t remaining = HOST_TEST_BYTES;
    size_t offset = 0;
    while (remaining != 0) {
        size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
        if (read_full(peer, buf, chunk) < 0)
            goto fail_peer;
        for (size_t i = 0; i < chunk; ++i) {
            if (buf[i] != payload_byte(offset + i))
                goto fail_peer;
        }
        digest = digest_bytes(digest, buf, chunk);
        if (write_full(peer, buf, chunk) < 0)
            goto fail_peer;
        remaining -= chunk;
        offset += chunk;
    }
    close(peer);
    printf("NET_LANE_HOSTFWD_CHILD: PASS port=%u bytes=%u digest=%016llx\n",
           port, HOST_TEST_BYTES, (unsigned long long)digest);
    fflush(stdout);
    return 0;

fail_peer:
    if (peer >= 0)
        close(peer);
    return -1;
}

int main(int argc, char **argv) {
    uint16_t ports[HOST_TEST_MAX_PORTS];
    pid_t children[HOST_TEST_MAX_PORTS];
    int listeners[HOST_TEST_MAX_PORTS];
    int statuses[HOST_TEST_MAX_PORTS];
    int count = argc - 1;
    if (count < 2 || count > HOST_TEST_MAX_PORTS) {
        printf("NET_LANE_HOSTFWD_TEST: FAIL (expected 2..%d ports)\n",
               HOST_TEST_MAX_PORTS);
        return 1;
    }

    for (int i = 0; i < count; ++i) {
        char *end = NULL;
        long value = strtol(argv[i + 1], &end, 10);
        if (!end || *end || value < 1 || value > 65535) {
            printf("NET_LANE_HOSTFWD_TEST: FAIL (invalid port %s)\n",
                   argv[i + 1]);
            return 1;
        }
        ports[i] = (uint16_t)value;
        for (int j = 0; j < i; ++j) {
            if (ports[j] == ports[i]) {
                printf("NET_LANE_HOSTFWD_TEST: FAIL (duplicate port %u)\n",
                       ports[i]);
                return 1;
            }
        }
    }

    for (int i = 0; i < count; ++i) {
        listeners[i] = open_listener(ports[i]);
        if (listeners[i] < 0) {
            for (int j = 0; j < i; ++j)
                close(listeners[j]);
            printf("NET_LANE_HOSTFWD_TEST: FAIL (listen port=%u errno=%d)\n",
                   ports[i], errno);
            return 1;
        }
    }

    int started = 0;
    for (int i = 0; i < count; ++i) {
        pid_t pid = fork();
        if (pid < 0)
            break;
        if (pid == 0) {
            for (int j = 0; j < count; ++j) {
                if (j != i)
                    close(listeners[j]);
            }
            int rc = serve_fd(listeners[i], ports[i]);
            close(listeners[i]);
            if (rc != 0) {
                printf("NET_LANE_HOSTFWD_CHILD: FAIL port=%u errno=%d\n",
                       ports[i], errno);
                fflush(stdout);
            }
            _exit(rc == 0 ? 0 : 1);
        }
        children[i] = pid;
        started++;
    }

    if (started != count) {
        for (int i = 0; i < count; ++i)
            close(listeners[i]);
        for (int i = 0; i < started; ++i) {
            (void)kill(children[i], SIGTERM);
            (void)waitpid(children[i], NULL, 0);
        }
        printf("NET_LANE_HOSTFWD_TEST: FAIL (forked %d of %d listeners)\n",
               started, count);
        return 1;
    }

    printf("NET_LANE_HOSTFWD_READY: count=%d", count);
    for (int i = 0; i < count; ++i)
        printf(" port=%u", ports[i]);
    printf("\n");
    fflush(stdout);
    for (int i = 0; i < count; ++i)
        close(listeners[i]);

    int failed = 0;
    for (int i = 0; i < count; ++i) {
        int status = 0;
        pid_t got;
        do {
            got = waitpid(children[i], &status, 0);
        } while (got < 0 && errno == EINTR);
        statuses[i] =
            got == children[i] && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        failed |= statuses[i] != 0;
    }
    if (failed) {
        printf("NET_LANE_HOSTFWD_TEST: FAIL (one or more host connections "
               "failed)\n");
        return 1;
    }

    printf("NET_LANE_HOSTFWD_TEST: PASS connections=%d bytes=%u "
           "payload=pattern-v1\n",
           count, HOST_TEST_BYTES * (unsigned)count);
    return 0;
}
