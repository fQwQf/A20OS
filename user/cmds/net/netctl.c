/*
 * netctl — regression coverage for the user-space network control surface:
 * SIOCGIFCONF interface enumeration, the /proc/net/{dev,route} projections
 * that depend on it, and MSG_OOB rejection.
 *
 * The point of using musl's own <net/if.h> structs rather than private copies
 * is that the kernel then has to agree with the layout user space actually
 * compiles against -- that agreement is the compatibility contract, and a
 * self-consistent private pair would test nothing.
 *
 * Prints "NETCTL: PASS" on success.  The first deviation prints
 * "NETCTL: FAIL <what>" and returns 1.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#ifndef SIOCGIFCONF
#define SIOCGIFCONF 0x8912
#endif
#ifndef MSG_OOB
#define MSG_OOB 0x0001
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x4000
#endif

static int fails;

static void fail(const char *what, long got)
{
    printf("NETCTL: FAIL %s (ret=%ld errno=%d)\n", what, got, errno);
    fails++;
}

/* Read one dotted-quad key from /proc/net/config.  Returns 0 on success. */
static int config_addr(const char *key, struct in_addr *out)
{
    char path[64];
    snprintf(path, sizeof(path), "%s=", key);
    FILE *f = fopen("/proc/net/config", "r");
    if (!f)
        return -1;
    char line[256];
    int rc = -1;
    size_t klen = strlen(path);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, path, klen) != 0)
            continue;
        if (inet_pton(AF_INET, line + klen, out) == 1)
            rc = 0;
        break;
    }
    fclose(f);
    return rc;
}

/* SIOCGIFCONF must report a size, then fill exactly that many whole entries,
 * and every name it hands back must be resolvable by the per-interface
 * getters -- otherwise enumeration is useless to the tools that use it. */
static int test_gifconf(int fd, int *count_out)
{
    struct ifconf q;
    memset(&q, 0, sizeof(q));
    if (ioctl(fd, SIOCGIFCONF, &q) < 0) {
        fail("SIOCGIFCONF size query", -1);
        return -1;
    }
    if (q.ifc_len <= 0 || (q.ifc_len % (int)sizeof(struct ifreq)) != 0) {
        fail("SIOCGIFCONF size is a positive multiple of sizeof(ifreq)", q.ifc_len);
        return -1;
    }

    int want = q.ifc_len;
    char *buf = calloc(1, (size_t)want);
    if (!buf) {
        fail("calloc for ifconf buffer", -1);
        return -1;
    }
    struct ifconf c;
    memset(&c, 0, sizeof(c));
    c.ifc_len = want;
    c.ifc_buf = buf;
    if (ioctl(fd, SIOCGIFCONF, &c) < 0) {
        fail("SIOCGIFCONF fill", -1);
        free(buf);
        return -1;
    }
    if (c.ifc_len != want) {
        fail("SIOCGIFCONF reported fewer bytes than the size query promised", c.ifc_len);
        free(buf);
        return -1;
    }

    int n = want / (int)sizeof(struct ifreq);
    for (int i = 0; i < n; i++) {
        struct ifreq *ifr = &((struct ifreq *)buf)[i];
        if (ifr->ifr_name[0] == '\0') {
            fail("SIOCGIFCONF returned an empty interface name", i);
            free(buf);
            return -1;
        }
        struct ifreq probe;
        memset(&probe, 0, sizeof(probe));
        snprintf(probe.ifr_name, sizeof(probe.ifr_name), "%s", ifr->ifr_name);
        if (ioctl(fd, SIOCGIFADDR, &probe) < 0) {
            printf("NETCTL: FAIL enumerated name '%s' is not resolvable by "
                   "SIOCGIFADDR (errno=%d)\n", ifr->ifr_name, errno);
            fails++;
            free(buf);
            return -1;
        }
    }
    free(buf);
    *count_out = n;
    return 0;
}

