/* Deterministic BSD readiness-scaling gate.
 *
 * The backend used to probe every open socket with a non-blocking recv() on
 * every progress pass, so an idle connection population turned each wakeup
 * into an O(open) syscall storm.  Readiness is now driven by one kqueue, so a
 * burst delivered to one socket must not cost work proportional to the number
 * of idle sockets.  bsd_recv_probes in /proc/net/status makes that observable
 * without relying on wall-clock timing.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IDLE_SOCKETS 512
#define BURST 32
#define PROBE_LIMIT 1024

static int failures;
#define CHECK(cond, what) do { \
    if (!(cond)) { \
        printf("BSD_SCALE: FAIL %s (errno=%d)\n", what, errno); \
        failures++; \
    } \
} while (0)

static unsigned long read_probes(void)
{
    char buffer[4096];
    int fd = open("/proc/net/status", O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buffer[n] = '\0';
    const char *at = strstr(buffer, "bsd_recv_probes: ");
    if (!at)
        return 0;
    return strtoul(at + strlen("bsd_recv_probes: "), NULL, 10);
}

static int bind_loopback(int fd, unsigned port)
{
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    return bind(fd, (struct sockaddr *)&address, sizeof(address));
}

int main(void)
{
    static int idle[IDLE_SOCKETS];
    for (int i = 0; i < IDLE_SOCKETS; ++i) {
        idle[i] = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(idle[i] >= 0, "create idle UDP socket");
        if (idle[i] >= 0)
            CHECK(bind_loopback(idle[i], 0) == 0, "bind idle UDP socket");
    }

    int receiver = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    int sender = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(receiver >= 0 && sender >= 0, "create active UDP sockets");
    if (receiver < 0 || sender < 0)
        return 1;
    CHECK(bind_loopback(receiver, 39001) == 0, "bind receiver");

    unsigned long before = read_probes();
    char payload[32];
    memset(payload, 's', sizeof(payload));
    for (int i = 0; i < BURST; ++i) {
        ssize_t sent = sendto(sender, payload, sizeof(payload), 0,
                              (struct sockaddr *)&(struct sockaddr_in){
                                  .sin_family = AF_INET,
                                  .sin_port = htons(39001),
                                  .sin_addr.s_addr = inet_addr("127.0.0.1"),
                              }, sizeof(struct sockaddr_in));
        CHECK(sent == (ssize_t)sizeof(payload), "send burst datagram");
    }

    int received = 0;
    for (int attempt = 0; attempt < 40 && received < BURST; ++attempt) {
        struct pollfd ready = {.fd = receiver, .events = POLLIN};
        if (poll(&ready, 1, 100) != 1)
            continue;
        while (received < BURST) {
            ssize_t got = recv(receiver, payload, sizeof(payload), 0);
            if (got <= 0)
                break;
            received++;
        }
    }
    unsigned long after = read_probes();
    unsigned long probes = after - before;
    printf("BSD_SCALE: idle=%d burst=%d received=%d probes=%lu limit=%d\n",
           IDLE_SOCKETS, BURST, received, probes, PROBE_LIMIT);
    CHECK(received == BURST, "all burst datagrams reached the active socket");
    CHECK(probes < PROBE_LIMIT, "idle sockets do not add per-pass recv probes");

    for (int i = 0; i < IDLE_SOCKETS; ++i)
        if (idle[i] >= 0) close(idle[i]);
    close(sender);
    close(receiver);
    printf("BSD_SCALE: %s (%d failure%s)\n", failures ? "FAIL" : "PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
