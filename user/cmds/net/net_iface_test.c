/*
 * SIOCGIFCONF / SIOCGIF* runtime gate.
 *
 * getifaddrs(), ifconfig and busybox `ip` are all built on SIOCGIFCONF, and the
 * per-interface getters (SIOCGIFADDR, SIOCGIFFLAGS, ...) are unreachable
 * without a way to discover which interfaces exist.  This exercises that whole
 * path the way a caller actually uses it: size query, fill, then feed a
 * discovered name back into the per-interface getter.
 *
 * Wire format, per the kernel side (kernel/net/socket_file.c):
 *   struct ifconf { int ifc_len; void *ifc_buf; }
 *   struct ifreq  { char ifr_name[16]; union { ... } }   -- 32 bytes total
 * User space walks the buffer in whole-entry strides, so the layout has to match
 * Linux exactly; the kernel pins that with a _Static_assert of its own.
 */
#include <errno.h>
#include <stddef.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define TEST_NAME "NET_IFACE"

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond) {
        printf("%s: ok   %s\n", TEST_NAME, what);
    } else {
        failures++;
        printf("%s: FAIL %s (errno=%d %s)\n", TEST_NAME, what, errno, strerror(errno));
    }
}

static size_t query_size(int fd)
{
    struct ifconf ifc;
    memset(&ifc, 0, sizeof(ifc));
    ifc.ifc_buf = NULL;
    if (ioctl(fd, SIOCGIFCONF, &ifc) < 0)
        return 0;
    return (size_t)ifc.ifc_len;
}

static void test_size_query(int fd)
{
    size_t need = query_size(fd);
    /* An exact multiple of the entry size: callers size their second pass from
     * this number, so an estimate here makes them loop reallocating. */
    ok(need % sizeof(struct ifreq) == 0, "size query is a whole number of entries");
    ok(need > 0, "at least one interface is configured");
    printf("%s: info %zu bytes = %zu entries; user sizeof(ifreq)=%zu "
           "offsetof(ifr_ifru)=%zu IFNAMSIZ=%d\n", TEST_NAME,
           need, need / sizeof(struct ifreq), sizeof(struct ifreq),
           offsetof(struct ifreq, ifr_ifru), IFNAMSIZ);
}

static void test_fill(int fd, char *first_name)
{
    size_t need = query_size(fd);
    char *buf = malloc(need);
    struct ifconf ifc;

    if (!buf) {
        ok(0, "allocate enumeration buffer");
        return;
    }
    memset(&ifc, 0, sizeof(ifc));
    ifc.ifc_len = (int)need;
    ifc.ifc_buf = buf;
    ok(ioctl(fd, SIOCGIFCONF, &ifc) == 0, "fill the enumeration buffer");
    ok((size_t)ifc.ifc_len == need, "reported byte count matches the size query");

    size_t entries = (size_t)ifc.ifc_len / sizeof(struct ifreq);
    struct ifreq *rows = (struct ifreq *)buf;
    int terminated = 1, family_ok = 1;

    for (size_t i = 0; i < entries; i++) {
        if (memchr(rows[i].ifr_name, '\0', IFNAMSIZ) == NULL)
            terminated = 0;
        /* Read the row the way a real caller does: as a `struct sockaddr_in`,
         * so the compiler applies network byte order to sin_family. Reading the
         * two family bytes by hand is how this check first "passed" a kernel
         * that was writing them little-endian. */
        const struct sockaddr_in *sin = (const struct sockaddr_in *)&rows[i].ifr_ifru;
        if (sin->sin_family != AF_INET)
            family_ok = 0;
        if (i == 0)
            snprintf(first_name, IFNAMSIZ, "%s", rows[i].ifr_name);
    }
    for (size_t i = 0; i < entries; i++) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)&rows[i].ifr_ifru;
        printf("%s: info %-4s %d.%d.%d.%d\n", TEST_NAME, rows[i].ifr_name,
               (unsigned char)sin->sin_addr.s_addr,
               (unsigned char)(sin->sin_addr.s_addr >> 8),
               (unsigned char)(sin->sin_addr.s_addr >> 16),
               (unsigned char)(sin->sin_addr.s_addr >> 24));
    }
    ok(terminated, "every interface name is NUL-terminated");
    ok(family_ok, "every row is an AF_INET sockaddr");
    printf("%s: info first interface is \"%s\"\n", TEST_NAME, first_name);
    free(buf);
}

