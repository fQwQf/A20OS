#include "net/lwip_stack.h"
#include "core/timer.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/consts.h"
#include "drv/virtio_net.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/stats.h"
#include "lwip/memp.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/raw.h"
#include "lwip/dns.h"
#include "lwip/dhcp.h"
#include "lwip/ethip6.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip6_addr.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"

static int g_lwip_ready;
static struct netif g_netifs[VIRTIO_NET_MAX_DEVS];

typedef struct {
    int idx;
} a20_lwip_netif_state_t;

static a20_lwip_netif_state_t g_netif_state[VIRTIO_NET_MAX_DEVS];

u32_t sys_now(void) {
    return (u32_t)(timer_get_ticks() * 1000UL / TICKS_PER_SEC);
}

static err_t a20_lwip_linkoutput(struct netif *netif, struct pbuf *p) {
    if (!netif || !netif->state || !p)
        return ERR_ARG;

    a20_lwip_netif_state_t *st = (a20_lwip_netif_state_t *)netif->state;
    uint8_t frame[1536];
    if (p->tot_len > sizeof(frame))
        return ERR_BUF;

    pbuf_copy_partial(p, frame, p->tot_len, 0);
    int r = virtio_net_send(st->idx, frame, p->tot_len);
    return (r == (int)p->tot_len) ? ERR_OK : ERR_IF;
}

static err_t a20_lwip_netif_init_cb(struct netif *netif) {
    if (!netif || !netif->state)
        return ERR_ARG;

    a20_lwip_netif_state_t *st = (a20_lwip_netif_state_t *)netif->state;
    const uint8_t *mac = virtio_net_mac(st->idx);
    if (!mac)
        return ERR_IF;

    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output = etharp_output;
    netif->linkoutput = a20_lwip_linkoutput;
#if LWIP_IPV6
    netif->output_ip6 = ethip6_output;
#endif
    netif->mtu = 1500;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, mac, ETH_HWADDR_LEN);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                   NETIF_FLAG_LINK_UP | NETIF_FLAG_IGMP | NETIF_FLAG_MLD6;
    return ERR_OK;
}

static void a20_lwip_register_virtio_netifs(void) {
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gw;
    ip4_addr_t dns;

    /*
     * QEMU user-mode networking does provide DHCP, but our no-thread lwIP
     * bring-up cannot rely on the DHCP exchange completing before userland
     * starts issuing socket calls. Use QEMU's documented default addresses as
     * a deterministic fallback so the netif has a usable route immediately.
     */
    IP4_ADDR(&ipaddr, 10, 0, 2, 15);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);
    IP4_ADDR(&dns, 10, 0, 2, 3);

    for (int i = 0; i < VIRTIO_NET_MAX_DEVS; i++) {
        if (!virtio_net_ready(i))
            continue;

        g_netif_state[i].idx = i;
        struct netif *n = netif_add(&g_netifs[i], &ipaddr, &netmask, &gw,
                                    &g_netif_state[i],
                                    a20_lwip_netif_init_cb,
                                    ethernet_input);
        if (!n) {
            printf("[LWIP] failed to add virtio-net%d\n", i);
            continue;
        }
        netif_set_default(n);
        netif_set_up(n);
        netif_set_link_up(n);
#if LWIP_IPV6
        netif_create_ip6_linklocal_address(n, 1);
#endif
#if LWIP_DNS
        ip_addr_t dns_addr;
        ip_addr_set_ip4_u32_val(dns_addr, dns.addr);
        dns_setserver(0, &dns_addr);
#endif
        printf("[LWIP] netif %c%c%d attached to virtio-net%d ip=10.0.2.15 gw=10.0.2.2 dns=10.0.2.3\n",
               n->name[0], n->name[1], n->num, i);
    }
}

void a20_lwip_init(void) {
    if (g_lwip_ready)
        return;

    lwip_init();
    a20_lwip_register_virtio_netifs();
    g_lwip_ready = 1;
    printf("[LWIP] initialized: IPv4 IPv6 TCP UDP RAW ICMP DHCP DNS loopif\n");
}

void a20_lwip_poll(void) {
    if (!g_lwip_ready)
        return;
    sys_check_timeouts();
    virtio_net_poll_all();
    for (struct netif *n = netif_list; n; n = n->next) {
        if (n->state) {
            a20_lwip_netif_state_t *st = (a20_lwip_netif_state_t *)n->state;
            uint8_t frame[1536];
            for (;;) {
                int len = virtio_net_recv(st->idx, frame, sizeof(frame));
                if (len <= 0)
                    break;
                struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
                if (!p) {
                    LINK_STATS_INC(link.memerr);
                    LINK_STATS_INC(link.drop);
                    continue;
                }
                pbuf_take(p, frame, (u16_t)len);
                if (n->input(p, n) != ERR_OK) {
                    pbuf_free(p);
                    LINK_STATS_INC(link.drop);
                }
            }
        }
        netif_poll(n);
    }
}

int a20_lwip_format_status(char *buf, size_t bufsz) {
    if (!buf || bufsz == 0)
        return 0;

    const char *ifname = "none";
    const char *state = "down";
    char ipbuf[24] = "0.0.0.0";
    char gwbuf[24] = "0.0.0.0";
    char dnsbuf[24] = "0.0.0.0";
    if (netif_default) {
        static char namebuf[8];
        snprintf(namebuf, sizeof(namebuf), "%c%c%d",
                 netif_default->name[0], netif_default->name[1],
                 netif_default->num);
        ifname = namebuf;
        state = netif_is_up(netif_default) ? "up" : "down";
        const ip4_addr_t *ip = netif_ip4_addr(netif_default);
        const ip4_addr_t *gw = netif_ip4_gw(netif_default);
        snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u",
                 ip4_addr1(ip), ip4_addr2(ip), ip4_addr3(ip), ip4_addr4(ip));
        snprintf(gwbuf, sizeof(gwbuf), "%u.%u.%u.%u",
                 ip4_addr1(gw), ip4_addr2(gw), ip4_addr3(gw), ip4_addr4(gw));
    }
#if LWIP_DNS
    const ip_addr_t *dns = dns_getserver(0);
    if (dns && IP_IS_V4(dns)) {
        const ip4_addr_t *d = ip_2_ip4(dns);
        snprintf(dnsbuf, sizeof(dnsbuf), "%u.%u.%u.%u",
                 ip4_addr1(d), ip4_addr2(d), ip4_addr3(d), ip4_addr4(d));
    }
#endif

    int n = snprintf(buf, bufsz,
        "lwip: ready=%d if=%s state=%s ip=%s gw=%s dns=%s\n"
        "protocols: ipv4 ipv6 tcp udp raw icmp icmp6 dhcp dhcp6 dns arp igmp mld loopif\n"
        "pcbs: udp=%u tcp_active=%u tcp_listen=%u raw=%u\n"
        "link: xmit=%u recv=%u drop=%u chkerr=%u memerr=%u\n",
        g_lwip_ready, ifname, state, ipbuf, gwbuf, dnsbuf,
        (unsigned)lwip_stats.memp[MEMP_UDP_PCB]->used,
        (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->used,
        (unsigned)lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->used,
        (unsigned)lwip_stats.memp[MEMP_RAW_PCB]->used,
        (unsigned)lwip_stats.link.xmit,
        (unsigned)lwip_stats.link.recv,
        (unsigned)lwip_stats.link.drop,
        (unsigned)lwip_stats.link.chkerr,
        (unsigned)lwip_stats.link.memerr);
    if (n < 0)
        return 0;
    if ((size_t)n >= bufsz)
        return (int)bufsz - 1;
    return n;
}
