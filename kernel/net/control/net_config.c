#include "proc/resource_ns.h"
#include "net/net_config.h"
#include "core/bootargs.h"
#include "core/string.h"
#include "core/stdio.h"
#include "net/stack.h"

a20_net_config_t g_a20_net_config;

static int parse_ipv4(const char *s, net_ipv4_addr_t *out)
{
    unsigned octets[4];
    const char *p = s;

    if (!s || !s[0])
        return -1;

    for (int i = 0; i < 4; i++) {
        unsigned val = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (unsigned)(*p - '0');
            digits++;
            p++;
            if (digits > 3 || val > 255)
                return -1;
        }
        if (digits == 0)
            return -1;
        octets[i] = val;
        if (i < 3) {
            if (*p != '.')
                return -1;
            p++;
        }
    }
    if (*p != '\0')
        return -1;

    uint8_t *bytes = (uint8_t *)&out->addr;
    for (int i = 0; i < 4; ++i) bytes[i] = (uint8_t)octets[i];
    return 0;
}

static const char *extract_key_value(const char *tok, const char *tok_end,
                                     const char *key, char *val, size_t valsz)
{
    size_t klen = strlen(key);
    if ((size_t)(tok_end - tok) <= klen)
        return NULL;
    if (strncmp(tok, key, klen) != 0 || tok[klen] != '=')
        return NULL;

    const char *vstart = tok + klen + 1;
    size_t vlen = (size_t)(tok_end - vstart);
    if (vlen >= valsz)
        vlen = valsz - 1;
    memcpy(val, vstart, vlen);
    val[vlen] = '\0';
    return tok_end;
}

void a20_net_config_init(void)
{
    memset(&g_a20_net_config, 0, sizeof(g_a20_net_config));
    /* A physical board with an Ethernet cable should be usable without
     * hand-editing a boot command line.  Static parameters or an explicit
     * a20.dhcp=0 below still select the static path. */
    g_a20_net_config.dhcp_enable = 1;

    const char *cmdline = bootargs_get();
    if (!cmdline)
        return;

    char val[256];
    int dhcp_explicit = 0;

    const char *p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *tok_end = p;
        while (*tok_end && *tok_end != ' ' && *tok_end != '\t')
            tok_end++;

        if (extract_key_value(p, tok_end, "a20.dhcp", val, sizeof(val))) {
            g_a20_net_config.dhcp_enable = (atoi(val) != 0);
            dhcp_explicit = 1;
        }

        p = tok_end;
    }

    p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *tok_end = p;
        while (*tok_end && *tok_end != ' ' && *tok_end != '\t')
            tok_end++;

        if (extract_key_value(p, tok_end, "a20.ip", val, sizeof(val)))
            parse_ipv4(val, &g_a20_net_config.ip);
        else if (extract_key_value(p, tok_end, "a20.netmask", val, sizeof(val)))
            parse_ipv4(val, &g_a20_net_config.netmask);
        else if (extract_key_value(p, tok_end, "a20.gateway", val, sizeof(val)))
            parse_ipv4(val, &g_a20_net_config.gateway);

        if (extract_key_value(p, tok_end, "a20.dns", val, sizeof(val))) {
            if (g_a20_net_config.dns_count < A20_NET_DNS_SERVERS &&
                parse_ipv4(val, &g_a20_net_config.dns[g_a20_net_config.dns_count]) == 0) {
                g_a20_net_config.dns_count++;
            }
        }

        if (extract_key_value(p, tok_end, "a20.hostname", val, sizeof(val))) {
            strncpy(g_a20_net_config.hostname, val,
                    sizeof(g_a20_net_config.hostname) - 1);
            g_a20_net_config.hostname[sizeof(g_a20_net_config.hostname) - 1] = '\0';
        }

        /*
         * Selects the TCP mode before any socket exists.  This has to be a
         * command-line key rather than only a /proc/net/config write, because
         * the first listener a server opens is usually opened by userspace at
         * boot -- a write from a shell cannot run before that, and until the
         * listener holds a real LISTEN pcb an inbound connection is answered
         * with RST.
         */
        if (extract_key_value(p, tok_end, "a20.tcpmode", val, sizeof(val))) {
            if (strcmp(val, "lwip") == 0)
                g_a20_tcp_path = A20_TCP_PATH_LWIP;
            else
                g_a20_tcp_path = A20_TCP_PATH_FAST;
        }

        p = tok_end;
    }

    /* Preserve the old static QEMU command lines, which provide an address
     * but do not carry a20.dhcp=0.  An explicit a20.dhcp=1 always wins. */
    if (!dhcp_explicit &&
        (g_a20_net_config.ip.addr != 0 ||
         g_a20_net_config.netmask.addr != 0 ||
         g_a20_net_config.gateway.addr != 0))
        g_a20_net_config.dhcp_enable = 0;
}

