/* Assert that an IPv6 loopback TCP connection can be established, accepted
 * and used in both directions.
 *
 * This is the gate for the v6 inbound-listen work.  Before it, an AF_INET6
 * SOCK_STREAM socket() succeeded but got no tcp_pcb at all
 * (net_inet_socket_init's pcb arm was AF_INET-only), so bind() had nothing to
 * bind, listen() had nothing to convert into a LISTEN pcb, and accept() could
 * never return a v6 fd.  The failure a caller saw was -ECONNREFUSED on
 * connect(), which reads like "nothing is listening" rather than "this kernel
 * has no v6 path", so the defect hid behind an ordinary-looking errno.
 *
 * Three things are asserted, deliberately separable:
 *
 *   1. bind/listen/accept on ::1 returns a usable fd, and the accepted peer's
 *      address is a v6 sockaddr (not a v4-shaped one reinterpreted -- the
 *      accept drain used to hardcode child->domain = AF_INET).
 *   2. Client -> server bytes arrive intact.
 *   3. Server -> client bytes arrive intact, on the fd accept() returned.
 *
 * (2) and (3) are separate because a half-broken data plane that only carries
 * one direction is the failure mode that a single one-way test would miss.
 *
 * Runs in both TCP modes, like tcp_accept_test does for v4: under "tcpmode
 * fast" the listener is matched by the socket layer pairing the two sockets,
 * under "tcpmode lwip" it is a real lwIP LISTEN pcb and the handshake is
 * completed by the stack.  Both must produce the same observable result, so
 * running both is what keeps the two implementations honest.
 *
 * Every blocking step is bracketed by SO_RCVTIMEO, so a hang fails the gate
 * with EAGAIN instead of parking QEMU forever. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#define TEST_PORT_DEFAULT 13001
/* Long enough that a loaded 4-core guest does not trip it, short enough that a
 * wedged handshake fails the gate in tens of seconds rather than minutes. */
#define IO_TIMEOUT_SEC 5
/* Retry budget in microseconds.  The unit has to match the step the loop
 * accumulates: `waited` grows by CONNECT_RETRY_US, so comparing it against a
 * millisecond bound would end the loop after a single attempt.  One attempt
 * races the forked server's bind()+listen(), which is what makes a healthy
 * listener look intermittently broken. */
#define CONNECT_BUDGET_US 4000000
#define CONNECT_RETRY_US 20000

/* Payload large enough to span several MSS-sized segments, so the test would
 * notice a data path that only ever moves one segment.  4000 B is three
 * segments at the default profile's TCP_MSS of 1460. */
#define PAYLOAD_LEN 4000

/* Linux TCP state for a listener, as /proc/net/tcp6 prints it. */
#define TCP_LISTEN_ST 0x0A

