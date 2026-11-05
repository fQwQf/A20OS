/* Network backend registry.
 *
 * Only compiled backends register.  The build defines exactly one of
 * CONFIG_NET_BACKEND_LWIP / CONFIG_NET_BACKEND_BSD (and, in the future,
 * CONFIG_NET_BACKEND_<NAME> for additional stacks); the instance configuration
 * supplies CONFIG_NET_BACKEND_NAME.  Selection is explicit and never falls
 * back, so a mis-typed instance cannot silently run a different stack.
 */
#include "net/backend.h"
#include "core/errno.h"
#include "core/stdio.h"
#include "core/string.h"

#ifndef CONFIG_NET_BACKEND_NAME
#define CONFIG_NET_BACKEND_NAME "none"
#endif

#if defined(CONFIG_NET_BACKEND_LWIP)
extern const net_backend_t a20_lwip_backend;
extern void a20_lwip_backend_register(void);
#endif
#if defined(CONFIG_NET_BACKEND_BSD)
extern const net_backend_t a20_bsd_backend;
extern void a20_bsd_backend_register(void);
#endif

static const net_backend_t *g_registry[NET_BACKEND_MAX];
static unsigned g_registry_count;
const net_backend_t *a20_net_backend_active;

int net_backend_register(const net_backend_t *backend) {
    if (!backend || !backend->name || !backend->name[0] || !backend->socket)
        return -EINVAL;
    if (!backend->cycle || !backend->event)
        return -EINVAL;
    if (!backend->socket->open || !backend->socket->close ||
        !backend->socket->bind || !backend->socket->connect ||
        !backend->socket->send)
        return -EINVAL;
    /* Stream sockets need the option module: TCP_NODELAY and the buffer
     * ceiling have no other home. */
    if ((backend->caps & (NET_CAP_STREAM | NET_CAP_SNDBUF | NET_CAP_RCVBUF)) &&
        !backend->opts)
        return -EINVAL;

    /* A capability is an ABI promise.  Refuse a backend whose descriptor
     * advertises one without the module that implements it: the frontend
     * would otherwise forward the promise to userspace and then fail at the
     * call site, which is exactly the "accepted but never applied" bug the
     * capability bits exist to prevent. */
    if ((backend->caps & NET_CAP_LISTEN) && (!backend->socket->listen))
        return -EINVAL;
    if ((backend->caps & NET_CAP_SHUTDOWN) && (!backend->socket->shutdown))
        return -EINVAL;
    if ((backend->caps & NET_CAP_BINDTODEVICE) &&
        (!backend->endpoint || !backend->endpoint->bind_device))
        return -EINVAL;
    if ((backend->caps & NET_CAP_UDP_CONTROL) &&
        (!backend->endpoint || !backend->endpoint->udp_control))
        return -EINVAL;
    if ((backend->caps & NET_CAP_PMTU) &&
        (!backend->endpoint || !backend->endpoint->pmtu))
        return -EINVAL;
    if ((backend->caps & (NET_CAP_RAW_TX | NET_CAP_RAW_IPV6_TX)) &&
        (!backend->endpoint || !backend->endpoint->raw_send))
        return -EINVAL;
    if ((backend->caps & NET_CAP_TUN) &&
        (!backend->packet || !backend->packet->tun_create ||
         !backend->packet->tun_inject))
        return -EINVAL;
    if ((backend->caps & NET_CAP_NEIGH) &&
        (!backend->iface || !backend->iface->neighbor_update_v4))
        return -EINVAL;
    if ((backend->caps & NET_CAP_ROUTE) &&
        (!backend->iface || !backend->iface->route_update_v4))
        return -EINVAL;
    if ((backend->caps & NET_CAP_IF_DYNAMIC) &&
        (!backend->iface || !backend->iface->set_mtu ||
         !backend->iface->set_flags || !backend->iface->set_addr))
        return -EINVAL;
    if ((backend->caps & NET_CAP_CONGESTION) &&
        (!backend->opts || !backend->opts->validate))
        return -EINVAL;

    for (unsigned i = 0; i < g_registry_count; ++i) {
        if (!strcmp(g_registry[i]->name, backend->name))
            return -EEXIST;
    }
    if (g_registry_count >= NET_BACKEND_MAX)
        return -ENOSPC;
    g_registry[g_registry_count++] = backend;
    return 0;
}