int a20_net_config_format(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return 0;

    if (!resource_ns_is_initial(resource_ns_current(PROCNS_NET)))
        return snprintf(buf, bufsz, "ip=127.0.0.1\nnetmask=255.0.0.0\ngateway=0.0.0.0\ndns_count=0\ndhcp=0\n");
    a20_net_config_t config;
    net_stack_config_snapshot(&config);

    size_t off = 0;

    char ipb[24], nmb[24], gwb[24];
    snprintf(ipb, sizeof(ipb), "%u.%u.%u.%u",
             ((const uint8_t *)&config.ip)[0],
             ((const uint8_t *)&config.ip)[1],
             ((const uint8_t *)&config.ip)[2],
             ((const uint8_t *)&config.ip)[3]);
    snprintf(nmb, sizeof(nmb), "%u.%u.%u.%u",
             ((const uint8_t *)&config.netmask)[0],
             ((const uint8_t *)&config.netmask)[1],
             ((const uint8_t *)&config.netmask)[2],
             ((const uint8_t *)&config.netmask)[3]);
    snprintf(gwb, sizeof(gwb), "%u.%u.%u.%u",
             ((const uint8_t *)&config.gateway)[0],
             ((const uint8_t *)&config.gateway)[1],
             ((const uint8_t *)&config.gateway)[2],
             ((const uint8_t *)&config.gateway)[3]);

    int n = snprintf(buf + off, bufsz - off,
                     "ip=%s\nnetmask=%s\ngateway=%s\n",
                     ipb, nmb, gwb);
    if (n > 0) off += (size_t)n;

    for (int i = 0; i < config.dns_count && i < A20_NET_DNS_SERVERS; i++) {
        char db[24];
        snprintf(db, sizeof(db), "%u.%u.%u.%u",
                 ((const uint8_t *)&config.dns[i])[0],
                 ((const uint8_t *)&config.dns[i])[1],
                 ((const uint8_t *)&config.dns[i])[2],
                 ((const uint8_t *)&config.dns[i])[3]);
        n = snprintf(buf + off, bufsz - off, "dns%d=%s\n", i, db);
        if (n > 0) off += (size_t)n;
    }

    n = snprintf(buf + off, bufsz - off,
                 "dhcp=%d\nhostname=%s\n",
                 config.dhcp_enable,
                 config.hostname[0] ? config.hostname : "(none)");
    if (n > 0) off += (size_t)n;

    n = snprintf(buf + off, bufsz - off, "tcpmode=%s\n",
                 g_a20_tcp_path == A20_TCP_PATH_LWIP ? "lwip" : "fast");
    if (n > 0) off += (size_t)n;

    if (off >= bufsz)
        off = bufsz - 1;
    return (int)off;
}

/* Default to a real lwIP LISTEN pcb.  The fast mode keeps the listener out of
 * the protocol stack entirely, which makes the port reachable only from other
 * processes in this kernel: an inbound connection from another machine (a TUN
 * peer, the LAN, a tailnet) is answered with RST.  A general-purpose system
 * must accept off-box connections, so lwip is the default; a20.tcpmode=fast
 * remains available for the accept-semantics tests that predate this. */
a20_tcp_path_t g_a20_tcp_path = A20_TCP_PATH_LWIP;

int a20_net_config_write(const char *buf, size_t count)
{
    if (!resource_ns_is_initial(resource_ns_current(PROCNS_NET))) return -EOPNOTSUPP;
    char tmp[32];
    size_t n = count < sizeof(tmp) - 1 ? count : sizeof(tmp) - 1;
    memcpy(tmp, buf, n);
    tmp[n] = '\0';
    while (n > 0 && (tmp[n - 1] == '\n' || tmp[n - 1] == '\r' ||
                     tmp[n - 1] == ' ' || tmp[n - 1] == '\t'))
        tmp[--n] = '\0';

    if (strcmp(tmp, "tcpmode fast") == 0) {
        g_a20_tcp_path = A20_TCP_PATH_FAST;
        return 0;
    }
    if (strcmp(tmp, "tcpmode lwip") == 0) {
        g_a20_tcp_path = A20_TCP_PATH_LWIP;
        return 0;
    }
    return -EINVAL;
}
