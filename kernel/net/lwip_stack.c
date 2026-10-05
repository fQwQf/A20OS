#include "net/lwip_stack.h"
#include "net/socket_internal.h"
#include "net/net_config.h"
#include "net/netfilter.h"
#include "core/timer.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/lock.h"
#include "core/lock_counters.h"
#include "core/perf.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"

/* Optional driver facility, resolved weakly for the same reason
 * core/progress.c resolves virtio_net_poll_rx_all_bounded weakly: virtio-net
 * may be absent or supplied as a loadable .a20drv, and the stack must build and
 * run either way.  Checked for non-NULL before use, so an absent driver leaves
 * the counters out rather than reporting zeros that look like a quiet link. */
extern void virtio_net_dev_stats(struct device *dev,
                                 net_dev_stats_t *out) __attribute__((weak));

#include "core/cpu.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/stats.h"
#include "lwip/memp.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/raw.h"
#include "lwip/dns.h"
#include "lwip/dhcp.h"
#include "lwip/ethip6.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip6_addr.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"

/* Defined far below, next to the other /proc row formatters.  Declared here
 * because a20_lwip_format_status() appends its lane line with it. */
static void a20_lwip_append(char *buf, size_t bufsz, size_t *off,
                            const char *row);

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
 *   acquire a socket-table bucket lock.
 * - a20_lwip_poll(): acquires g_lwip_lock, runs progress, releases it, then
 *   runs the socket deferred bottom-half (net_inet_bottom_half_process_all)
 *   under a socket-table bucket lock only.
 *
 * "The two locks are never held together" used to be false here, and the lane
 * work below reasoned from that.  net_inet_accept_stage_drain() ran inside the
 * g_net_lock region and took a20_lwip_lock() for the pcb handoff, so both were
 * genuinely held together on the accept path.  The socket-table sharding
 * removed the reason for it: the drain now drops the listener's bucket across
 * the handoff (it has to, because it also registers each child, and
 * net_register_socket_locked() takes a shard of its own), so the rule holds
 * again for the accept path.
 *
 * Order still matters and is still one-way: nothing takes g_lwip_lock and then
 * a socket-table bucket lock, so there is no ABBA cycle.  Any future path that
 * does -- which stage D wants, since draining a receive ring per lane wants to
 * touch socket state -- deadlocks against this one.  Treat net->lwip as the
 * fixed order.  See docs/net/network-lock-contract.md and
 * docs/measured/impl-notes-net.md.
 */
static int g_lwip_ready;
static spinlock_t g_lwip_lock = SPINLOCK_INIT;
#define A20_LWIP_LOCK_UNOWNED 0xffffffffu
#define A20_LWIP_LOCK_SITES 8
#if CONFIG_NET_LOCK_ASSERT
static volatile unsigned g_lwip_lock_owner = A20_LWIP_LOCK_UNOWNED;
static unsigned g_lwip_lock_violations;
static void *g_lwip_lock_sites[A20_LWIP_LOCK_SITES];
static unsigned g_lwip_lock_nsites;
#endif /* CONFIG_NET_LOCK_ASSERT */
/*
 * Device count follows the profile.  This was a hardcoded 4 on every rung,
 * and each entry carries two frame buffers -- 12672 B of unconditional .bss on
 * a build whose whole point was to fit a 20 KiB part.  Every use of the array
 * is a bounded loop over this ceiling, and a20_lwip_register_netifs() breaks at
 * the first index the device class does not have, so a smaller ceiling means
 * "only that many NICs are ever registered", never an overrun.  See
 * docs/server-readiness.md.
 */
#define A20_NET_MAX_DEVS NET_PROFILE_NETIF_MAX_DEVS

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
    if (__atomic_load_n(&g_lwip_rx_pending, __ATOMIC_ACQUIRE) != 0)
        return 1;
    /*
     * A queued loopback packet is work in exactly the sense the hint means, and
     * leaving it out starves the loopif.  netif_loop_output() only enqueues onto
     * netif->loop_first; netif_poll() is the sole drain, and netif_poll() is
     * only reached from the a20_lwip_poll_* family, which this gate otherwise
     * skips.  A loopback TCP transfer raises no device RX, so with the gate
     * closed on the device hint alone the SYN sits in loop_first forever and the
     * connecting task parks until its timeout.  Reading loop_first here is safe
     * without g_lwip_lock: it is a NULL check on a pointer the producer publishes
     * under SYS_ARCH_PROTECT, and a false positive only costs one extra
     * acquisition, which is what the gate is trying to avoid but cannot do by
     * lying about pending work.
     */
    for (struct netif *n = netif_list; n; n = n->next) {
        if (n->loop_first != NULL)
            return 1;
    }
    return 0;
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
    uint8_t rx_frame[NET_PROFILE_NETIF_FRAME_SIZE];
    uint8_t tx_frame[NET_PROFILE_NETIF_FRAME_SIZE];
    uint64_t rx_packets, rx_bytes, rx_errors, rx_dropped;
    uint64_t tx_packets, tx_bytes, tx_errors;
    uint64_t rx_filtered, tx_filtered;
} a20_lwip_netif_state_t;

