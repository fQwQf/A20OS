/*
 * SNAT end-to-end: a guest-initiated TCP flow is translated on the output hook
 * and its replies return.  This is the counterpart to netnat_test's inbound
 * DNAT gate; together they cover both NAT directions on a real virtio netif.
 *
 * The peer is the host alias 10.0.2.2, where tools/probes/netnat_snat_probe.py listens.
 * QEMU's user-mode network performs its own address translation outside the
 * guest, so the probe cannot observe the guest's rewritten source port.  What
 * this gate does establish is that the guest output hook rewrote the tuple,
 * conntrack recorded the binding, and the reply arrived on the translated flow.
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define PEER_IP "10.0.2.2"
#define PEER_PORT 18083
#define SNAT_PORT 18084
#define TRANSACTIONS 16
#define NF_PATH "/proc/a20/netfilter"

#define CHK(cond, msg)                                                        \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("NETNAT_SNAT_TEST: FAIL %s:%d %s (errno=%d %s)\n",         \
                   __func__, __LINE__, (msg), errno, strerror(errno));        \
            return 1;                                                         \
        }                                                                     \
    } while (0)

static int nf_read(char *buf, size_t bufsz) {
    int fd = open(NF_PATH, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return (int)n;
}

static int nf_write(const char *s) {
    int fd = open(NF_PATH, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, s, strlen(s));
    int e = (n == (ssize_t)strlen(s)) ? 0 : -1;
    close(fd);
    return e;
}

/* One connection, N messages.  A fixed SNAT port cannot distinguish two
 * concurrent flows to the same peer, so the workload must not open a new
 * connection per message: the second SYN would collide with the first flow's
 * return tuple. */
static int run_connection(int count) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons(PEER_PORT);
    peer.sin_addr.s_addr = inet_addr(PEER_IP);
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) != 0) {
        close(fd);
        return -1;
    }
    for (int i = 0; i < count; i++) {
        char c = 'S', back = 0;
        if (write(fd, &c, 1) != 1 || read(fd, &back, 1) != 1 || back != c) {
            close(fd);
            return -1;
        }
    }
    close(fd);
    return 0;
}

int main(void) {
    char buf[8192];

    CHK(nf_write("reset") == 0, "reset filter and NAT tables");

    /* The wrong-hook forms are refused, so a rule that cannot fire is never
     * reported as installed. */
    CHK(nf_write("natadd in proto=tcp dport=1 action=snat to=10.0.2.15 toport=99") < 0,
        "snat on the input hook is rejected");
    CHK(nf_write("natadd in proto=tcp dport=1 action=masquerade") < 0,
        "masquerade on the input hook is rejected");

    char rule[128];
    snprintf(rule, sizeof(rule),
             "natadd out proto=tcp dport=%d action=snat to=10.0.2.15 toport=%d",
             PEER_PORT, SNAT_PORT);
    CHK(nf_write(rule) == 0, "install the SNAT rule");

    CHK(run_connection(TRANSACTIONS) == 0,
        "complete the SNAT-translated messages");

    CHK(nf_read(buf, sizeof(buf)) > 0, "read the conntrack table");
    CHK(strstr(buf, "action=snat") != NULL,
        "the conntrack entry records the SNAT binding");
    CHK(strstr(buf, "state=established") != NULL ||
            strstr(buf, "state=new") != NULL,
        "the translated flow has a conntrack state");

    /* Negative control: with the rule gone the same flow must not acquire an
     * SNAT binding.  The connection still succeeds -- the host peer is
     * reachable without translation -- so a passing echo here cannot be
     * mistaken for the rule still being in force. */
    CHK(nf_write("natdel 0") == 0, "delete the SNAT rule");
    CHK(nf_write("ctflush") == 0, "flush conntrack");
    CHK(run_connection(1) == 0, "the untranslated flow still echoes");
    CHK(nf_read(buf, sizeof(buf)) > 0, "read conntrack after the control");
    CHK(strstr(buf, "action=snat") == NULL,
        "without the rule the flow has no SNAT binding");

    printf("NETNAT_SNAT_TEST: PASS translated=%d control=%d snat_port=%d\n",
           TRANSACTIONS, 1, SNAT_PORT);
    return 0;
}
