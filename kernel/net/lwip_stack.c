#include "net/lwip_stack.h"
#include "net/socket_internal.h"
#include "net/net_config.h"
#include "core/timer.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/lock.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"

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

/*
 * LWIP_NO_THREAD_PROGRESS_CONTRACT:
 * - NO_SYS lwIP progress consists of sys_check_timeouts(), virtio-net TX
 *   completion cleanup, RX frame delivery into netif input, and netif_poll().
 * - a20_lwip_poll()/a20_lwip_poll_locked() are the only generic progress
 *   entries. Scheduler/idle access them only via kernel_progress_poll().
 * - g_lwip_lock serializes lwIP core state. While holding it, callers may use
 *   only nonblocking virtio-net send/recv/progress paths; blocking driver calls
 *   or reverse driver->lwIP lock acquisition are forbidden.
 * - Network smoke must cover timeout advancement, RX/TX delivery, DNS, UDP, TCP,
 *   and ICMP-facing paths before removing the compatibility poll bridge.
 *
 * Lock-safe entry points:
 * - a20_lwip_lock()/a20_lwip_unlock(): outer lock for all lwIP API calls.
 * - a20_lwip_poll_locked(): run with g_lwip_lock held; does not allocate or
 *   acquire g_net_lock.
 * - a20_lwip_poll(): acquires g_lwip_lock, runs progress, releases it, then
 *   runs the socket deferred bottom-half (net_inet_bottom_half_process_all)
 *   under g_net_lock only.  This is the only generic path that may transition
 *   from g_lwip_lock to g_net_lock, and the two locks are never held together.
 */
static int g_lwip_ready;
static spinlock_t g_lwip_lock = SPINLOCK_INIT;
#define A20_NET_MAX_DEVS 4

/*
 * RX progress hint.  The virtio-net IRQ top-half raises this flag before
 * draining a netif under g_lwip_lock; a20_lwip_poll_locked() consumes it.
 * kernel_progress_poll() skips the scheduler hot-path drain (and the
 * g_lwip_lock acquisition) while no device has work pending, turning the
 * per-context-switch compatibility poll into an event-driven bottom-half.
 * Platforms whose transport has no IRQ line must keep draining unconditionally
 * (see virtio_net_poll_rx_all()); they are tracked through the driver.
 */
static volatile unsigned g_lwip_rx_pending;

int a20_lwip_rx_pending_any(void)
{
    return __atomic_load_n(&g_lwip_rx_pending, __ATOMIC_ACQUIRE) != 0;
}

void a20_lwip_signal_rx_pending(void)
{
    __atomic_store_n(&g_lwip_rx_pending, 1, __ATOMIC_RELEASE);
}

static void a20_lwip_clear_rx_pending(void)
{
    __atomic_store_n(&g_lwip_rx_pending, 0, __ATOMIC_RELEASE);
}

static struct netif g_netifs[A20_NET_MAX_DEVS];
static struct netif g_loopif;

typedef struct {
    int idx;
    device_t *dev;
    const net_dev_ops_t *ops;
    uint8_t rx_frame[1536];
    uint8_t tx_frame[1536];
} a20_lwip_netif_state_t;

static a20_lwip_netif_state_t g_netif_state[A20_NET_MAX_DEVS];

u32_t sys_now(void) {
    return (u32_t)(timer_get_ticks() * 1000UL / TICKS_PER_SEC);
}

static err_t a20_lwip_linkoutput(struct netif *netif, struct pbuf *p) {
    if (!netif || !netif->state || !p)
        return ERR_ARG;

    a20_lwip_netif_state_t *st = (a20_lwip_netif_state_t *)netif->state;
    if (p->tot_len > sizeof(st->tx_frame))
        return ERR_BUF;

    pbuf_copy_partial(p, st->tx_frame, p->tot_len, 0);
    int r = st->ops->send(st->dev, st->tx_frame, p->tot_len);
    return (r == (int)p->tot_len) ? ERR_OK : ERR_IF;
}