/*
 * The frame buffers were a hardcoded 1536 on every profile while
 * PBUF_POOL_BUFSIZE was 512 on the embedded one, so a full-size frame was read
 * into a 1536 B buffer and then handed to pbuf_alloc() as a single 1536 B
 * request: three chained 512 B elements per frame, and PBUF_POOL_SIZE=24 held
 * eight full-size frames rather than the twenty-four the profile names.  They
 * now follow PBUF_POOL_BUFSIZE, and NET_PROFILE_NETIF_MTU keeps the link MTU
 * small enough that a frame still fits one element (see the assertions in
 * net_profile.h).
 *
 * The exact layout is asserted here rather than only in the header because the
 * header can only approximate it: 96 B is the non-frame part measured on
 * riscv64 LP64, and ILP32 has narrower pointers and counters.  Overstating
 * that term is the safe direction for a ceiling; understating it is not, which
 * is why sizeof() is what the per-profile budget is checked against here.
 */
_Static_assert(sizeof(a20_lwip_netif_state_t) <=
                   NET_PROFILE_NETIF_STATE_BYTES,
               "a20_lwip_netif_state_t exceeds the profile's per-netif state "
               "budget; these are unconditional .bss per registered device");

static a20_lwip_netif_state_t g_netif_state[A20_NET_MAX_DEVS];

static int a20_lwip_device_link_up(const a20_lwip_netif_state_t *st)
{
    return !st->ops->link_up || st->ops->link_up(st->dev) > 0;
}

static void a20_lwip_sync_link_state(struct netif *netif)
{
    if (!netif || !netif->state)
        return;

    a20_lwip_netif_state_t *st = (a20_lwip_netif_state_t *)netif->state;
    int link_up = a20_lwip_device_link_up(st);
    if (link_up && !netif_is_link_up(netif))
        netif_set_link_up(netif);
    else if (!link_up && netif_is_link_up(netif))
        netif_set_link_down(netif);
}

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
    if (netfilter_output(st->tx_frame, p->tot_len) == NETFILTER_DROP) {
        st->tx_filtered++;
        return ERR_OK;
    }
    int r = st->ops->send(st->dev, st->tx_frame, p->tot_len);
    if (r == (int)p->tot_len) {
        st->tx_packets++;
        st->tx_bytes += p->tot_len;
        a20_perf_count(A20_PERF_NET_TX_PACKETS);
        a20_perf_add(A20_PERF_NET_TX_BYTES, p->tot_len);
        return ERR_OK;
    }
    st->tx_errors++;
    return ERR_IF;
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
    netif->mtu = NET_PROFILE_NETIF_MTU;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, mac, ETH_HWADDR_LEN);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                   NETIF_FLAG_IGMP | NETIF_FLAG_MLD6;
    if (a20_lwip_device_link_up(st))
        netif->flags |= NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static err_t a20_lwip_loopif_init_cb(struct netif *netif)
{
    const char *hostname = g_a20_net_config.hostname[0] ?
                           g_a20_net_config.hostname : "a20os";
    netif->hostname = hostname;
    netif->name[0] = 'l';
    netif->name[1] = 'o';
    /* Same reasoning as the device netif: the loopback path also allocates from
     * PBUF_POOL, so its MTU has to leave a frame inside one pool element.  On
     * the default and server rungs this is the historical 1500. */
    netif->mtu = NET_PROFILE_NETIF_MTU;
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
        if (g_netif_state[i].dev == dev)
            continue;
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
        /* First one registered becomes the default, as on Linux.  Calling
         * this unconditionally per iteration let the last device enumerated
         * win, so a multi-NIC host silently routed through whichever happened
         * to probe last. */
        if (!netif_default)
            netif_set_default(n);
        netif_set_up(n);
        a20_lwip_sync_link_state(n);
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
    /* g_lwip_lock serialises the entire TCP/IP data plane, so its contention
     * is the single most important number for deciding whether the network
     * stack can ever scale across CPUs.  Register it (and enable per-callsite
     * sampling) so /proc/a20/lock_contention attributes it to exact call
     * sites.  Measuring before rewriting is deliberate: sharding a lock this
     * central is a high-risk protocol change, and the same callsite-first
     * method used for proc_lock is what made that rewrite's scope decidable
     * (see docs/roadmap/perf-overhaul.md). */
    lock_counters_register(&g_lwip_lock, "lwip");
    lock_counters_enable_callsite(&g_lwip_lock);
    lwip_init();
    a20_lwip_register_netifs();
    /* Add loopback after physical links.  lwIP prepends to netif_list, so
     * loopback ends up at the *head* -- the previous comment here claimed the
     * opposite and that hardware was left first.  Nothing depends on the
     * order either way: the poll loops walk the entire list and the IRQ path
     * matches on st->idx, so this is about keeping the hardware netifs
     * adjacent in diagnostics output, not about polling precedence. */
    a20_lwip_register_loopif();
    g_lwip_ready = 1;
    printf("[LWIP] initialized: IPv4 IPv6 TCP UDP RAW ICMP DHCP DNS loopif\n");
}