static void test_truncation(int fd)
{
    struct ifreq row;
    char small[sizeof(struct ifreq)];
    struct ifconf ifc;

    /* A buffer too small for even one row must report zero, not a partial
     * entry that user space would then walk off the end of. */
    memset(&ifc, 0, sizeof(ifc));
    ifc.ifc_len = (int)sizeof(row) - 1;
    ifc.ifc_buf = small;
    memset(small, 0xAA, sizeof(small));
    ok(ioctl(fd, SIOCGIFCONF, &ifc) == 0, "sub-entry buffer is accepted");
    ok(ifc.ifc_len == 0, "sub-entry buffer yields zero entries");
    ok((unsigned char)small[0] == 0xAA, "sub-entry buffer was not written to");

    /* Exactly one row must fit, and stop there. */
    memset(&ifc, 0, sizeof(ifc));
    ifc.ifc_len = (int)sizeof(row);
    ifc.ifc_buf = (char *)&row;
    ok(ioctl(fd, SIOCGIFCONF, &ifc) == 0, "single-row buffer is accepted");
    ok(ifc.ifc_len <= (int)sizeof(row), "single-row buffer does not overflow");
    ok(ifc.ifc_len % (int)sizeof(row) == 0, "truncated result is still whole entries");
}

static void test_family_filter(int fd)
{
    size_t need = query_size(fd);
    char *buf = malloc(need);
    struct ifconf ifc;
    struct ifreq *rows;

    if (!buf) {
        ok(0, "allocate buffer for family filter");
        return;
    }
    /* Linux reads the wanted family from the first entry of a non-empty
     * buffer. Only IPv4 is implemented, so asking for anything else must come
     * back empty rather than IPv4 rows wearing the wrong family tag. */
    rows = (struct ifreq *)buf;
    memset(rows, 0, sizeof(*rows));
    {
        unsigned char *raw = (unsigned char *)&rows->ifr_ifru;
        raw[0] = (unsigned char)(AF_PACKET & 0xFF);
        raw[1] = (unsigned char)((AF_PACKET >> 8) & 0xFF);
    }

    memset(&ifc, 0, sizeof(ifc));
    ifc.ifc_len = (int)need;
    ifc.ifc_buf = buf;
    ok(ioctl(fd, SIOCGIFCONF, &ifc) == 0, "non-IPv4 family request is accepted");
    ok(ifc.ifc_len == 0, "non-IPv4 family request yields an empty list");
    free(buf);
}

static void test_negative_length(int fd)
{
    struct ifconf ifc;
    memset(&ifc, 0, sizeof(ifc));
    ifc.ifc_len = -1;
    ifc.ifc_buf = NULL;
    errno = 0;
    ok(ioctl(fd, SIOCGIFCONF, &ifc) < 0, "negative ifc_len is rejected");
    ok(errno == EINVAL, "negative ifc_len fails with EINVAL");
}

static void test_round_trip(int fd, const char *name)
{
    struct ifreq ifr;
    struct sockaddr_in *addr;

    /* The point of enumeration: a name discovered above must be usable with the
     * per-interface getter that was previously unreachable. */
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    ok(ioctl(fd, SIOCGIFADDR, &ifr) == 0, "SIOCGIFADDR accepts an enumerated name");

    addr = (struct sockaddr_in *)&ifr.ifr_ifru;
    printf("%s: info %s address %d.%d.%d.%d\n", TEST_NAME, name,
           (unsigned char)addr->sin_addr.s_addr & 0xFF,
           (unsigned char)(addr->sin_addr.s_addr >> 8) & 0xFF,
           (unsigned char)(addr->sin_addr.s_addr >> 16) & 0xFF,
           (unsigned char)(addr->sin_addr.s_addr >> 24) & 0xFF);
    ok(addr->sin_family == AF_INET, "SIOCGIFADDR returns an AF_INET address");

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
    ok(ioctl(fd, SIOCGIFFLAGS, &ifr) == 0, "SIOCGIFFLAGS accepts an enumerated name");

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "nosuchif0");
    errno = 0;
    ok(ioctl(fd, SIOCGIFADDR, &ifr) < 0, "unknown interface name is rejected");
}

int main(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    char first_name[IFNAMSIZ] = { 0 };

    if (fd < 0) {
        printf("%s: FAIL socket: %s\n", TEST_NAME, strerror(errno));
        return 1;
    }

    test_size_query(fd);
    test_fill(fd, first_name);
    test_truncation(fd);
    test_family_filter(fd);
    test_negative_length(fd);
    if (first_name[0])
        test_round_trip(fd, first_name);

    close(fd);

    if (failures == 0) {
        printf("%s: PASS (%d checks)\n", TEST_NAME, checks);
        return 0;
    }
    printf("%s: FAIL (%d of %d checks failed)\n", TEST_NAME, failures, checks);
    return 1;
}