/* /proc/net/dev must project the same interfaces SIOCGIFCONF enumerates. */
static void test_proc_net_dev(int expected)
{
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) {
        fail("open /proc/net/dev", -1);
        return;
    }
    char line[512];
    int rows = 0;
    while (fgets(line, sizeof(line), f))
        if (strchr(line, ':'))
            rows++;
    fclose(f);
    if (rows < expected) {
        printf("NETCTL: FAIL /proc/net/dev has %d rows but SIOCGIFCONF "
               "enumerated %d\n", rows, expected);
        fails++;
    }
}

/* A configured gateway must actually appear in /proc/net/route; an empty
 * table under a configured gateway is the exact failure that made in-guest
 * network state unobservable. */
static void test_proc_net_route(void)
{
    struct in_addr gw;
    int have_gw = (config_addr("gateway", &gw) == 0) &&
                  (ntohl(gw.s_addr) != 0);
    if (!have_gw)
        return;

    FILE *f = fopen("/proc/net/route", "r");
    if (!f) {
        fail("open /proc/net/route", -1);
        return;
    }
    char line[512];
    int rows = 0;
    int header = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!header) { header = 1; continue; }
        if (line[0] == '\n' || line[0] == '\0')
            continue;
        /* Destination is field 2, gateway field 3; both non-zero means a
         * real default route row. */
        char *save = NULL;
        char *tok = strtok_r(line, " \t\n", &save);
        int field = 0, nonzero_gw = 0;
        while (tok) {
            field++;
            if (field == 3 && strtoul(tok, NULL, 16) != 0)
                nonzero_gw = 1;
            tok = strtok_r(NULL, " \t\n", &save);
        }
        if (nonzero_gw)
            rows++;
    }
    fclose(f);
    if (rows == 0) {
        printf("NETCTL: FAIL /proc/net/config reports a gateway but "
               "/proc/net/route has no route row\n");
        fails++;
    }
}

/* There is no out-of-band data path, so MSG_OOB must be refused rather than
 * silently satisfied with ordinary in-band bytes. */
static void test_msg_oob(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fail("socket for MSG_OOB test", -1);
        return;
    }
    char c = 0;
    errno = 0;
    long r = recv(fd, &c, 1, MSG_OOB | MSG_DONTWAIT);
    if (r >= 0 || errno != EOPNOTSUPP) {
        fail("recv(MSG_OOB) should be EOPNOTSUPP", r);
        close(fd);
        return;
    }
    errno = 0;
    r = send(fd, &c, 1, MSG_OOB | MSG_DONTWAIT);
    if (r >= 0 || errno != EOPNOTSUPP) {
        fail("send(MSG_OOB) should be EOPNOTSUPP", r);
        close(fd);
        return;
    }
    close(fd);
}

/* MSG_NOSIGNAL is accepted.  This stack never raises SIGPIPE on a socket, so
 * a dead-peer write reports EPIPE whether or not the flag is passed; the flag
 * must at least not be rejected as unknown. */
static void test_msg_nosignal(int fd)
{
    char c = 'x';
    errno = 0;
    long r = send(fd, &c, 1, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (r >= 0)
        return;                      /* datagram left; fine */
    if (errno == EOPNOTSUPP) {
        fail("send(MSG_NOSIGNAL) rejected as unsupported", r);
    }
}

int main(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("NETCTL: FAIL socket(AF_INET) failed errno=%d\n", errno);
        return 1;
    }

    int count = 0;
    if (test_gifconf(fd, &count) == 0) {
        test_proc_net_dev(count);
        test_msg_nosignal(fd);
    }
    test_proc_net_route();
    test_msg_oob();
    close(fd);

    if (fails) {
        printf("NETCTL: FAIL (%d failing checks)\n", fails);
        return 1;
    }
    printf("NETCTL: PASS (%d interfaces enumerated)\n", count);
    return 0;
}