void a20_lwip_attach_netifs(void)
{
    if (!g_lwip_ready)
        return;

    uint64_t flags = a20_lwip_lock();
    a20_lwip_register_netifs();
    a20_lwip_unlock(flags);
}

uint64_t a20_lwip_lock(void)
{
    uint64_t flags = spin_lock_irqsave(&g_lwip_lock);
    a20_perf_count(A20_PERF_NET_LOCK_ACQUIRES);
#if CONFIG_NET_LOCK_ASSERT
    g_lwip_lock_owner = cpu_current_id();
#endif
    return flags;
}

void a20_lwip_unlock(uint64_t flags)
{
#if CONFIG_NET_LOCK_ASSERT
    g_lwip_lock_owner = A20_LWIP_LOCK_UNOWNED;
#endif
    spin_unlock_irqrestore(&g_lwip_lock, flags);
}

#if CONFIG_NET_LOCK_ASSERT
int a20_lwip_lock_is_held(void)
{
    return g_lwip_lock_owner == cpu_current_id();
}

void a20_lwip_note_lock_violation(void *site)
{
    __atomic_fetch_add(&g_lwip_lock_violations, 1, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < A20_LWIP_LOCK_SITES; i++) {
        if (__atomic_load_n(&g_lwip_lock_sites[i], __ATOMIC_RELAXED) == site)
            return;
        if (__atomic_load_n(&g_lwip_lock_sites[i], __ATOMIC_RELAXED) == NULL) {
            __atomic_store_n(&g_lwip_lock_sites[i], site, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_lwip_lock_nsites, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}

unsigned a20_lwip_lock_violations(void)
{
    return __atomic_load_n(&g_lwip_lock_violations, __ATOMIC_RELAXED);
}
#endif

/*
 * Drain one netif's receive ring.  `budget` caps how many packets this call
 * processes and 0 means no cap, which is what the IRQ top-half and the
 * scheduler path want: both are the primary reason the ring gets drained.
 *
 * Returns 0 when the budget ran out with packets still queued.  A caller that
 * stops early must leave the RX pending flag set, because the interrupt that
 * would have drained the remainder has already been consumed.
 */
static int a20_lwip_process_netif_rx_tx_locked(struct netif *n, unsigned budget)
{
    if (!n || !n->state)
        return 1;

    a20_lwip_netif_state_t *st = (a20_lwip_netif_state_t *)n->state;
    a20_lwip_sync_link_state(n);

    if (!netif_is_link_up(n)) {
        netif_poll(n);
        return 1;
    }

    int drained = 1;
    unsigned done = 0;
    for (;;) {
        if (budget && done >= budget) {
            drained = 0;
            break;
        }
        int len = st->ops->recv(st->dev, st->rx_frame, sizeof(st->rx_frame));
        if (len <= 0)
            break;
        done++;
        /* recv() was handed sizeof(rx_frame), so this only fires if a driver
         * over-reports; without it an over-report reads past rx_frame below. */
        if ((size_t)len > sizeof(st->rx_frame))
            len = (int)sizeof(st->rx_frame);
        st->rx_packets++;
        st->rx_bytes += (uint64_t)len;
        a20_perf_count(A20_PERF_NET_RX_PACKETS);
        a20_perf_add(A20_PERF_NET_RX_BYTES, (uint64_t)len);
        net_packet_rx_defer((unsigned)netif_get_index(n), st->rx_frame,
                            (size_t)len);
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
        if (!p) {
            LINK_STATS_INC(link.memerr);
            LINK_STATS_INC(link.drop);
            st->rx_dropped++;
            a20_perf_count(A20_PERF_NET_ALLOC_FAIL);
            continue;
        }
        pbuf_take(p, st->rx_frame, (u16_t)len);
        /*
         * Two pbuf_free() sites bracket the n->input() call below, and they
         * are not redundant -- ownership moves at the call:
         *
         *   - Before n->input(): the pbuf is still ours, so the filter's drop
         *     path is the one place we must free it.  The packet never reaches
         *     the IP layer, so no socket is woken and no state is built.
         *   - After n->input(): ethernet_input() has taken ownership and frees
         *     the pbuf itself on its error paths while still returning ERR_OK
         *     (see the "so the caller doesn't have to free it again" note in
         *     lwip ethernet.c), so the caller must not free again.
         */
        if (netfilter_input(st->rx_frame, (size_t)len) == NETFILTER_DROP) {
            pbuf_free(p);
            LINK_STATS_INC(link.drop);
            st->rx_filtered++;
            continue;
        }
        if (n->input(p, n) != ERR_OK) {
            LINK_STATS_INC(link.drop);
            st->rx_dropped++;
        }
    }
    netif_poll(n);
    return drained;
}

/*
 * IRQ top-half entry for a single virtio-net instance.
 * Runs with g_lwip_lock held; performs bounded work only (descriptor ring
 * drainer, lwIP input, no kmalloc, no socket-table bucket lock).
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
            /* Unbounded: this interrupt is the primary reason the ring needs
             * draining, so deferring here would only move the work. */
            a20_lwip_process_netif_rx_tx_locked(n, 0);
            break;
        }
    }
}

