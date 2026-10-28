/* Identical Linux ELF for connection capacity and guest memory snapshots.
 * Each retained connection exchanges a unique token before and after opening
 * the others. Connection setup time is not a bulk throughput measurement. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static double seconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) exit(2);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int exchange(int fd, unsigned token)
{
    unsigned reply = ~token;
    if (send(fd, &token, sizeof(token), MSG_NOSIGNAL) != sizeof(token)) return -1;
    size_t received = 0;
    while (received < sizeof(reply)) {
        ssize_t n = recv(fd, (char *)&reply + received, sizeof(reply) - received, 0);
        if (n <= 0) return -1;
        received += (size_t)n;
    }
    if (reply != token) { errno = EIO; return -1; }
    return 0;
}

static void snapshot(const char *phase)
{
    const char *paths[] = {"/proc/meminfo", "/proc/net/sockstat", "/proc/a20/netmem", "/proc/net/status"};
    printf("MEMORY_BEGIN %s\n", phase);
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        printf("FILE %s\n", paths[i]);
        char text[512];
        while (fgets(text, sizeof(text), f)) fputs(text, stdout);
        fclose(f);
    }
    printf("MEMORY_END %s\n", phase);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    unsigned requested = (unsigned)strtoul(argv[3], NULL, 10);
    if (!requested || requested > 8192) return 2;
    int *fds = calloc(requested, sizeof(*fds));
    if (!fds) return 2;
    /* Touch the user-space array before taking the baseline. */
    for (unsigned i = 0; i < requested; ++i) fds[i] = -1;
    struct sockaddr_in peer = { .sin_family = AF_INET, .sin_port = htons(atoi(argv[2])) };
    if (inet_pton(AF_INET, argv[1], &peer.sin_addr) != 1) return 2;
    snapshot("baseline");
    unsigned established = 0;
    int error = 0;
    double start = seconds();
    for (; established < requested; ++established) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { error = errno; break; }
        fds[established] = fd;
        struct timeval timeout = { .tv_sec = 3 };
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
            connect(fd, (void *)&peer, sizeof(peer)) || exchange(fd, established)) {
            error = errno;
            break;
        }
    }
    /* Verify the first connections still work at the final population. */
    int verified = established == requested;
    for (unsigned i = 0; i < established; ++i)
        if (exchange(fds[i], i ^ 0xa5200000U)) { verified = 0; error = errno; break; }
    double elapsed = seconds() - start;
    snapshot("held");
    for (unsigned i = 0; i < requested; ++i)
        if (fds[i] >= 0) close(fds[i]);
    snapshot("closed");
    /* TIME_WAIT and slab/page caches intentionally remain observable. */
    sleep(2);
    snapshot("closed_2s");
    printf("CONNECTION_RESULT requested=%u established=%u verified=%d errno=%d seconds=%.6f\n",
           requested, established, verified, error, elapsed);
    free(fds);
    return verified ? 0 : 1;
}
