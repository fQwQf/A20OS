/*
 * netstat — socket, route and interface reporting over the /proc/net
 * projections the kernel maintains.
 *
 * The kernel writes /proc/net/{tcp,udp,unix} in the Linux column layout, with
 * the address as the raw network-order word and the port in host order, so a
 * row decodes back to exactly the endpoint the socket holds.  Fields the
 * kernel reports as absent (see the notes in kernel/fs/procfs/procfs_render.c)
 * print as 0 here rather than being invented: a uid of 0 means "unrecorded",
 * not root, and there is no socket inode to show.
 *
 * /proc/net/route and /proc/net/dev are tab-separated and, for route, carry
 * only the default gateway -- this stack has no forwarding table, so there is
 * nothing else to display and nothing is synthesised to fill the gap.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINE 512

/* Indexed by the state number itself, so index 0 is an unused placeholder:
 * Linux numbers TCP states from 1, and LISTEN is 0x0A. */
static const char *const tcp_states[] = {
    "-",           "ESTABLISHED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1",
    "FIN_WAIT2",   "TIME_WAIT",   "CLOSE",    "CLOSE_WAIT", "LAST_ACK",
    "LISTEN",      "CLOSING",     "NEW_SYN_RECV",
};

static const char *const unix_states[] = {
    "-",           "CONNECTED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1",
    "FIN_WAIT2",   "UNCONNECTED", "UNCONNECTED", "CLOSE_WAIT", "LAST_ACK",
    "LISTENING",   "CLOSING",   "NEW_SYN_RECV",
};

static const char *state_name(const char *const *table, unsigned st)
{
    return st < sizeof(tcp_states) / sizeof(tcp_states[0]) ? table[st]
                                                            : "UNKNOWN";
}

/* /proc/net/{tcp,udp,unix} prints an address as the raw network-order 32-bit
 * word, so the first octet is the least significant byte: 0100007F is
 * 127.0.0.1.  /proc/net/route is the other way round -- Linux formats those
 * fields with ntohl, so the same hex reads big-endian.  Getting this backwards
 * turns 10.0.2.2 into 2.2.0.10, so the two need separate decoders. */