/*
 * Timer advance only: no device is touched and no packet is processed, so the
 * critical section stays short enough for the timer-interrupt path that calls
 * it.  kernel_progress_timer_tick() runs on every CPU 0 tick, and it used to
 * reach the receive drain from there, which put up to a ring's worth of
 * protocol processing inside an interrupt with interrupts disabled.
 */
void a20_lwip_poll_timers_locked(void)
{
    if (!g_lwip_ready)
        return;
    sys_check_timeouts();
    a20_net_config_sync_from_lwip();
    /*
     * Drain queued loopback packets here, and not only from a20_lwip_poll().
     * netif_poll() is the sole drain for netif->loop_first, and this tick is
     * the only progress driver that runs regardless of what any task or device
     * is doing -- kernel_progress_poll() and the reader path both reach it by
     * choice, and neither choice is made when a task is parked in connect()
     * waiting for a handshake that only a loopback packet can complete.  Without
     * this the SYN sits in loop_first until the connect timeout expires.
     *
     * Guarded on loop_first so an idle system does no work here beyond the
     * pointer walk, and bounded by LWIP_LOOPBACK_MAX_PBUFS on how much can be
     * released per tick.
     */
    for (struct netif *n = netif_list; n; n = n->next) {
        if (n->loop_first != NULL)
            netif_poll(n);
    }
}

/* Device completions plus the receive drain.  `budget` of 0 means no cap. */
void a20_lwip_poll_rx_locked(unsigned budget)
{
    if (!g_lwip_ready)
        return;
    for (int i = 0; i < A20_NET_MAX_DEVS; i++) {
        a20_lwip_netif_state_t *st = &g_netif_state[i];
        if (st->dev && st->ops && st->ops->poll)
            st->ops->poll(st->dev);
    }
    int complete = 1;
    for (struct netif *n = netif_list; n; n = n->next) {
        if (n->state) {
            if (!a20_lwip_process_netif_rx_tx_locked(n, budget))
                complete = 0;
        } else {
            netif_poll(n);
        }
    }
    if (complete)
        a20_lwip_clear_rx_pending();
}

void a20_lwip_poll_locked(void) {
    a20_lwip_poll_timers_locked();
    a20_lwip_poll_rx_locked(0);
}

void a20_lwip_poll(void) {
    a20_perf_count(A20_PERF_NET_POLL_CALLS);
    uint64_t flags = a20_lwip_lock();
    a20_lwip_poll_locked();
    a20_lwip_unlock(flags);
    net_inet_bottom_half_process_all();
    net_packet_bottom_half_process();
}

/*
 * Poll for a waiter that is only blocked on network progress (a socket read
 * with no data queued).  The g_lwip_lock acquisition is skipped unless some
 * device has actually signalled work, so a blocked reader stops serialising on
 * a global lock to discover there is nothing to do.  This is the same gating
 * virtio_net_poll_rx_all() applies on the scheduler path, and it is safe here
 * because delivery does not run through this call: the IRQ top-half drains the
 * device, and sched() runs the socket bottom-halves (which move bh_ring into
 * the socket queues and wake read_waitq) before picking the next task.  TCP
 * timers likewise advance from kernel_progress_timer_tick() on the timer IRQ.
 *
 * The bottom-halves are NOT gated: they take a socket-table bucket lock rather
 * than g_lwip_lock, and the waiter needs them to drain its own deferred
 * receive data.
 */