static int arm_timeout(int fd)
{
    struct timeval tv;
    tv.tv_sec = IO_TIMEOUT_SEC;
    tv.tv_usec = 0;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void fill_payload(unsigned char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (unsigned char)(i * 7 + 13);
}

static int test_port;
static const char *phase = "start";

static int v6_addr(struct sockaddr_in6 *a6, uint16_t port)
{
    memset(a6, 0, sizeof(*a6));
    a6->sin6_family = AF_INET6;
    a6->sin6_port = htons(port);
    return inet_pton(AF_INET6, "::1", &a6->sin6_addr) == 1 ? 0 : -1;
}

/*
 * Assert this listener is visible in /proc/net/tcp6, in the tcp6 layout, and
 * absent from /proc/net/tcp.
 *
 * Checked from inside the test, while the listener is still open, because that
 * is the only moment it exists: a shell gate that `cat`s /proc/net/tcp6 after
 * the test returns always reads an empty file, because every socket the test
 * made has already been closed -- so a gate written that way could not have
 * detected a missing or empty tcp6 file at all.
 *
 * Linux splits the two families: /proc/net/tcp lists IPv4, /proc/net/tcp6 lists
 * IPv6.  A tcp6 row renders the address as four 32-bit words in upper case, so
 * a listener bound to ::1 reads "00000000000000000000000001000000:<port>"
 * followed by the state; a tcp row renders one 32-bit word, so the same field
 * is 8 hex digits.  Matching on the exact hex width is therefore what pins the
 * layout -- a v4 row cannot satisfy the 32-digit match, and a v6 row rendered
 * in the v4 layout cannot produce 32 digits either.
 */
#define V6_LOOPBACK_HEX  "00000000000000000000000001000000"

static int check_proc_net(void)
{
    char want_port[8];
    snprintf(want_port, sizeof(want_port), ":%04X", (unsigned)test_port);
    size_t want_port_len = strlen(want_port);
    int bad = 0;

    for (int which = 0; which < 2; which++) {
        const char *path = which ? "/proc/net/tcp6" : "/proc/net/tcp";
        /* tcp6 addresses are four 32-bit words; tcp addresses are one. */
        size_t hexdigits = which ? 32 : 8;
        int found = 0;
        int listen_found = 0;
        char content[4096];
        size_t used = 0;

        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            printf("  procheck: %s: %s\n", path, strerror(errno));
            bad = 1;
            continue;
        }
        /* One pass.  These files are rendered per read() from a running
         * offset, so a second read after the first has consumed the content
         * comes back empty -- parsing a live stream in two sweeps would report
         * a file that is perfectly present as missing. */
        while (used + 1 < sizeof(content)) {
            ssize_t n = read(fd, content + used, sizeof(content) - 1 - used);
            if (n <= 0)
                break;
            used += (size_t)n;
        }
        content[used] = '\0';
        close(fd);
        if (which)
            printf("  procheck: %s read %zu bytes\n", path, used);
        char raw[4096];
        memcpy(raw, content, used + 1);
        size_t raw_used = used;

        char *save = content;
        char *nl;
        while ((nl = strchr(save, '\n')) != NULL) {
            *nl = '\0';
            char *row = save;
            save = nl + 1;
            char *colon = strchr(row, ':');
            if (!colon)
                continue;               /* header line */
            /* "   0: <local> <remote> ..." -- the row format puts a space
             * after the sl: field, so skip it to reach local_address. */
            char *addr = colon + 1;
            while (*addr == ' ')
                addr++;
            size_t alen = 0;
            while (addr[alen] && addr[alen] != ' ')
                alen++;
            /* want_port already carries its ':', so it is the whole ":32C9"
             * tail -- hexdigits addresses plus want_port_len, nothing more. */
            if (alen != hexdigits + want_port_len)
                continue;               /* wrong address width for this file */
            if (memcmp(addr + hexdigits, want_port, want_port_len) != 0)
                continue;               /* a different port */
            found = 1;
            if (!which) {
                /* /proc/net/tcp must not list a v6 port at all.  Keep scanning:
                 * one bad row is enough to fail, and stopping at the first
                 * match could hide a second one. */
                printf("  procheck: %s lists v6 port %d; that file is "
                       "IPv4-only; row was: %.200s\n", path, test_port, row);
                bad = 1;
                continue;
            }
            /* The state column follows the remote address, in the same
             * width+port shape, so skip past it. */
            char *st = addr + alen;
            while (*st == ' ')
                st++;
            st += hexdigits + want_port_len;
            while (*st == ' ')
                st++;
            /* Do NOT stop at the first row for this port.  In fast mode the
             * listener shares its port with the connection the client has
             * already established, so an ESTABLISHED row for the same local
             * port legitimately precedes the LISTEN one; the assertion is
             * about the listener, so scan every row and require that one of
             * them is in LISTEN. */
            if (st[0] == '0' && st[1] == 'A') {
                listen_found = 1;
                if (memcmp(addr, V6_LOOPBACK_HEX, 32) != 0) {
                    printf("  procheck: %s listener local_address is %.32s, "
                           "expected %s (::1)\n", path, addr, V6_LOOPBACK_HEX);
                    bad = 1;
                }
            }
        }
        if (which && found && !listen_found) {
            printf("  procheck: %s has no LISTEN row for v6 port %d "
                   "(no state 0A); file was:\n", path, test_port);
            char dump[4096];
            memcpy(dump, raw, raw_used);
            dump[raw_used] = '\0';
            for (char *q = dump; *q; ) {
                char *e = strchr(q, '\n');
                if (!e)
                    break;
                *e = '\0';
                printf("    |%s\n", q);
                q = e + 1;
            }
            bad = 1;
        }
        if (which && !found) {
            /* Dump from an untouched copy: parsing above replaced the newlines
             * with terminators, so walking `content` here would print nothing
             * and hide the very evidence this message exists to provide. */
            printf("  procheck: %s has no row for v6 port %d; file was:\n",
                   path, test_port);
            char dump[4096];
            memcpy(dump, raw, raw_used);
            dump[raw_used] = '\0';
            for (char *q = dump; *q; ) {
                char *e = strchr(q, '\n');
                if (!e)
                    break;
                *e = '\0';
                printf("    |%s\n", q);
                q = e + 1;
            }
            bad = 1;
        }
    }
    return bad;
}

