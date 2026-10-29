#include "native.h"
#include "net/net_config.h"
#include "net/lwip_stack.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
void a20_net_config_sync_from_lwip(void)
{
    struct netif *n = netif_default;
    if (!n)
        return;

    g_a20_net_config.ip.addr = netif_ip4_addr(n)->addr;
    g_a20_net_config.netmask.addr = netif_ip4_netmask(n)->addr;
    g_a20_net_config.gateway.addr = netif_ip4_gw(n)->addr;

    g_a20_net_config.dns_count = 0;
#if LWIP_DNS
    for (int i = 0; i < A20_NET_DNS_SERVERS; i++) {
        const ip_addr_t *d = dns_getserver(i);
        if (d && IP_IS_V4(d) && !ip_addr_isany(d)) {
            g_a20_net_config.dns[i].addr = ip_2_ip4(d)->addr;
            g_a20_net_config.dns_count++;
        }
    }
#endif
}

void a20_lwip_config_snapshot(a20_net_config_t *out) {
    uint64_t flags = a20_lwip_lock();
    a20_net_config_sync_from_lwip();
    *out = g_a20_net_config;
    a20_lwip_unlock(flags);
}