void a20_lwip_poll_waiter(void) {
    int need_lock = a20_lwip_rx_pending_any();
    for (int i = 0; i < A20_NET_MAX_DEVS && !need_lock; i++) {
        const a20_lwip_netif_state_t *st = &g_netif_state[i];
        if (!st->dev)
            continue;
        /* A driver that does not report IRQ-driven RX may only be making
         * progress through polling, so it must be drained unconditionally. */
        if (!st->ops || !st->ops->rx_irq_driven ||
            !st->ops->rx_irq_driven(st->dev))
            need_lock = 1;
    }
    if (need_lock) {
        uint64_t flags = a20_lwip_lock();
        a20_lwip_poll_locked();
        a20_lwip_unlock(flags);
    } else {
        a20_perf_count(A20_PERF_NET_POLL_SKIPPED);
    }
    net_inet_bottom_half_process_all();
    net_packet_bottom_half_process();
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
    /* TCP timer firings.  tcp_ticks advances exactly once per tcp_tmr() call, so
     * at TCP_TMR_INTERVAL it directly witnesses how often the TCP timer ran.
     * Reported because the cadence is an invariant a caller can break with no
     * compile error: driving the timer work from a faster path makes
     * retransmission timers expire early and tears down live connections. */
    u32_t tmr_fired = tcp_ticks;

    a20_lwip_unlock(flags);
    if (n < 0)
        return 0;
    if ((size_t)n >= bufsz)
        return (int)bufsz - 1;

    /*
     * Lane occupancy, appended after g_lwip_lock is dropped: sockets live under
     * the socket-table bucket locks.  The drop above is what keeps the order
     * one-way -- the accept
     * path nests net -> lwip, so anything that nests the other way round would
     * deadlock against it.  A gateway / netconf
     * line, not a hot counter -- this exists so that "did the lanes actually
     * spread the connections" is answerable without attaching a debugger, which
     * is the question every later stage depends on.
     */
    unsigned lanes[CONFIG_NET_LANES];
    unsigned total = 0;
    memset(lanes, 0, sizeof(lanes));
    /* One bucket at a time.  The census never needs a second lock -- it reads
     * only `lane`, which is fixed for the life of the socket -- so it does not
     * take the shard set as a whole, which the lock rules forbid. */
    for (int b = 0; b < NET_SOCK_BUCKETS; b++) {
        uint64_t nflags = net_bucket_lock(b);
        int base = b << NET_SOCK_BUCKET_SHIFT;
        for (int k = 0; k < NET_SOCK_SLOTS_PER_BUCKET; k++) {
            net_socket_t *s = g_sockets[base + k];
            if (!s || !net_socket_is_live(s))
                continue;
            unsigned l = s->lane;
            if (l < CONFIG_NET_LANES)
                lanes[l]++;
            total++;
        }
        net_bucket_unlock(b, nflags);
    }

    size_t off = (size_t)n;
    char cell[128];
    snprintf(cell, sizeof(cell),
             "\nlanes: count=%u sockets=%u occupancy:", CONFIG_NET_LANES, total);
    a20_lwip_append(buf, bufsz, &off, cell);
    for (unsigned i = 0; i < CONFIG_NET_LANES; i++) {
        char num[16];
        snprintf(num, sizeof(num), " %u", lanes[i]);
        a20_lwip_append(buf, bufsz, &off, num);
    }
    a20_lwip_append(buf, bufsz, &off, "\n");
    snprintf(cell, sizeof(cell), "\ntcp_ticks: %lu", (unsigned long)tmr_fired);
    a20_lwip_append(buf, bufsz, &off, cell);
#if CONFIG_NET_LOCK_ASSERT
    snprintf(cell, sizeof(cell), "\nlwip_lock: owner=%u violations=%u sites=%u\n",
             g_lwip_lock_owner, a20_lwip_lock_violations(), g_lwip_lock_nsites);
    a20_lwip_append(buf, bufsz, &off, cell);
    for (unsigned i = 0; i < A20_LWIP_LOCK_SITES; i++) {
        void *site = __atomic_load_n(&g_lwip_lock_sites[i], __ATOMIC_RELAXED);
        if (!site)
            break;
        snprintf(cell, sizeof(cell), "lwip_lock_site%u: %lx\n", i,
                 (unsigned long)(uintptr_t)site);
        a20_lwip_append(buf, bufsz, &off, cell);
    }
#else
    /* Say it is off.  Printing violations=0 without that would read as "none
     * found" when it means "nothing was checked". */
    snprintf(cell, sizeof(cell),
             "\nlwip_lock: not checked (CONFIG_NET_LOCK_ASSERT=0)\n");
    a20_lwip_append(buf, bufsz, &off, cell);
#endif
    return (int)off;
}

/* core/printf.c has no '-' flag, so rows are assembled in a local buffer and
 * appended by hand rather than with a single wide snprintf. */
static void a20_lwip_append(char *buf, size_t bufsz, size_t *off,
                            const char *row);
static void a20_lwip_append(char *buf, size_t bufsz, size_t *off,
                            const char *row)
{
    if (*off + 1 >= bufsz)
        return;
    size_t len = strlen(row);
    size_t room = bufsz - *off - 1;
    if (len > room)
        len = room;
    memcpy(buf + *off, row, len);
    *off += len;
    buf[*off] = '\0';
}

/* A netif's identity here is (name[0], name[1], num): lwIP's netif_find parses
 * the number out of name[2] and rejects a name with no digit there, so a bare
 * two-byte name is not a name user space can resolve. */
static void a20_lwip_ifname(char *out, size_t outsz, const struct netif *nif)
{
    if (outsz == 0)
        return;
    snprintf(out, outsz, "%c%c%u", nif->name[0], nif->name[1], nif->num);
}

static void a20_lwip_copy_ip4(uint8_t out[4], const ip4_addr_t *a)
{
    out[0] = ip4_addr1(a);
    out[1] = ip4_addr2(a);
    out[2] = ip4_addr3(a);
    out[3] = ip4_addr4(a);
}

/* /proc/net/route prints addresses as host-order hex. */
static uint32_t a20_ip4_host(const ip4_addr_t *a)
{
    return ((uint32_t)ip4_addr1(a) << 24) | ((uint32_t)ip4_addr2(a) << 16) |
           ((uint32_t)ip4_addr3(a) << 8) | (uint32_t)ip4_addr4(a);
}