static err_t a20_lwip_netif_init_cb(struct netif *netif) {
    if (!netif || !netif->state)
        return ERR_ARG;

    a20_lwip_netif_state_t *st = (a20_lwip_netif_state_t *)netif->state;
    const uint8_t *mac = st->ops->mac(st->dev);
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

static err_t a20_lwip_loopif_init_cb(struct netif *netif)
{
    const char *hostname = g_a20_net_config.hostname[0] ?
                           g_a20_net_config.hostname : "a20os";
    netif->hostname = hostname;
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_LINK_UP;
#if LWIP_IPV6
    static s8_t sn[] = {0, 0, 0, 0, 0, 0, 0, 0};
    ip6_addr_t lo6;
    ip6_addr_set_loopback(&lo6);
    netif_add_ip6_address(netif, &lo6, sn);
    netif_ip6_addr_set_state(netif, 0, IP6_ADDR_VALID);
#endif
    return ERR_OK;
}

#if ENABLE_LOOPBACK
static err_t a20_lwip_loopif_output(struct netif *netif, struct pbuf *p,
                                    const ip4_addr_t *ipaddr)
{
    (void)ipaddr;
    return netif_loop_output(netif, p);
}
#endif

static void a20_lwip_register_loopif(void)
{
    ip4_addr_t lo_addr, lo_mask, lo_gw;
    ip4_addr_set_loopback(&lo_addr);
    IP4_ADDR(&lo_mask, 255, 0, 0, 0);
    ip4_addr_set_zero(&lo_gw);

    struct netif *n = netif_add(&g_loopif, &lo_addr, &lo_mask, &lo_gw,
                                NULL, a20_lwip_loopif_init_cb, netif_input);
    if (!n) {
        printf("[LWIP] failed to add loopback netif\n");
        return;
    }
    netif_set_up(n);
    netif_set_link_up(n);
    n->output = a20_lwip_loopif_output;
}

static void a20_lwip_register_netifs(void) {
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gw;

    ip4_addr_set_zero(&ipaddr);
    ip4_addr_set_zero(&netmask);
    ip4_addr_set_zero(&gw);

    int configured = 0;
    if (g_a20_net_config.dhcp_enable) {
        configured = 1;
    } else if (!ip4_addr_isany_val(g_a20_net_config.ip) ||
               !ip4_addr_isany_val(g_a20_net_config.netmask) ||
               !ip4_addr_isany_val(g_a20_net_config.gateway)) {
        configured = 1;
        ip4_addr_copy(ipaddr, g_a20_net_config.ip);
        ip4_addr_copy(netmask, g_a20_net_config.netmask);
        ip4_addr_copy(gw, g_a20_net_config.gateway);
    }

    for (int i = 0; i < A20_NET_MAX_DEVS; i++) {
        device_t *dev = device_find_by_class(DEV_CLASS_NET, i);
        if (!dev || !dev->drv || !dev->drv->class_ops)
            break;
        const net_dev_ops_t *ops = (const net_dev_ops_t *)dev->drv->class_ops;
        if (!ops->send || !ops->recv || !ops->mac)
            continue;

        g_netif_state[i].idx = i;
        g_netif_state[i].dev = dev;
        g_netif_state[i].ops = ops;
        struct netif *n = netif_add(&g_netifs[i], &ipaddr, &netmask, &gw,
                                    &g_netif_state[i],
                                    a20_lwip_netif_init_cb,
                                    ethernet_input);
        if (!n) {
            printf("[LWIP] failed to add %s\n", dev->name ? dev->name : "net");
            continue;
        }
        netif_set_default(n);
        netif_set_up(n);
        netif_set_link_up(n);
#if LWIP_IPV6
        netif_create_ip6_linklocal_address(n, 1);
#endif
#if LWIP_DNS
        for (int d = 0; d < g_a20_net_config.dns_count && d < DNS_MAX_SERVERS; d++) {
            ip_addr_t dns_addr;
            ip_addr_set_ip4_u32_val(dns_addr, g_a20_net_config.dns[d].addr);
            dns_setserver(d, &dns_addr);
        }
#endif
        if (configured) {
            printf("[LWIP] netif %c%c%d attached to %s ip=%u.%u.%u.%u gw=%u.%u.%u.%u dns_count=%d\n",
                   n->name[0], n->name[1], n->num,
                   dev->name ? dev->name : "net",
                   ip4_addr1(&ipaddr), ip4_addr2(&ipaddr),
                   ip4_addr3(&ipaddr), ip4_addr4(&ipaddr),
                   ip4_addr1(&gw), ip4_addr2(&gw),
                   ip4_addr3(&gw), ip4_addr4(&gw),
                   g_a20_net_config.dns_count);
        } else {
            printf("[LWIP] netif %c%c%d attached to %s (unconfigured)\n",
                   n->name[0], n->name[1], n->num,
                   dev->name ? dev->name : "net");
        }

#if LWIP_DHCP
        if (g_a20_net_config.dhcp_enable) {
            dhcp_start(n);
        }
#endif
    }
}

void a20_lwip_init(void) {
    if (g_lwip_ready)
        return;

    a20_net_config_init();
    spin_init(&g_lwip_lock);
    spin_set_debug(&g_lwip_lock, "lwip", NULL);
    lwip_init();
    a20_lwip_register_netifs();
    /* Add loopback after physical links.  lwIP prepends netifs to its list;
     * keeping loopback last here leaves hardware first for polling code and
     * for diagnostics which inspect netif_list. */
    a20_lwip_register_loopif();
    g_lwip_ready = 1;
    printf("[LWIP] initialized: IPv4 IPv6 TCP UDP RAW ICMP DHCP DNS loopif\n");
}

uint64_t a20_lwip_lock(void)
{
    return spin_lock_irqsave(&g_lwip_lock);
}

void a20_lwip_unlock(uint64_t flags)
{
    spin_unlock_irqrestore(&g_lwip_lock, flags);
}

static void a20_lwip_process_netif_rx_tx_locked(struct netif *n)
{
    if (!n || !n->state)
        return;

    a20_lwip_netif_state_t *st = (a20_lwip_netif_state_t *)n->state;

    for (;;) {
        int len = st->ops->recv(st->dev, st->rx_frame, sizeof(st->rx_frame));
        if (len <= 0)
            break;
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (!p) {
            LINK_STATS_INC(link.memerr);
            LINK_STATS_INC(link.drop);
            continue;
        }
        pbuf_take(p, st->rx_frame, (u16_t)len);
        if (n->input(p, n) != ERR_OK) {
            pbuf_free(p);
            LINK_STATS_INC(link.drop);
        }
    }
    netif_poll(n);
}

/*
 * IRQ top-half entry for a single virtio-net instance.
 * Runs with g_lwip_lock held; performs bounded work only (descriptor ring
 * drainer, lwIP input, no kmalloc, no g_net_lock).
 */
void a20_lwip_process_netif_irq_locked(int net_idx)
{
    if (!g_lwip_ready)
        return;

    a20_lwip_signal_rx_pending();

    for (int i = 0; i < A20_NET_MAX_DEVS; i++) {
        a20_lwip_netif_state_t *st = &g_netif_state[i];
        if (st->dev && st->ops && st->ops->poll)
            st->ops->poll(st->dev);
    }

    for (struct netif *n = netif_list; n; n = n->next) {
        if (!n->state)
            continue;
        a20_lwip_netif_state_t *st = (a20_lwip_netif_state_t *)n->state;
        if (st->idx == net_idx) {
            a20_lwip_process_netif_rx_tx_locked(n);
            break;
        }
    }
}

void a20_lwip_poll_locked(void) {
    if (!g_lwip_ready)
        return;
    sys_check_timeouts();
    a20_net_config_sync_from_lwip();
    for (int i = 0; i < A20_NET_MAX_DEVS; i++) {
        a20_lwip_netif_state_t *st = &g_netif_state[i];
        if (st->dev && st->ops && st->ops->poll)
            st->ops->poll(st->dev);
    }
    for (struct netif *n = netif_list; n; n = n->next) {
        if (n->state) {
            a20_lwip_process_netif_rx_tx_locked(n);
        } else {
            netif_poll(n);
        }
    }
    a20_lwip_clear_rx_pending();
}

void a20_lwip_poll(void) {
    uint64_t flags = a20_lwip_lock();
    a20_lwip_poll_locked();
    a20_lwip_unlock(flags);
    net_inet_bottom_half_process_all();
}

int a20_lwip_format_status(char *buf, size_t bufsz) {
    if (!buf || bufsz == 0)
        return 0;

    uint64_t flags = a20_lwip_lock();
    const char *ifname = "none";
    const char *state = "down";
    char ipbuf[24] = "0.0.0.0";
    char maskbuf[24] = "0.0.0.0";
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
        const ip4_addr_t *mask = netif_ip4_netmask(netif_default);
        const ip4_addr_t *gw = netif_ip4_gw(netif_default);
        snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u",
                 ip4_addr1(ip), ip4_addr2(ip), ip4_addr3(ip), ip4_addr4(ip));
        snprintf(maskbuf, sizeof(maskbuf), "%u.%u.%u.%u",
                 ip4_addr1(mask), ip4_addr2(mask), ip4_addr3(mask), ip4_addr4(mask));
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
        "lwip: ready=%d if=%s state=%s ip=%s mask=%s gw=%s dns=%s\n"
        "protocols: ipv4 ipv6 tcp udp raw icmp icmp6 dhcp dhcp6 dns arp igmp mld loopif\n"
        "pcbs: udp=%u tcp_active=%u tcp_listen=%u raw=%u\n"
        "link: xmit=%u recv=%u drop=%u chkerr=%u memerr=%u\n",
        g_lwip_ready, ifname, state, ipbuf, maskbuf, gwbuf, dnsbuf,
        (unsigned)lwip_stats.memp[MEMP_UDP_PCB]->used,
        (unsigned)lwip_stats.memp[MEMP_TCP_PCB]->used,
        (unsigned)lwip_stats.memp[MEMP_TCP_PCB_LISTEN]->used,
        (unsigned)lwip_stats.memp[MEMP_RAW_PCB]->used,
        (unsigned)lwip_stats.link.xmit,
        (unsigned)lwip_stats.link.recv,
        (unsigned)lwip_stats.link.drop,
        (unsigned)lwip_stats.link.chkerr,
        (unsigned)lwip_stats.link.memerr);
    a20_lwip_unlock(flags);
    if (n < 0)
        return 0;
    if ((size_t)n >= bufsz)
        return (int)bufsz - 1;
    return n;
}
