/* Assert that a loopback TCP connection can be established and accepted.
 *
 * This deliberately checks only the handshake and the accept, not the data
 * transfer.  The two are separable: a connection can be accepted and then fail
 * to move bytes, and conflating them would make this test red for reasons that
 * have nothing to do with the accept path.
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
#define CONNECT_BUDGET_MS 4000
#define CONNECT_RETRY_US 20000

static int arm_timeout(int fd)
{
    struct timeval tv;
    tv.tv_sec = IO_TIMEOUT_SEC;
    tv.tv_usec = 0;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int test_port;

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
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(test_port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    if (arm_timeout(fd) < 0) {
        close(fd);
        return -1;
    }

    /* accept() honours SO_RCVTIMEO, so a handshake that never completes fails
     * here with EAGAIN instead of parking the gate forever. */
    int c = accept(fd, NULL, NULL);
    close(fd);
    if (c < 0)
        return -1;
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

    for (int waited = 0; waited < CONNECT_BUDGET_MS; waited += CONNECT_RETRY_US) {
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