/* This stack has no FIB: the only route it holds is each netif's default
 * gateway, so that is all that is reported. */
int a20_lwip_format_route(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return 0;

    uint64_t flags = a20_lwip_lock();
    size_t off = 0;
    char row[192], name[8];

    a20_lwip_append(buf, bufsz, &off,
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\t"
        "Mask\t\tMTU\tWindow\tIRTT\n");
    for (struct netif *n = netif_list; n; n = n->next) {
        uint32_t gw = a20_ip4_host(netif_ip4_gw(n));
        if (gw == 0)
            continue;
        a20_lwip_ifname(name, sizeof(name), n);
        snprintf(row, sizeof(row), "%s\t%08X\t%08X\t%04X\t%d\t%d\t%d\t"
                 "%08X\t%d\t%d\t%d\n",
                 name, 0u, gw, 0x0003u, 0, 0, 0, 0u, 0, 0, 0);
        a20_lwip_append(buf, bufsz, &off, row);
    }
    a20_lwip_unlock(flags);
    return (int)off;
}

/* /proc/net/dev.  Counters come from the per-netif state the RX drain and
 * linkoutput maintain; loopback bypasses the driver path, so its registers
 * legitimately read zero. */
int a20_lwip_format_net_dev(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return 0;

    uint64_t flags = a20_lwip_lock();
    size_t off = 0;
    char row[256], name[8];

    a20_lwip_append(buf, bufsz, &off,
        "Inter-|   Receive                            "
        "          |  Transmit\n"
        " face |bytes    packets errs drop fifo frame "
        "compressed multicast|bytes    packets errs drop fifo "
        "colls carrier compressed\n");
    for (struct netif *n = netif_list; n; n = n->next) {
        const a20_lwip_netif_state_t *st =
            (const a20_lwip_netif_state_t *)n->state;
        a20_lwip_ifname(name, sizeof(name), n);
        snprintf(row, sizeof(row),
                 "%s: %8llu %7llu %4llu %4llu %4llu %4llu %4llu %10llu "
                 "%8llu %8llu %4llu %4llu %4llu %4llu %4llu %6llu\n",
                 name,
                 st ? st->rx_bytes : 0ULL, st ? st->rx_packets : 0ULL,
                 st ? st->rx_errors : 0ULL, st ? st->rx_dropped : 0ULL,
                 0ULL, 0ULL, 0ULL, 0ULL,
                 st ? st->tx_bytes : 0ULL, st ? st->tx_packets : 0ULL,
                 st ? st->tx_errors : 0ULL, 0ULL, 0ULL, 0ULL, 0ULL, 0ULL);
        a20_lwip_append(buf, bufsz, &off, row);

        /* Driver-level counts, when the driver publishes them.  A separate line
         * rather than extra columns because these count what the device moved
         * while the block above counts what lwIP was handed, and the two
         * diverging is how a loss between the ring and the protocol stack gets
         * localized.  The symbol is weak: it is absent, not zero, when the
         * driver is not loaded. */
        if (virtio_net_dev_stats && st && st->dev) {
            net_dev_stats_t ds;
            memset(&ds, 0, sizeof(ds));
            virtio_net_dev_stats(st->dev, &ds);
            snprintf(row, sizeof(row),
                     "%s-drv: rx %llu pkts %llu drops, tx %llu pkts %llu drops\n",
                     name, ds.rx_packets, ds.rx_drops,
                     ds.tx_packets, ds.tx_drops);
            a20_lwip_append(buf, bufsz, &off, row);
        }
    }
    a20_lwip_unlock(flags);
    return (int)off;
}

/* MEMP_STATS is pinned to 1 in lwip_port/lwipopts.h, so desc->stats is read
 * unguarded: with the counters off this stops being a build error.  The
 * #else arm that used to sit here was unreachable -- opt.h already derived
 * MEMP_STATS to 1 -- so it was a stub that no configuration could ever reach,
 * which is worse than no stub at all: it looked like the pool report was
 * conditional on something. */