const net_backend_t *net_backend_find(const char *name) {
    if (!name)
        return NULL;
    for (unsigned i = 0; i < g_registry_count; ++i) {
        if (!strcmp(g_registry[i]->name, name))
            return g_registry[i];
    }
    return NULL;
}

int net_backend_select(const char *name) {
    if (!name || !name[0])
        name = CONFIG_NET_BACKEND_NAME;
    const net_backend_t *backend = net_backend_find(name);
    if (!backend)
        return -ENODEV;
    a20_net_backend_active = backend;
    return 0;
}

int net_backend_has(uint32_t caps) {
    return a20_net_backend_active &&
           (a20_net_backend_active->caps & caps) == caps;
}

static const struct {
    uint32_t bit;
    const char *name;
} g_cap_names[] = {
    {NET_CAP_STREAM, "stream"},
    {NET_CAP_DGRAM, "dgram"},
    {NET_CAP_RAW, "raw"},
    {NET_CAP_IPV6, "ipv6"},
    {NET_CAP_LISTEN, "listen"},
    {NET_CAP_SHUTDOWN, "shutdown"},
    {NET_CAP_SNDBUF, "sndbuf"},
    {NET_CAP_RCVBUF, "rcvbuf"},
    {NET_CAP_PKTINFO, "pktinfo"},
    {NET_CAP_BINDTODEVICE, "bindtodev"},
    {NET_CAP_UDP_CONTROL, "udpctrl"},
    {NET_CAP_PMTU, "pmtu"},
    {NET_CAP_RAW_TX, "rawtx"},
    {NET_CAP_RAW_IPV6_TX, "rawtx6"},
    {NET_CAP_TUN, "tun"},
    {NET_CAP_NEIGH, "neigh"},
    {NET_CAP_ROUTE, "route"},
    {NET_CAP_IF_ADDR6, "ifaddr6"},
    {NET_CAP_IF_DYNAMIC, "ifdynamic"},
    {NET_CAP_CONGESTION, "congestion"},
    {NET_CAP_DEVICE_IRQ, "irq"},
    {NET_CAP_MULTI_QUEUE, "multiqueue"},
    {NET_CAP_OFFLOAD_SG, "txsg"},
    {NET_CAP_OFFLOAD_CSUM, "csumoffload"},
};

int net_backend_format_caps(char *buf, size_t len) {
    if (!buf || !len)
        return 0;
    if (!a20_net_backend_active)
        return snprintf(buf, len, "%s: (no backend)\n",
                        CONFIG_NET_BACKEND_NAME);
    int used = snprintf(buf, len, "%s:", a20_net_backend_active->name);
    if (used < 0 || (size_t)used >= len)
        return (int)len - 1;
    int first = 1;
    for (unsigned i = 0; i < sizeof(g_cap_names) / sizeof(g_cap_names[0]);
         ++i) {
        if (!(a20_net_backend_active->caps & g_cap_names[i].bit))
            continue;
        int n = snprintf(buf + used, len - (size_t)used, "%s%s",
                         first ? " " : ",", g_cap_names[i].name);
        if (n < 0 || (size_t)(used + n) >= len)
            return (int)len - 1;
        used += n;
        first = 0;
    }
    if (first) {
        int n = snprintf(buf + used, len - (size_t)used, " (no optional caps)");
        if (n < 0 || (size_t)(used + n) >= len)
            return (int)len - 1;
        used += n;
    }
    if ((size_t)used >= len - 1)
        return (int)len - 1;
    buf[used++] = '\n';
    buf[used] = '\0';
    return used;
}

/* Called once by net_stack_init().  Kept here so backend selection, capability
 * queries and status rendering all have exactly one owner. */
void net_backend_registry_init(void) {
    if (g_registry_count)
        return;
    int rc;
#if defined(CONFIG_NET_BACKEND_LWIP)
    rc = net_backend_register(&a20_lwip_backend);
    if (rc < 0)
        printf("[NET] backend '%s' rejected: %d\n", a20_lwip_backend.name, rc);
#endif
#if defined(CONFIG_NET_BACKEND_BSD)
    rc = net_backend_register(&a20_bsd_backend);
    if (rc < 0)
        printf("[NET] backend '%s' rejected: %d\n", a20_bsd_backend.name, rc);
#endif
    (void)rc;
}