static int server(void)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("  server: socket: %s\n", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in6 a6;
    if (v6_addr(&a6, (uint16_t)test_port) < 0)
        return -1;
    if (bind(fd, (struct sockaddr *)&a6, sizeof(a6)) < 0) {
        printf("  server: bind: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) {
        printf("  server: listen: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    /* Assert the procfs view now, while the listener exists.  A gate that
     * checked it after the test exited would see an empty file. */
    if (check_proc_net() != 0) {
        close(fd);
        return -1;
    }
    if (arm_timeout(fd) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_storage peer;
    socklen_t peerlen = sizeof(peer);
    memset(&peer, 0, sizeof(peer));
    int c = accept(fd, (struct sockaddr *)&peer, &peerlen);
    close(fd);
    if (c < 0) {
        printf("  server: accept: %s\n", strerror(errno));
        return -1;
    }

    /* The accepted peer address must be a v6 sockaddr.  Before the fix the
     * accept drain set child->domain = AF_INET regardless of the listener, so
     * this reported AF_INET and the address read back as 16 bytes of v4
     * layout.  Checking the family is what turns that into a gate failure
     * rather than a silent mis-decode later. */
    if (peer.ss_family != AF_INET6) {
        printf("  server: peer family is %d, expected AF_INET6 (%d)\n",
               (int)peer.ss_family, AF_INET6);
        close(c);
        return -1;
    }
    {
        const struct sockaddr_in6 *p = (const struct sockaddr_in6 *)&peer;
        unsigned char want[16] = { 0 };
        want[15] = 1;
        if (memcmp(p->sin6_addr.s6_addr, want, 16) != 0) {
            printf("  server: peer address is not ::1\n");
            close(c);
            return -1;
        }
        if (ntohs(p->sin6_port) == 0) {
            printf("  server: peer port is 0\n");
            close(c);
            return -1;
        }
    }

    if (arm_timeout(c) < 0) {
        close(c);
        return -1;
    }

    /* Direction 1: client -> server. */
    static unsigned char rx[PAYLOAD_LEN];
    unsigned char want[PAYLOAD_LEN];
    fill_payload(want, sizeof(want));
    size_t got = 0;
    while (got < sizeof(want)) {
        ssize_t n = read(c, rx + got, sizeof(want) - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            printf("  server: read at %zu: %s\n", got, strerror(errno));
            close(c);
            return -1;
        }
        if (n == 0) {
            printf("  server: peer closed after %zu of %zu bytes\n",
                   got, sizeof(want));
            close(c);
            return -1;
        }
        got += (size_t)n;
    }
    if (memcmp(rx, want, sizeof(want)) != 0) {
        printf("  server: client->server payload mismatch\n");
        close(c);
        return -1;
    }

    /* Direction 2: server -> client, on the fd accept() returned. */
    static unsigned char tx[PAYLOAD_LEN];
    fill_payload(tx, sizeof(tx));
    size_t sent = 0;
    while (sent < sizeof(tx)) {
        ssize_t n = write(c, tx + sent, sizeof(tx) - sent);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            printf("  server: write at %zu: %s\n", sent, strerror(errno));
            close(c);
            return -1;
        }
        sent += (size_t)n;
    }
    close(c);
    return 0;
}

static int client(void)
{
    struct sockaddr_in6 a6;
    if (v6_addr(&a6, (uint16_t)test_port) < 0)
        return -1;

    for (int waited = 0; waited < CONNECT_BUDGET_US; waited += CONNECT_RETRY_US) {
        int fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        /* Bound the kernel's own connect timeout so one attempt cannot consume
         * the whole budget on its own. */
        arm_timeout(fd);
        if (connect(fd, (struct sockaddr *)&a6, sizeof(a6)) == 0) {
            static unsigned char tx[PAYLOAD_LEN];
            static unsigned char rx[PAYLOAD_LEN];
            fill_payload(tx, sizeof(tx));
            if (arm_timeout(fd) < 0) {
                close(fd);
                return -1;
            }
            size_t sent = 0;
            while (sent < sizeof(tx)) {
                ssize_t n = write(fd, tx + sent, sizeof(tx) - sent);
                if (n < 0) {
                    if (errno == EINTR)
                        continue;
                    printf("  client: write at %zu: %s\n", sent,
                           strerror(errno));
                    close(fd);
                    return -1;
                }
                sent += (size_t)n;
            }
            size_t got = 0;
            while (got < sizeof(rx)) {
                ssize_t n = read(fd, rx + got, sizeof(rx) - got);
                if (n < 0) {
                    if (errno == EINTR)
                        continue;
                    printf("  client: read at %zu: %s\n", got, strerror(errno));
                    close(fd);
                    return -1;
                }
                if (n == 0) {
                    printf("  client: server closed after %zu of %zu bytes\n",
                           got, sizeof(rx));
                    close(fd);
                    return -1;
                }
                got += (size_t)n;
            }
            close(fd);
            if (memcmp(rx, tx, sizeof(rx)) != 0) {
                printf("  client: server->client payload mismatch\n");
                return -1;
            }
            return 0;
        }
        phase = "connect";
        close(fd);
        usleep(CONNECT_RETRY_US);
    }
    return -1;
}

int main(int argc, char **argv)
{
    test_port = (argc > 1) ? atoi(argv[1]) : TEST_PORT_DEFAULT;
    if (test_port <= 0 || test_port > 65535) {
        printf("IPV6_LOOPBACK_TEST: FAIL (bad port %d)\n", test_port);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("IPV6_LOOPBACK_TEST: FAIL (fork)\n");
        return 1;
    }
    if (pid == 0) {
        int r = server();
        _exit(r < 0 ? 1 : 0);
    }

    int r = client();
    int status = 0;
    /* Bounded so a server that failed before accept() cannot wedge the gate on
     * waitpid; the client budget above already bounds the interesting case. */
    for (int i = 0; i < 400; i++) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid)
            break;
        usleep(25000);
    }

    if (r == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("IPV6_LOOPBACK_TEST: PASS port=%d (%d B each way over ::1, "
               "peer reported as AF_INET6)\n", test_port, PAYLOAD_LEN);
        return 0;
    }
    printf("IPV6_LOOPBACK_TEST: FAIL port=%d (client_failed_at=%s "
           "client=%d server_status=%d)\n",
           test_port, phase, r, status);
    return 1;
}