int a20_lwip_format_memp(char *buf, size_t bufsz)
{
    static const struct { memp_t pool; const char *name; } pools[] = {
        { MEMP_PBUF_POOL,      "PBUF_POOL"      },
        { MEMP_PBUF,           "PBUF"           },
        { MEMP_TCP_SEG,        "TCP_SEG"        },
        { MEMP_TCP_PCB,        "TCP_PCB"        },
        { MEMP_TCP_PCB_LISTEN, "TCP_PCB_LISTEN" },
        { MEMP_UDP_PCB,        "UDP_PCB"        },
    };
    size_t npools = sizeof(pools) / sizeof(pools[0]);
    const size_t NAME_COL = 16;
    char row[128];

    if (!buf || bufsz == 0)
        return 0;

    uint64_t flags = a20_lwip_lock();
    size_t off = 0;

    /*
     * `elem` is lwIP's desc->size, the bytes one element occupies.
     *
     * It deliberately does not report a per-pool capacity.  memp's `avail`
     * used to serve that role and read desc->num, but desc->num only exists in
     * the statically reserved pool layout: with MEMP_MEM_MALLOC=1 memp_init_pool()
     * is an empty stub and `avail` is never written, so printing it yielded a
     * column of silent zeros.  Under MEMP_MEM_MALLOC the pools draw from the
     * lwIP heap instead, so the honest bound is MEM_SIZE rather than a per-pool
     * element count, and the per-pool exhaustion signal is `err`.
     *
     * used, max and err are maintained unconditionally by memp_malloc_pool()
     * and memp_free_pool(), so they stay meaningful in either mode.
     */
    a20_lwip_append(buf, bufsz, &off,
        "pool             elem    used     max    err\n");

    for (size_t i = 0; i < npools; i++) {
        const struct memp_desc *desc = memp_pools[pools[i].pool];
        if (!desc || !desc->stats)
            continue;
        /* The kernel printf has no '-' flag and ignores width for %s, so
         * left-align the name by hand and let width pad only the numbers. */
        size_t nlen = strlen(pools[i].name);
        if (nlen > NAME_COL)
            nlen = NAME_COL;
        char name[NAME_COL + 1];
        memcpy(name, pools[i].name, nlen);
        memset(name + nlen, ' ', NAME_COL - nlen);
        name[NAME_COL] = '\0';

        snprintf(row, sizeof(row), "%s%6lu%7lu%8lu%6lu\n", name,
                 (unsigned long)desc->size,
                 (unsigned long)desc->stats->used,
                 (unsigned long)desc->stats->max,
                 (unsigned long)desc->stats->err);
        a20_lwip_append(buf, bufsz, &off, row);
    }

    /*
     * The two frame-buffer arrays are the part of the stack's footprint that
     * the pool table above structurally cannot show: with MEMP_MEM_MALLOC=1
     * every pool element is a claim on the heap, but these are reserved in
     * .bss whether or not a frame is ever moved.  They were 37312 B on every
     * tier before net_profile.h took them into scope, and no runtime counter
     * reported them -- which is the whole reason the embedded tier could not
     * fit a 20 KiB part while its netmem page looked small.  Printed after the
     * pool rows (not among them) so that gates keying on '^POOLNAME <digits>'
     * are unaffected.
     */
    {
        size_t netif_bytes = sizeof(g_netif_state);
        size_t netif_structs = sizeof(g_netifs);
        size_t packet_bytes = net_packet_static_bytes();
        snprintf(row, sizeof(row),
                 "static .bss (not from the heap): netif_state=%lu netif=%lu "
                 "pkt_ring=%lu total=%lu\n",
                 (unsigned long)netif_bytes,
                 (unsigned long)netif_structs,
                 (unsigned long)packet_bytes,
                 (unsigned long)(netif_bytes + netif_structs + packet_bytes));
        a20_lwip_append(buf, bufsz, &off, row);
    }

    a20_lwip_unlock(flags);
    return (int)off;
}


static struct netif *a20_lwip_netif_by_index(unsigned ifindex)
{
    for (struct netif *n = netif_list; n; n = n->next) {
        if (n->state && (unsigned)netif_get_index(n) == ifindex)
            return n;
    }
    return NULL;
}

int a20_lwip_if_hwaddr(unsigned ifindex, uint8_t out[8])
{
    if (!out)
        return -EINVAL;
    uint64_t flags = a20_lwip_lock();
    struct netif *n = a20_lwip_netif_by_index(ifindex);
    if (!n) {
        a20_lwip_unlock(flags);
        return -ENODEV;
    }
    memcpy(out, n->hwaddr, ETH_ALEN);
    a20_lwip_unlock(flags);
    return 0;
}

int a20_lwip_if_up(unsigned ifindex)
{
    uint64_t flags = a20_lwip_lock();
    struct netif *n = a20_lwip_netif_by_index(ifindex);
    int up;
    if (!n)
        up = -ENODEV;
    else
        up = (netif_is_up(n) && netif_is_link_up(n)) ? 1 : 0;
    a20_lwip_unlock(flags);
    return up;
}

int a20_lwip_if_default_index(void)
{
    uint64_t flags = a20_lwip_lock();
    int idx = netif_default ? (int)netif_get_index(netif_default) : -ENODEV;
    a20_lwip_unlock(flags);
    return idx;
}

/* IFF_UP, the single netif flag user space may drive.  The netlink layer uses
 * the same bit value (see NLRT_IF_FLAGS_UP in kernel/net/socket_netlink.c);
 * carrier is not settable because a20_lwip_sync_link_state() re-derives it
 * from the driver on every poll. */
#define A20_LWIP_IF_F_UP 0x1u

/* These resolve through a20_lwip_netif_by_index(), which matches on device
 * state, so the loopback netif (registered without one) is reported as -ENODEV
 * rather than being reconfigured out from under ARP/loopback. */

