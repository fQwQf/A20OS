#include "net/net_config.h"
#include "core/bootargs.h"
#include "core/string.h"
#include "core/stdio.h"
#include "net/lwip_stack.h"
#include "lwip/netif.h"
#include "lwip/dns.h"

a20_net_config_t g_a20_net_config;

static int parse_ipv4(const char *s, ip4_addr_t *out)
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

    IP4_ADDR(out, (u8_t)octets[0], (u8_t)octets[1], (u8_t)octets[2], (u8_t)octets[3]);
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

    const char *cmdline = bootargs_get();
    if (!cmdline)
        return;

    char val[256];

    const char *p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        const char *tok_end = p;
        while (*tok_end && *tok_end != ' ' && *tok_end != '\t')
            tok_end++;

        if (extract_key_value(p, tok_end, "a20.dhcp", val, sizeof(val)))
            g_a20_net_config.dhcp_enable = (atoi(val) != 0);

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

        if (!g_a20_net_config.dhcp_enable) {
            if (extract_key_value(p, tok_end, "a20.ip", val, sizeof(val)))
                parse_ipv4(val, &g_a20_net_config.ip);
            else if (extract_key_value(p, tok_end, "a20.netmask", val, sizeof(val)))
                parse_ipv4(val, &g_a20_net_config.netmask);
            else if (extract_key_value(p, tok_end, "a20.gateway", val, sizeof(val)))
                parse_ipv4(val, &g_a20_net_config.gateway);
        }

        if (extract_key_value(p, tok_end, "a20.dns", val, sizeof(val))) {
            if (g_a20_net_config.dns_count < DNS_MAX_SERVERS) {
                parse_ipv4(val, &g_a20_net_config.dns[g_a20_net_config.dns_count]);
                g_a20_net_config.dns_count++;
            }
        }

        if (extract_key_value(p, tok_end, "a20.hostname", val, sizeof(val))) {
            strncpy(g_a20_net_config.hostname, val,
                    sizeof(g_a20_net_config.hostname) - 1);
            g_a20_net_config.hostname[sizeof(g_a20_net_config.hostname) - 1] = '\0';
        }

        p = tok_end;
    }
}

void a20_net_config_sync_from_lwip(void)
{
    struct netif *n = netif_default;
    if (n) {
        ip4_addr_copy(g_a20_net_config.ip, *netif_ip4_addr(n));
        ip4_addr_copy(g_a20_net_config.netmask, *netif_ip4_netmask(n));
        ip4_addr_copy(g_a20_net_config.gateway, *netif_ip4_gw(n));
    }

    g_a20_net_config.dns_count = 0;
#if LWIP_DNS
    for (int i = 0; i < DNS_MAX_SERVERS; i++) {
        const ip_addr_t *d = dns_getserver(i);
        if (d && !ip_addr_isany(d)) {
            ip4_addr_copy(g_a20_net_config.dns[i], *ip_2_ip4(d));
            g_a20_net_config.dns_count++;
        }
    }
#endif
}

int a20_net_config_format(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return 0;

    uint64_t flags = a20_lwip_lock();
    a20_net_config_sync_from_lwip();
    a20_lwip_unlock(flags);

    size_t off = 0;

    char ipb[24], nmb[24], gwb[24];
    snprintf(ipb, sizeof(ipb), "%u.%u.%u.%u",
             ip4_addr1(&g_a20_net_config.ip),
             ip4_addr2(&g_a20_net_config.ip),
             ip4_addr3(&g_a20_net_config.ip),
             ip4_addr4(&g_a20_net_config.ip));
    snprintf(nmb, sizeof(nmb), "%u.%u.%u.%u",
             ip4_addr1(&g_a20_net_config.netmask),
             ip4_addr2(&g_a20_net_config.netmask),
             ip4_addr3(&g_a20_net_config.netmask),
             ip4_addr4(&g_a20_net_config.netmask));
    snprintf(gwb, sizeof(gwb), "%u.%u.%u.%u",
             ip4_addr1(&g_a20_net_config.gateway),
             ip4_addr2(&g_a20_net_config.gateway),
             ip4_addr3(&g_a20_net_config.gateway),
             ip4_addr4(&g_a20_net_config.gateway));

    int n = snprintf(buf + off, bufsz - off,
                     "ip=%s\nnetmask=%s\ngateway=%s\n",
                     ipb, nmb, gwb);
    if (n > 0) off += (size_t)n;

    for (int i = 0; i < g_a20_net_config.dns_count && i < DNS_MAX_SERVERS; i++) {
        char db[24];
        snprintf(db, sizeof(db), "%u.%u.%u.%u",
                 ip4_addr1(&g_a20_net_config.dns[i]),
                 ip4_addr2(&g_a20_net_config.dns[i]),
                 ip4_addr3(&g_a20_net_config.dns[i]),
                 ip4_addr4(&g_a20_net_config.dns[i]));
        n = snprintf(buf + off, bufsz - off, "dns%d=%s\n", i, db);
        if (n > 0) off += (size_t)n;
    }

    n = snprintf(buf + off, bufsz - off,
                 "dhcp=%d\nhostname=%s\n",
                 g_a20_net_config.dhcp_enable,
                 g_a20_net_config.hostname[0] ? g_a20_net_config.hostname : "(none)");
    if (n > 0) off += (size_t)n;

    if (off >= bufsz)
        off = bufsz - 1;
    return (int)off;
}
