/* Assert that a TCP connection can be established and accepted.
 *
 * Two modes, chosen by whether a bind address is given:
 *
 *   tcp_accept_test [port]
 *       The loopback self-connect.  The parent connects to 127.0.0.1 on the
 *       forked child's listener.  This deliberately checks only the handshake
 *       and the accept, not the data transfer: the two are separable, and
 *       conflating them would make this test red for reasons that have nothing
 *       to do with the accept path.  Nothing crosses a NIC here.
 *
 *   tcp_accept_test <port> <bind-address>
 *       Serve mode, for a NIC gate.  The peer is the host, arriving through
 *       QEMU's hostfwd, which delivers to the guest's own address -- not to
 *       loopback -- so the listener has to be bound there.  No in-guest client
 *       is forked: a loopback client would connect to a listener that never
 *       saw the wire and report a pass for a connection that crossed nothing.
 *       One byte is read and echoed back, because the host probe's assertion is
 *       that the byte comes back; a handshake alone would not show the data path
 *       moved payload.
 *
 * The test is mode-agnostic on purpose.  Under "tcpmode fast" the listener is
 * matched by the socket layer pairing the two sockets; under "tcpmode lwip"
 * the listener is a real lwIP LISTEN pcb and the connection is completed by
 * the protocol stack.  Both must reach accept() with a usable fd, so running
 * this in both modes is what keeps the two implementations honest about
 * delivering the same observable result.
 *
 * Every blocking step is bracketed by SO_RCVTIMEO.  A hang inside a gate is
 * unacceptable, and connect() in particular has a 10s kernel timeout that
 * exceeds any sane gate budget. */

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

/* Overridable so a multi-lane run can place connections on different ports.
 * A lane is derived from (local_ip, local_port), so a single fixed port always
 * lands in one lane and cannot witness that per-lane timers all still fire. */
#define TEST_PORT_DEFAULT 12346
/* Long enough that a loaded 4-core guest does not trip it, short enough that a
 * wedged handshake fails the gate in tens of seconds rather than minutes. */
#define IO_TIMEOUT_SEC 5
/* Retry budget in microseconds.  The unit has to match the step the loop below
 * accumulates: `waited` grows by CONNECT_RETRY_US, so comparing it against a
 * millisecond bound ends the loop after a single attempt.  One attempt races
 * the forked server's bind()+listen(), which is what makes a healthy fast-mode
 * listener look intermittently broken. */
#define CONNECT_BUDGET_US 4000000
#define CONNECT_RETRY_US 20000

static int arm_timeout(int fd)
{
    struct timeval tv;
    tv.tv_sec = IO_TIMEOUT_SEC;
    tv.tv_usec = 0;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int test_port;

/* Non-zero when this run serves an address other than loopback, i.e. it is the
 * guest half of a QEMU hostfwd round trip and must NOT fork the loopback
 * client below.  Set from argv[2]. */
static int serve_only;
/* argv[2] exactly as inet_aton() returned it, which is already in network byte
 * order, so it is assigned to sin_addr unchanged.  htonl() must not be applied
 * on top of it: that reverses the four bytes a second time and yields the
 * reverse-order address no interface holds, so bind() fails with EADDRNOTAVAIL
 * and the host's forwarded SYN is answered with RST -- a failure that looks
 * exactly like a dead data path.  INADDR_LOOPBACK is the one address spelled as
 * a host-order constant, which is why the two branches differ. */
static uint32_t test_bind_addr;

/* Which step of server() failed, so a FAIL names the step rather than reporting
 * every failure as "no connection arrived" -- a bind that never happened says
 * nothing at all about whether the data path works. */
static const char *server_stage = "socket";

static int server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = serve_only ? test_bind_addr : htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(test_port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        server_stage = "bind";
        return -1;
    }
    if (listen(fd, 1) < 0) {
        close(fd);
        server_stage = "listen";
        return -1;
    }
    if (arm_timeout(fd) < 0) {
        close(fd);
        server_stage = "setsockopt(SO_RCVTIMEO)";
        return -1;
    }

    /* accept() honours SO_RCVTIMEO, so a handshake that never completes fails
     * here with EAGAIN instead of parking the gate forever. */
    int c = accept(fd, NULL, NULL);
    close(fd);
    if (c < 0) {
        server_stage = "accept";
        return -1;
    }
    /* The connection crossed the wire, so the same bound the echo below. */
    if (arm_timeout(c) < 0) {
        close(c);
        return -1;
    }
    if (serve_only) {
        /* The host probe connects, sends one byte and requires it back.  Echoing
         * it is what makes the round trip an assertion rather than a handshake:
         * a connection that completes but never moves payload proves nothing
         * about the data path it crossed.  Both directions are bounded by the
         * timeout armed above. */
        char b = 0;
        ssize_t r = read(c, &b, 1);
        if (r != 1) {
            close(c);
            return -1;
        }
        ssize_t w = write(c, &b, 1);
        close(c);
        return w == 1 ? 0 : -1;
    }
    close(c);
    return 0;
}

static int client(void)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(test_port);

    for (int waited = 0; waited < CONNECT_BUDGET_US; waited += CONNECT_RETRY_US) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        /* Bound the kernel's own connect timeout so one attempt cannot consume
         * the whole budget on its own. */
        arm_timeout(fd);
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(fd);
            return 0;
        }
        close(fd);
        usleep(CONNECT_RETRY_US);
    }
    return -1;
}

int main(int argc, char **argv)
{
    test_port = (argc > 1) ? atoi(argv[1]) : TEST_PORT_DEFAULT;
    if (test_port <= 0 || test_port > 65535) {
        printf("TCP_ACCEPT_TEST: FAIL (bad port %d)\n", test_port);
        return 1;
    }

    /* argv[2], when present, is the address to listen on.  A NIC gate passes
     * the guest's own address because that is where QEMU's hostfwd delivers
     * the host's connection; the default loopback case is a self-connect and
     * has no forward behind it. */
    if (argc > 2) {
        struct in_addr parsed;
        if (inet_aton(argv[2], &parsed) != 1) {
            printf("TCP_ACCEPT_TEST: FAIL (bad bind address %s)\n", argv[2]);
            return 1;
        }
        test_bind_addr = parsed.s_addr;
        serve_only = 1;
    }

    /* Serve mode has no in-guest client: the peer is the host, arriving through
     * the port forward, so forking the loopback client here would accept a
     * connection that never crossed the NIC and report a pass for it. */
    if (serve_only) {
        int r = server();
        if (r == 0) {
            printf("TCP_ACCEPT_TEST: PASS port=%d (hostfwd connection accepted and one byte echoed)\n",
                   test_port);
            return 0;
        }
        printf("TCP_ACCEPT_TEST: FAIL port=%d (serve mode: failed at %s, errno=%d)\n",
               test_port, server_stage, errno);
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("TCP_ACCEPT_TEST: FAIL (fork)\n");
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
    for (int i = 0; i < 200; i++) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid)
            break;
        usleep(25000);
    }

    if (r == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("TCP_ACCEPT_TEST: PASS port=%d (handshake completed, accept returned a fd)\n",
               test_port);
        return 0;
    }
    printf("TCP_ACCEPT_TEST: FAIL port=%d (client=%d server_status=%d)\n",
           test_port, r, status);
    return 1;
}