/* The single IPv4 slot lwIP keeps per netif (netif.ip_addr).  LWIP_NETIF_API=0
 * means there is no netif_add_ip4_addr(), so a second address is not
 * representable -- but what to do about one is a netlink question, not an lwIP
 * one, so the policy lives with the request that carries it
 * (nlrt_apply_addr(), which sees IFA_LOCAL/IFA_ADDRESS and NLM_F_REPLACE).
 * An all-zero addr clears the slot and takes the netmask and gateway with it:
 * lwIP derives subnet membership from the (addr, netmask) pair, so keeping the
 * netmask would advertise a prefix for an interface that has no address. */
int a20_lwip_if_set_addr(unsigned ifindex, const uint8_t addr[4],
                         const uint8_t mask[4], const uint8_t gw[4])
{
    if (!addr)
        return -EINVAL;
    ip4_addr_t want;
    IP4_ADDR(&want, addr[0], addr[1], addr[2], addr[3]);

    uint64_t flags = a20_lwip_lock();
    struct netif *n = a20_lwip_netif_by_index(ifindex);
    if (!n) {
        a20_lwip_unlock(flags);
        return -ENODEV;
    }
    netif_set_ipaddr(n, &want);
    if (ip4_addr_isany_val(want)) {
        ip4_addr_t zero;
        ip4_addr_set_zero(&zero);
        netif_set_netmask(n, &zero);
        netif_set_gw(n, &zero);
    } else if (mask) {
        ip4_addr_t m;
        IP4_ADDR(&m, mask[0], mask[1], mask[2], mask[3]);
        netif_set_netmask(n, &m);
    } else if (gw) {
        ip4_addr_t g;
        IP4_ADDR(&g, gw[0], gw[1], gw[2], gw[3]);
        netif_set_gw(n, &g);
    }
    a20_lwip_unlock(flags);
    return 0;
}

int a20_lwip_if_get_addr(unsigned ifindex, uint8_t addr[4], uint8_t mask[4],
                         uint8_t gw[4])
{
    if (!addr || !mask || !gw)
        return -EINVAL;
    uint64_t flags = a20_lwip_lock();
    struct netif *n = a20_lwip_netif_by_index(ifindex);
    if (!n) {
        a20_lwip_unlock(flags);
        return -ENODEV;
    }
    a20_lwip_copy_ip4(addr, netif_ip4_addr(n));
    a20_lwip_copy_ip4(mask, netif_ip4_netmask(n));
    a20_lwip_copy_ip4(gw, netif_ip4_gw(n));
    a20_lwip_unlock(flags);
    return 0;
}

int a20_lwip_if_set_mtu(unsigned ifindex, uint16_t mtu)
{
    if (mtu < 68)                  /* RFC 791 minimum link MTU */
        return -EINVAL;
    /*
     * An MTU larger than the profile's frame buffer is not a smaller MTU, it is
     * a broken one: the receive path would truncate every full-size frame to
     * the scratch buffer (silently, at a20_lwip_process_netif_rx_tx_locked()'s
     * clamp) and the transmit path would refuse the frame outright with
     * ERR_BUF.  Bounding it here keeps the invariant the profile asserts --
     * a frame fits one pbuf-pool element -- reachable from userspace too,
     * rather than only at netif_add() time.  The default and server rungs are
     * unaffected: their ceiling is 1536 - ETH_HLEN = 1522, above the historical
     * 1500.
     */
    if (mtu + ETH_HLEN > NET_PROFILE_NETIF_FRAME_SIZE)
        return -EINVAL;
    uint64_t flags = a20_lwip_lock();
    struct netif *n = a20_lwip_netif_by_index(ifindex);
    if (!n) {
        a20_lwip_unlock(flags);
        return -ENODEV;
    }
    n->mtu = mtu;
    a20_lwip_unlock(flags);
    return 0;
}

int a20_lwip_if_set_flags(unsigned ifindex, unsigned flags, unsigned mask)
{
    if (!mask)
        return -EINVAL;
    if (mask & ~(unsigned)A20_LWIP_IF_F_UP)
        return -EOPNOTSUPP;
    uint64_t lf = a20_lwip_lock();
    struct netif *n = a20_lwip_netif_by_index(ifindex);
    if (!n) {
        a20_lwip_unlock(lf);
        return -ENODEV;
    }
    if (flags & A20_LWIP_IF_F_UP)
        netif_set_up(n);
    else
        netif_set_down(n);
    a20_lwip_unlock(lf);
    return 0;
}

int a20_lwip_packet_tx(unsigned ifindex, const uint8_t *frame, size_t len)
{
    if (!frame || len == 0 || len > 0xffff)
        return -EINVAL;
    uint64_t flags = a20_lwip_lock();
    struct netif *n = a20_lwip_netif_by_index(ifindex);
    if (!n || !n->state) {
        a20_lwip_unlock(flags);
        return -ENODEV;
    }
    a20_lwip_netif_state_t *st = (a20_lwip_netif_state_t *)n->state;
    if (!st->dev || !st->ops || !st->ops->send) {
        a20_lwip_unlock(flags);
        return -ENODEV;
    }
    int r = st->ops->send(st->dev, frame, (int)len);
    a20_lwip_unlock(flags);
    return r == (int)len ? (int)len : -EIO;
}