static void format_words(char *out, size_t outsz, const char *hex,
                         size_t hexlen, int big_endian)
{
    unsigned char b[16];
    size_t words = hexlen / 8;

    if (words == 0 || words > 4) {
        snprintf(out, outsz, "*");
        return;
    }
    for (size_t w = 0; w < words; w++) {
        char word[9] = {0};
        memcpy(word, hex + w * 8, 8);
        uint32_t v = (uint32_t)strtoul(word, NULL, 16);
        if (big_endian) {
            b[w * 4 + 0] = (unsigned char)(v >> 24);
            b[w * 4 + 1] = (unsigned char)(v >> 16);
            b[w * 4 + 2] = (unsigned char)(v >> 8);
            b[w * 4 + 3] = (unsigned char)v;
        } else {
            b[w * 4 + 0] = (unsigned char)v;
            b[w * 4 + 1] = (unsigned char)(v >> 8);
            b[w * 4 + 2] = (unsigned char)(v >> 16);
            b[w * 4 + 3] = (unsigned char)(v >> 24);
        }
    }
    if (words == 1) {
        snprintf(out, outsz, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return;
    }
    if (words == 4 && b[10] == 0xff && b[11] == 0xff) {
        snprintf(out, outsz, "::ffff:%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
        return;
    }
    snprintf(out, outsz, "%x:%x:%x:%x:%x:%x:%x:%x",
             (unsigned)(b[0] << 8 | b[1]), (unsigned)(b[2] << 8 | b[3]),
             (unsigned)(b[4] << 8 | b[5]), (unsigned)(b[6] << 8 | b[7]),
             (unsigned)(b[8] << 8 | b[9]), (unsigned)(b[10] << 8 | b[11]),
             (unsigned)(b[12] << 8 | b[13]), (unsigned)(b[14] << 8 | b[15]));
}

static void format_addr(char *out, size_t outsz, const char *hex, size_t hexlen)
{
    format_words(out, outsz, hex, hexlen, 0);
}

static void format_addr_be(char *out, size_t outsz, const char *hex,
                           size_t hexlen)
{
    format_words(out, outsz, hex, hexlen, 1);
}

static void format_endpoint(char *out, size_t outsz, const char *hex, size_t hexlen,
                            const char *porthex)
{
    char addr[64];

    format_addr(addr, sizeof(addr), hex, hexlen);
    snprintf(out, outsz, "%s:%u", addr,
             (unsigned)strtoul(porthex, NULL, 16));
}

static int state_selected(unsigned st, int listening, int all)
{
    if (listening)
        return st == 0x0a;
    if (all)
        return 1;
    return st == 0x0a || st == 0x01;
}

static void show_inet(const char *path, const char *proto, int listening, int all)
{
    FILE *f = fopen(path, "r");
    char line[MAX_LINE];
    int header_skipped = 0;

    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        unsigned sl, st, uid, tx, rx, timer, retrnsmt, discard;
        unsigned long timeout;
        int inode;
        char lhex[40], rhex[40], lp[8], rp[8];

        /* A data row is "%4d: ..." and so also starts with a space, which is
         * exactly what the column header starts with.  The header can only be
         * identified by position, not by its first character. */
        if (!header_skipped) {
            header_skipped = 1;
            continue;
        }
        if (sscanf(line, "%u: %39[0-9A-Fa-f]:%7[0-9A-Fa-f] "
                        "%39[0-9A-Fa-f]:%7[0-9A-Fa-f] %x %x:%x %x:%x %x "
                        "%u %lu %d",
                   &sl, lhex, lp, rhex, rp, &st, &tx, &rx,
                   &timer, &discard, &retrnsmt, &uid, &timeout, &inode) != 14)
            continue;
        if (!state_selected(st, listening, all))
            continue;
        char local[80], remote[80];
        format_endpoint(local, sizeof(local), lhex, strlen(lhex), lp);
        format_endpoint(remote, sizeof(remote), rhex, strlen(rhex), rp);
        printf("%-4s %7u %7u %-24s %-23s %-10s\n", proto, rx, tx, local,
               remote, state_name(tcp_states, st));
    }
    fclose(f);
}

static void show_unix(int listening, int all)
{
    FILE *f = fopen("/proc/net/unix", "r");
    char line[MAX_LINE];

    if (!f)
        return;
    printf("Active UNIX domain sockets (%s)\n",
           listening ? "servers only" : "servers and established");
    printf("Proto RefCnt Flags   Type   State      I-Node   Path\n");
    while (fgets(line, sizeof(line), f)) {
        unsigned num_hi, num_lo, refcnt, proto, flags, type, st;
        int inode, consumed = 0;
        char path[256];

        path[0] = '\0';
        if (line[0] == 'N' || line[0] == '\n')
            continue;
        if (sscanf(line, "%x%x: %x %x %x %x %x %d %n",
                   &num_hi, &num_lo, &refcnt, &proto, &flags, &type, &st,
                   &inode, &consumed) < 7)
            continue;
        if (consumed > 0)
            snprintf(path, sizeof(path), "%s", line + consumed);
        if (!state_selected(st, listening, all))
            continue;
        const char *ts = type == 1 ? "STREAM" : type == 2 ? "DGRAM" : "RAW";
        char *nl = strchr(path, '\n');
        if (nl)
            *nl = '\0';
        printf("unix  %-6u [ %-5s] %-6s %-10s %-8d %s\n", refcnt,
               (flags & 0x10000) ? "ACC" : "   ", ts,
               state_name(unix_states, st), inode, path);
    }
    fclose(f);
    printf("\n");
}

static void show_route(void)
{
    FILE *f = fopen("/proc/net/route", "r");
    char line[MAX_LINE];

    if (!f)
        return;
    printf("Kernel IP routing table\n");
    printf("Destination     Gateway         Genmask         Flags   MSS "
           "Window  irtt Iface\n");
    while (fgets(line, sizeof(line), f)) {
        char iface[32], dest[16], gw[16], mask[16], flags[16];
        unsigned fl;

        if (line[0] == 'I' || line[0] == '\n')
            continue;
        if (sscanf(line, "%31[^\t]\t%15[^\t]\t%15[^\t]\t%15[^\t]",
                   iface, dest, gw, mask) != 4)
            continue;
        if (sscanf(flags, "%x", &fl) != 1)
            fl = 0;
        char d[32], g[32], m[32];
        format_addr_be(d, sizeof(d), dest, strlen(dest));
        format_addr_be(g, sizeof(g), gw, strlen(gw));
        format_addr_be(m, sizeof(m), mask, strlen(mask));
        printf("%-15s %-15s %-15s %-7s %5u %6u %5u %s\n", d, g, m,
               (fl & 0x2) ? "UG" : "U", 0u, 0u, 0u, iface);
    }
    fclose(f);
    printf("\n");
}

static void show_interfaces(void)
{
    FILE *f = fopen("/proc/net/dev", "r");
    char line[MAX_LINE];
    int header = 2;

    if (!f)
        return;
    printf("Kernel Interface table\n");
    printf("%-8s %-10s %-12s %-12s %-12s %-12s\n", "Iface", "MTU", "RX-OK",
           "RX-ERR", "TX-OK", "TX-ERR");
    while (fgets(line, sizeof(line), f)) {
        char name[32];
        unsigned long long v[16];
        int n;

        if (header > 0) {
            header--;
            continue;
        }
        n = sscanf(line, "%31[^:]: %llu %llu %llu %llu %llu %llu %llu %llu "
                         "%llu %llu %llu %llu %llu %llu %llu %llu",
                   name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                   &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13],
                   &v[14], &v[15]);
        if (n < 4)
            continue;
        char *nl = strchr(name, ' ');
        if (nl)
            *nl = '\0';
        printf("%-8s %-10s %-12llu %-12llu %-12llu %-12llu\n", name, "-",
               v[1], v[2], v[9], v[10]);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    int all = 0, listening = 0, route = 0, iface = 0, inet = 0, unix_ = 0;
    int opt;

    while ((opt = getopt(argc, argv, "alrinTux")) != -1) {
        switch (opt) {
        case 'a': all = 1; break;
        case 'l': listening = 1; break;
        case 'r': route = 1; break;
        case 'i': iface = 1; break;
        case 'n': break;  /* names are never resolved, so this is the default */
        case 'T': break;  /* TCP only: implied by the absence of -u/-x */
        case 't': inet = 1; break;
        case 'u': inet = 1; break;
        case 'x': unix_ = 1; break;
        default:
            fprintf(stderr,
                    "usage: netstat [-a] [-l] [-r] [-i] [-t] [-u] [-x]\n");
            return 1;
        }
    }
    if (!route && !iface) {
        printf("Active Internet connections (%s)\n",
               listening ? "servers only" : "servers and established");
        printf("Proto Recv-Q Send-Q  Local Address           "
               "Foreign Address         State      \n");
        show_inet("/proc/net/tcp", "tcp", listening, all);
        show_inet("/proc/net/udp", "udp", listening, all);
        printf("\n");
    }
    if (!route && !iface && (all || unix_ || !inet))
        show_unix(listening, all);
    if (route)
        show_route();
    if (iface)
        show_interfaces();
    return 0;
}
