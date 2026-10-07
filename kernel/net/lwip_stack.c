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
#include "core/panic.h"
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
 *   acquire a net lock.
 * - a20_lwip_poll(): acquires g_lwip_lock, runs progress, releases it, then
 *   runs the socket deferred bottom-half (net_inet_bottom_half_process_all)
 *   under socket locks only.
 *
 * "The two locks are never held together" used to be false here.  Under the
 * single g_net_lock, net_inet_accept_stage_drain() held it and then took
 * a20_lwip_lock() for the pcb handoff, so both were genuinely held together on
 * the accept path.  Two changes removed the reason for it, in that order: the
 * registry sharding made the drain drop the listener's lock across the handoff
 * (it has to, because it also registers each child and
 * net_register_socket_locked() takes a bucket lock of its own), and stage E
 * replaced that lock with a socket lock that covers strictly less.
 *
 * The order is one-way and no path nests them at all: nothing takes
 * g_lwip_lock and then a net lock, so there is no ABBA cycle to form.  A future
 * path that did -- draining a receive ring per lane while touching socket state
 * is the obvious candidate -- has to drop g_lwip_lock first.  See
 * docs/net/network-lock-contract.md and docs/measured/impl-notes-net.md.
 */
static int g_lwip_ready;
static spinlock_t g_lwip_lock = SPINLOCK_INIT;
#define A20_LWIP_LOCK_UNOWNED 0xffffffffu
#define A20_LWIP_LOCK_SITES 8
#if CONFIG_NET_LOCK_ASSERT
static volatile unsigned g_lwip_lock_owner = A20_LWIP_LOCK_UNOWNED;
static unsigned g_lwip_lock_violations;
/*
 * Armed at the end of a20_lwip_init().  Until then the assertion is a no-op on
 * purpose, not because the boot path is exempt by decree: lwIP's own
 * netif_add()/netif_init()/dhcp_start() chain runs inside a20_lwip_init() with
 * no A20OS lock held at all, and the probe measured 23 assertion hits across
 * 8 distinct return addresses there (netif_init, netif_add, lwip_init,
 * netif_add_ip6_address, a20_lwip_init, a20_lwip_loopif_init_cb).  Panicking
 * on those would kill every configuration at boot.  What matters is the claim
 * made about *steady state*, so the assertion starts biting exactly when the
 * stack becomes reachable: after arming, every LWIP_ASSERT_CORE_LOCKED() site
 * must be holding g_lwip_lock, and one that is not is a defect, not noise.
 *
 * Pre-arm hits are therefore *not* counted either: a counter that mixes 23
 * known-init hits with real ones reads as noise and trains everyone to ignore
 * the line.  The exemption rests on the earlier probe's site list, recorded in
 * docs/net/net-lanes.md ("运行时证据（已验证）"); a post-arm violation is the
 * only thing this counter is for, and it panics rather than merely counting.
 */
static volatile int g_lwip_lock_armed;
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
#if CONFIG_NET_LANES > 1
    /* Same reasoning one stage further along.  With more than one lane the
     * device interrupt only stages frames, so a frame already off the device is
     * not yet delivered, and a reader that gated on the device hint alone would
     * skip the drain that would have delivered it.  One relaxed load, and the
     * same false-positive-only argument applies: it is a lower bound on real
     * work. */
    if (net_lane_rx_queued_total() != 0)
        return 1;
#endif
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
/* Capabilities the driver reported at registration time, sampled once.
     * Sampling here rather than per packet is deliberate: a caps() query on
     * every linkoutput() would put a driver call in front of every frame, and
     * the answer can only change across a re-probe, which replaces `ops`
     * wholesale in a20_lwip_register_netifs() anyway. */
    uint32_t caps;
    uint8_t rx_frame[NET_PROFILE_NETIF_FRAME_SIZE];
    uint8_t tx_frame[NET_PROFILE_NETIF_FRAME_SIZE];
    uint64_t rx_packets, rx_bytes, rx_errors, rx_dropped;
    uint64_t tx_packets, tx_bytes, tx_errors;
    uint64_t rx_filtered, tx_filtered;
    uint64_t tx_sg_frames, tx_sg_bytes;
    /* A link state change not yet published to RTNLGRP_LINK.  Set under
     * g_lwip_lock, cleared by a20_lwip_netlink_flush().  See the comment on
     * that function for why the broadcast cannot happen here. */
    uint8_t link_pending;
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
 * header can only approximate it: 128 B is the non-frame part measured on
 * riscv64 LP64, and ILP32 has narrower pointers and counters.  Overstating
 * that term is the safe direction for a ceiling; understating it is not, which
 * is why sizeof() is what the per-profile budget is checked against here.
 */
_Static_assert(sizeof(a20_lwip_netif_state_t) <=
                   NET_PROFILE_NETIF_STATE_BYTES,
               "a20_lwip_netif_state_t exceeds the profile's per-netif state "
               "budget; these are unconditional .bss per registered device");

static a20_lwip_netif_state_t g_netif_state[A20_NET_MAX_DEVS];

/* Bumped whenever some netif's link_pending goes 0 -> 1, so the flush's fast
 * path is one relaxed load instead of a walk of every device. */
static volatile uint32_t g_netif_link_pending;

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
    if (link_up && !netif_is_link_up(netif)) {
        netif_set_link_up(netif);
    } else if (!link_up && netif_is_link_up(netif)) {
        netif_set_link_down(netif);
    } else {
        return;
    }
    /* Only a transition, not every poll: netif_set_link_up() on every pass
     * would flood RTNLGRP_LINK with a message per poll per device.  The event
     * is queued here and published by a20_lwip_netlink_flush(), which is
     * reachable from the same poll path. */
    st->link_pending = 1;
    __atomic_store_n(&g_netif_link_pending, 1, __ATOMIC_RELAXED);
}

/*
 * Publish link-state changes collected under g_lwip_lock to RTNLGRP_LINK.
 *
 * Called with g_lwip_lock NOT held.  That is the whole reason the change is
 * not broadcast from a20_lwip_sync_link_state() directly: net_netlink_link_notify()
 * walks the socket table under net locks, and the lock contract in
 * docs/net/network-lock-contract.md forbids holding g_lwip_lock together with a
 * net lock.  So the event is recorded under the lwIP lock, collected here,
 * and the lwIP lock is dropped before any bucket is touched.
 *
 * Events are collected under the lock so the netif cannot be freed or re-registered
 * between noticing the change and naming the interface.  Coalescing is by
 * device: two flips between two flushes publish only the settled state, which
 * is what a listener wants and what it would have to do the work of deriving
 * itself.
 */
static void a20_lwip_netlink_flush(void)
{
    if (!__atomic_load_n(&g_netif_link_pending, __ATOMIC_RELAXED))
        return;

    nlrt_link_event_t events[A20_NET_MAX_DEVS];
    int n = 0;
    uint64_t lf = a20_lwip_lock();
    for (int i = 0; i < A20_NET_MAX_DEVS; i++) {
        a20_lwip_netif_state_t *st = &g_netif_state[i];
        if (!st->dev || !st->link_pending)
            continue;
        st->link_pending = 0;
        struct netif *nif = &g_netifs[i];
        events[n].index = (uint32_t)netif_get_index(nif);
        events[n].want_up = netif_is_link_up(nif) ? 1 : 0;
        n++;
    }
    __atomic_store_n(&g_netif_link_pending, 0, __ATOMIC_RELAXED);
    a20_lwip_unlock(lf);

    if (n > 0)
        net_netlink_link_notify(events, n);
}

/*
 * The IPv4 address of one netif, in host order, or 0 if it has none yet.  A
 * negative index means "whichever netif has an address", which is what a NAT
 * rule installed before DHCP completes wants: translating with the wrong
 * interface's address is worse than not translating at all, but translating
 * with the only address the host has is usually right.
 *
 * Callers hold g_lwip_lock -- netfilter's MASQUERADE runs from a packet hook,
 * which is under it by construction.
 */
static uint32_t a20_netif_ipv4_host(const struct netif *n);

uint32_t a20_lwip_netif_ipv4(int net_idx)
{
    if (net_idx < 0 || net_idx >= A20_NET_MAX_DEVS) {
        for (int i = 0; i < A20_NET_MAX_DEVS; i++) {
            uint32_t a = a20_netif_ipv4_host(&g_netifs[i]);
            if (a)
                return a;
        }
        return 0;
    }
    return a20_netif_ipv4_host(&g_netifs[net_idx]);
}

/* ip_addr_t is a union over both IP versions here (LWIP_IPV6 is on), so the v4
 * member has to be selected before it can be read; ip4_addr_get_u32() yields
 * network byte order and every netfilter address is host order. */
static uint32_t a20_netif_ipv4_host(const struct netif *n)
{
    const ip4_addr_t *v4;
    if (!n)
        return 0;
    v4 = &n->ip_addr.u_addr.ip4;
    if (ip4_addr_isany_val(*v4))
        return 0;
    return lwip_ntohl(ip4_addr_get_u32(v4));
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

int r;

    /*
     * Scatter-gather transmit, taken only when the driver advertised
     * NET_DEV_CAP_TX_SG *and* the chain is already one contiguous segment.
     *
     * Both conditions are load bearing:
     *
     *  - The capability is what says the driver implements send_sg().  A
     *    driver that leaves the callback NULL reports no bit, and lands in the
     *    staging-copy path below with byte-identical behaviour -- the same
     *    pbuf_copy_partial() into the same tx_frame, the same netfilter call on
     *    the same bytes, the same counters.
     *
     *  - Single-segment only.  netfilter_output() takes a flat buffer and
     *    parses Ethernet/VLAN/IPv4/L4 out of it, so a multi-segment chain has
     *    to be linearised before the hook can run.  A PBUF_POOL head element
     *    is contiguous, so the one-segment case needs no chain walk at all;
     *    anything longer still takes the staging buffer.  Growing the filter
     *    to walk a pbuf chain is a netfilter.c change and out of scope here.
     *
     * The netfilter hook sits between the staging copy and the transmit, and
     * that is the only ordering that works: before the copy and the rewrite
     * is discarded, after the send and it is pointless.
     *
     * This path stages like the other one and so gives up its zero-copy
     * property.  That is forced, not incidental.  The hook takes `uint8_t *`
     * because NAT is an in-place rewrite of the frame the caller already holds
     * (netfilter_nat.c), so the bytes it is given have to be writable.  The
     * pbuf payload cannot serve that: send_sg takes `const net_iovec_t *`, and
     * the payload is lwIP pool memory that another holder may also be looking
     * at, so SNAT-ing it in place would corrupt the pool rather than this
     * socket's view of the wire.  The driver still gets the scatter-gather
     * transmit it asked for -- it reads the descriptor instead of taking a
     * second copy into driver-internal memory -- it just reads it from
     * tx_frame rather than from the pbuf.
     *
     * The loopback netif never reaches this function (netif_loop_output()
     * bypasses linkoutput entirely), so the fast path cannot affect it.
     */
    if ((st->caps & NET_DEV_CAP_TX_SG) && st->ops->send_sg && p->next == NULL) {
        net_iovec_t iov;
        pbuf_copy_partial(p, st->tx_frame, p->tot_len, 0);
        if (netfilter_output(st->tx_frame, p->tot_len, st->idx) ==
            NETFILTER_DROP) {
            st->tx_filtered++;
            return ERR_OK;
        }
        iov.base = st->tx_frame;
        iov.len = p->tot_len;
        r = st->ops->send_sg(st->dev, &iov, 1);
        if (r == (int)p->tot_len) {
            st->tx_sg_frames++;
            st->tx_sg_bytes += p->tot_len;
        }
    } else {
        pbuf_copy_partial(p, st->tx_frame, p->tot_len, 0);
        if (netfilter_output(st->tx_frame, p->tot_len, st->idx) ==
            NETFILTER_DROP) {
            st->tx_filtered++;
            return ERR_OK;
        }
        r = st->ops->send(st->dev, st->tx_frame, p->tot_len);
    }

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
        g_netif_state[i].caps = ops->caps ? ops->caps(dev) : 0;
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
    /* Before lwip_init(), so no packet can meet a half-built conntrack table. */
    netfilter_conntrack_init();
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
#if CONFIG_NET_LOCK_ASSERT
    /* Past this point every lwIP entry point that carries an assertion must be
     * reached with g_lwip_lock held.  See g_lwip_lock_armed. */
    __atomic_store_n(&g_lwip_lock_armed, 1, __ATOMIC_RELEASE);
#endif
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
    /*
     * The lane context ends with the critical section.  Resetting it here is
     * what makes "lane 0" the value a section gets when it never establishes
     * one of its own, instead of inheriting whatever lane the previous holder
     * of this lock happened to be working on.  Folding to nothing at one lane
     * is what net_lane_ctx_pop(0) is for.
     */
    net_lane_ctx_pop(0);
    spin_unlock_irqrestore(&g_lwip_lock, flags);
}

/*
 * Stage C: at CONFIG_NET_LANES == 1 the declaration of a lane is a macro that
 * expands to nothing, in lwip_stack.h -- see the note there for why it has to
 * vanish from the preprocessed source and not merely fold away.
 *
 * What this file owns is the three halves that cannot fold: the definition the
 * macro stands for at more than one lane, the reset on unlock, and the answer
 * lwIP asks through the LWIP_MEMP_LANE() hook.
 */
#if CONFIG_NET_LANES > 1
/*
 * Declare which lane owns the work that is about to run inside the lwIP core.
 * lwIP's allocator has no lane parameter, so memp asks the port
 * (LWIP_MEMP_LANE -> a20_lwip_memp_lane below) which pool to index.
 *
 * Call this only with g_lwip_lock held, and only at a top-level entry: there is
 * no scoped push/pop here because a20_lwip_unlock() clears the context, so a
 * previous value has nothing to restore to.  The one caller that genuinely
 * nests -- a20_lwip_process_netif_rx_tx_locked(), reached from a socket's
 * a20_lwip_poll_locked() -- uses net_lane_ctx_push()/pop() directly so the
 * socket's lane survives the drain.
 */
void a20_lwip_lane_enter(unsigned lane)
{
    (void)net_lane_ctx_push(lane);
}

/*
 * The port's answer to "which lane is this?", handed to lwIP through the
 * LWIP_MEMP_LANE() hook in lwipopts.h.  Declared there rather than here
 * because lwipopts.h is read before any kernel header.
 *
 * It reads the context rather than hashing anything itself.  A second
 * derivation of "the lane" inside memp is exactly the hazard net-lane.h
 * describes: the PCBs of one connection and the pbufs of the same connection
 * must agree on one lane, and they can only do that if there is exactly one
 * definition of it.
 */
unsigned a20_lwip_memp_lane(void)
{
    return net_lane_ctx_get();
}

/*
 * Which lane owns the receive drain of one netif.
 *
 * A netif has no lane of its own -- upstream's struct netif is unmodified, see
 * DIVERGENCE.md 2.4 -- so the address is what there is to hash, and it is the
 * same reasoning net_lane_of_ip() already applies to ARP, ICMP and NDP: state
 * with no port to key on is spread by address rather than left to land on
 * whichever CPU took the interrupt.  A netif that has no address yet hashes
 * 0.0.0.0, which is deterministic, so the boot-time drain is not a special
 * case.
 *
 * This is NOT the packet's owning lane.  That one is only known once the
 * packet has been parsed far enough to find the connection, which is stage D;
 * until then, packets for every lane's connections arrive through the same
 * netif and are drained in its lane.
 */
static unsigned a20_lwip_netif_lane(const struct netif *n)
{
    return net_lane_of_ip(a20_netif_ipv4_host(n));
}
#endif /* CONFIG_NET_LANES > 1 */

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

/*
 * The whole point of the lock contract is that it is executable, not prose.
 * lwIP ships LWIP_ASSERT_CORE_LOCKED() as an empty macro
 * (src/include/lwip/opt.h:227), so every one of the ~50 sites in tcp.c,
 * tcp_in.c, raw.c, udp.c, dns.c and ethernet.c expanded to nothing and a path
 * that mutated PCB lists without g_lwip_lock corrupted them silently --
 * exactly the failure net-lanes.md had to chase through assembly and UBSan
 * descriptors.  lwipopts.h maps the macro here; this is the mapping.
 *
 * `site` is the return address taken at the macro's expansion point, so it
 * identifies the lwIP function that ran unlocked rather than this function.
 * It is recorded before the panic because the backtrace of a panic in a kernel
 * built without a line table is only as good as the return address it prints.
 */
void a20_lwip_assert_core_locked(void *site)
{
    if (g_lwip_lock_owner == cpu_current_id())
        return;
    /* Pre-arm: boot-time lwIP construction, which by construction holds no
     * A20OS lock.  Counted (that count is the evidence for the exemption),
     * not fatal. */
    if (!__atomic_load_n(&g_lwip_lock_armed, __ATOMIC_ACQUIRE))
        return;
    a20_lwip_note_lock_violation(site);
    panic("lwIP core unlocked: site=%lx owner=%u cpu=%u violations=%u",
          (unsigned long)(uintptr_t)site, g_lwip_lock_owner,
          cpu_current_id(), a20_lwip_lock_violations());
}
#endif

#if CONFIG_NET_LANES > 1
/*
 * STAGE D: the receive split.
 *
 * Everything in this block exists only on a multi-lane build.  At one lane
 * a20_lwip_process_netif_rx_tx_locked() below still reads a frame and hands it
 * straight to n->input() under g_lwip_lock, which is what it always did, and
 * this whole arrangement is absent from the preprocessed source.  That is not
 * tidiness: net-lanes.md's rule is that the one-lane build has to *be* the
 * pre-lane build, so the duplicated receive prologue below is deliberate.
 */

/* Spelled out rather than taken from lwip/sockets.h, which this file does not
 * include and should not: the only thing wanted from it is these two numbers,
 * and the frame parser below must agree with what ip4_input() will decide the
 * protocol is. */
#define A20_IPPROTO_TCP 6
#define A20_IPPROTO_UDP 17

/* Which lane owns the connection this frame belongs to.
 *
 * net_lane_of(dst_ip, dst_port), and the two halves have to be the same halves
 * two other places use, or a packet lands on a lane that does not own its pcb:
 *
 *   - lwIP looks the pcb up in the bucket NET_PCB_LANE_OF(ip_current_dest_addr(),
 *     hdr->dest), i.e. exactly this pair (tcp_in.c, udp.c);
 *   - a socket's own lane is net_socket_lane_of_addr() on its bound address,
 *     and for an established connection the bound address IS this frame's
 *     destination.
 *
 * An inbound frame's destination address and port are our local address and
 * port, so the value is computable from wire bytes alone, on a CPU that has
 * never seen the connection.  That is the same property the ownership hash has
 * always had; stage D is the first caller that has to evaluate it on every
 * packet, from outside lwIP.
 *
 * Both values are read out of the frame in network byte order and fed to the
 * hash in network byte order, which is what net_pcb_lane_ip() and the tcphdr /
 * udphdr port fields give lwIP.  Byte-swapping here would make every packet
 * land on the wrong lane while still looking self-consistent.
 *
 * Three classes deliberately do not use the port:
 *
 *   - a fragment.  Only the first fragment of a datagram carries the transport
 *     header, so reading a "port" out of a later one reads payload.  Every
 *     fragment of a datagram satisfies (MF || frag_offset), so all of them take
 *     the address-only hash and land on one lane, and reassembly cannot be split
 *     across two CPUs.
 *   - an IPv4 header whose own length field is nonsense, and any frame too short
 *     to hold what is being read.  Those go to the netif's own lane, which is
 *     where every packet went before stage D; the frame is about to be dropped
 *     by ethernet_input() regardless, and reading past the end to decide which
 *     lane it belongs to would be a bug in the code that is supposed to be the
 *     safe one.
 *   - everything that is not IPv4/IPv6 carrying TCP or UDP, which is hashed by
 *     address for the reason net_lane_of_ip() gives: ARP, ICMP and NDP have no
 *     port, and there is no PCB for an inbound packet of that shape to miss.
 *
 * One VLAN tag is skipped.  A second one is not, and falls into the last case
 * above: the address is still the one the frame is really for, it just does not
 * get the port.
 */
static unsigned a20_lwip_frame_lane(const struct netif *n, const uint8_t *f,
                                    unsigned len)
{
    unsigned l3 = ETH_HLEN;
    uint16_t ethertype;

    if (len < l3 + 2)
        return a20_lwip_netif_lane(n);
    ethertype = (uint16_t)(((uint16_t)f[l3 - 2] << 8) | f[l3 - 1]);
    if (ethertype == ETHTYPE_VLAN) {
        l3 += 4;
        if (len < l3 + 2)
            return a20_lwip_netif_lane(n);
        ethertype = (uint16_t)(((uint16_t)f[l3 - 2] << 8) | f[l3 - 1]);
    }

    if (ethertype == ETHTYPE_IP) {
        unsigned ihl;
        uint16_t frag;
        uint32_t dst;
        if (len < l3 + 20)
            return a20_lwip_netif_lane(n);
        ihl = (unsigned)(f[l3] & 0x0f) * 4;
        if (ihl < 20 || len < l3 + ihl)
            return a20_lwip_netif_lane(n);
        memcpy(&dst, f + l3 + 16, sizeof(dst));
        frag = (uint16_t)(((uint16_t)f[l3 + 6] << 8) | f[l3 + 7]);
        /* MF is the low bit of the flags half-word; the offset is the top 13
         * bits of the same field.  Either set means this is one fragment of a
         * datagram that was split, which is the condition that has to hold for
         * every fragment of it. */
        if (frag & 0x2000u || (frag & 0x1fffu) != 0)
            return net_lane_of_ip(dst);
        if (len < l3 + ihl + 4)
            return net_lane_of_ip(dst);
        if (f[l3 + 9] == A20_IPPROTO_TCP || f[l3 + 9] == A20_IPPROTO_UDP) {
            uint16_t dport;
            memcpy(&dport, f + l3 + ihl + 2, sizeof(dport));
            return net_lane_of(dst, dport);
        }
        return net_lane_of_ip(dst);
    }

    if (ethertype == ETHTYPE_IPV6) {
        uint8_t nexthdr;
        uint32_t dst;
        if (len < l3 + 40)
            return a20_lwip_netif_lane(n);
        nexthdr = f[l3 + 6];
        /* The low 32 bits, which is what net_pcb_lane_ip() takes for v6 and
         * what net_socket_lane_of_addr() takes from a sockaddr_in6. */
        memcpy(&dst, f + l3 + 24 + 12, sizeof(dst));
        /* No extension-header walk: an extension header means the transport
         * header is not where it would be, so this falls to the address-only
         * hash rather than reading a port out of an extension header.  Every
         * fragment of a v6 datagram is on this path too, since the fragment
         * header is itself an extension header. */
        if (nexthdr == A20_IPPROTO_TCP || nexthdr == A20_IPPROTO_UDP) {
            uint16_t dport;
            if (len < l3 + 40 + 4)
                return net_lane_of_ip(dst);
            memcpy(&dport, f + l3 + 40 + 2, sizeof(dport));
            return net_lane_of(dst, dport);
        }
        return net_lane_of_ip(dst);
    }

    if (ethertype == ETHTYPE_ARP) {
        /* The target protocol address is the address being resolved, i.e. the
         * one this frame is about.  That is the same value net_lane_of_ip()
         * hashes for every other portless protocol, so ARP lands with the rest
         * of the traffic for its address rather than in a bucket of its own.
         * Fixed layout: htype(2) ptype(2) hlen(1) plen(1) oper(2) sha(6) spa(4)
         * tha(6) tpa(4), all of it IPv4-over-Ethernet on this link by
         * definition of the ethertype we matched. */
        uint32_t tpa;
        if (len < l3 + 38)
            return a20_lwip_netif_lane(n);
        memcpy(&tpa, f + l3 + 24 + 14, sizeof(tpa));
        return net_lane_of_ip(tpa);
    }

    return a20_lwip_netif_lane(n);
}

/*
 * Read every frame the device has and put each one on its lane's queue.
 * Requires g_lwip_lock, which is what serialises this against a device
 * interrupt landing on another CPU.  Returns 0 when the budget ran out with
 * frames still queued, with the same "leave the RX pending flag set" contract
 * the inline drain has.
 */
static int a20_lwip_rx_enqueue_locked(struct netif *n, unsigned budget)
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
        if ((size_t)len > sizeof(st->rx_frame))
            len = (int)sizeof(st->rx_frame);
        st->rx_packets++;
        st->rx_bytes += (uint64_t)len;
        a20_perf_count(A20_PERF_NET_RX_PACKETS);
        a20_perf_add(A20_PERF_NET_RX_BYTES, (uint64_t)len);
        net_packet_rx_defer((unsigned)netif_get_index(n), st->rx_frame,
                            (size_t)len);
        /* The filter still runs here, on rx_frame, before the lane is worked
         * out -- and that ordering is load-bearing for the same reason it was
         * before the pbuf existed: a DNAT rewrites the destination in place, so
         * a lane derived before the rewrite would be the lane of the address
         * the packet used to have.  Running it after the enqueue would make the
         * dispatch disagree with the lookup in udp_input(), which reads the
         * post-NAT destination. */
        if (netfilter_input(st->rx_frame, (size_t)len, st->idx) ==
            NETFILTER_DROP) {
            LINK_STATS_INC(link.drop);
            st->rx_filtered++;
            continue;
        }
        unsigned lane = a20_lwip_frame_lane(n, st->rx_frame, (unsigned)len);
        if (!net_lane_rx_put(lane, n, st->rx_frame, (unsigned)len)) {
            /* Counted twice on purpose, at two different granularities: the
             * netif counts a frame it could not hand on, the lane counts a queue
             * that was full, and only the second one says which lane's consumer
             * failed to keep up.  No perf event for this -- a staged-frame drop
             * is not the bottom-half overflow the existing counter describes,
             * and reusing that one would make an unrelated counter move. */
            LINK_STATS_INC(link.drop);
            st->rx_dropped++;
        }
    }
    /* Loopback is not staged.  netif_poll() already unlinks the pbuf from
     * loop_first under SYS_ARCH_PROTECT and processes it in place, so there is
     * no window to close by deferring it, and turning it into a staged frame
     * would mean freeing a pbuf and re-allocating one to say the same thing. */
    netif_poll(n);
    return drained;
}

/* Hand one staged frame to netif input.  Requires g_lwip_lock and the lane's
 * current-lane context already declared by the caller. */
static void a20_lwip_lane_input_one(unsigned lane, struct netif *n,
                                    const uint8_t *frame, unsigned len)
{
    if (!n || !n->state || !n->input)
        return;
    a20_lwip_netif_state_t *st = (a20_lwip_netif_state_t *)n->state;

    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (!p) {
        LINK_STATS_INC(link.memerr);
        LINK_STATS_INC(link.drop);
        st->rx_dropped++;
        a20_perf_count(A20_PERF_NET_ALLOC_FAIL);
        return;
    }
    pbuf_take(p, frame, (u16_t)len);
    /* One pbuf_free() call site brackets the n->input() call below, and it is
     * not redundant -- ownership moves at the call.  After ethernet_input() has
     * taken ownership it frees the pbuf itself on its error paths while still
     * returning ERR_OK, so the caller must not free again. */
    if (n->input(p, n) != ERR_OK) {
        LINK_STATS_INC(link.drop);
        st->rx_dropped++;
    }
    net_lane_rx_count_processed(lane);
}

/*
 * Drain one lane's staged frames.  `budget` of 0 means no cap.
 *
 * THE ORDERING RULES, because both of them are easy to get wrong:
 *
 *   - the lane is claimed before g_lwip_lock, never the other way round.  The
 *     claim is only ever taken from process context and g_lwip_lock is taken
 *     from interrupt context, so the reverse order is an inversion with a real
 *     deadlock behind it: an interrupt on this CPU would wait for g_lwip_lock
 *     while this CPU holds it and spins for the claim.  Nothing takes a lane
 *     claim under g_lwip_lock, so the pair is a strict order, not a cycle.
 *   - the emptiness test happens before the lock, while the claim is held.  A
 *     producer only ever adds, so a lane found non-empty cannot become empty
 *     under us, and the test saves taking the core lock for nothing on the
 *     overwhelmingly common "no traffic" pass.
 *
 * The claim is given back before returning, including on the early return, so
 * a lane is never left unclaimable.
 */
static unsigned a20_lwip_lane_drain_locked(unsigned lane, unsigned budget)
{
    if (!net_lane_rx_claim(lane))
        return 0;
    unsigned done = 0;
    if (net_lane_rx_ready(lane)) {
        uint64_t flags = a20_lwip_lock();
        /* Declare this lane for the whole batch: every pbuf the stack allocates
         * here belongs to the connections whose packets these are, which is
         * what makes stage C's per-lane pool table mean something.  Popped and
         * restored rather than assigned, because the caller may have arrived
         * with a lane of its own -- a socket's send path reaches the receive
         * drain through a20_lwip_poll_locked() -- and giving it up for the
         * duration would charge this traffic to the wrong pool. */
        unsigned prev_lane = net_lane_ctx_push(lane);
        for (;;) {
            if (budget && done >= budget)
                break;
            const uint8_t *frame;
            unsigned len;
            struct netif *n = net_lane_rx_pop(lane, &frame, &len);
            if (!n)
                break;
            done++;
            a20_lwip_lane_input_one(lane, n, frame, len);
        }
        net_lane_ctx_pop(prev_lane);
        a20_lwip_unlock(flags);
    }
    net_lane_rx_release(lane);
    return done;
}

/* Drain every lane that will hand itself over.  `budget` caps each lane, not
 * the total, because the point of the stage is that four CPUs can each be
 * inside a different lane at the same time. */
static unsigned a20_lwip_lane_drain_all(unsigned budget)
{
    unsigned done = 0;
    for (unsigned lane = 0; lane < CONFIG_NET_LANES; lane++)
        done += a20_lwip_lane_drain_locked(lane, budget);
    return done;
}
#endif /* CONFIG_NET_LANES > 1 */

/*
 * Drain one netif's receive ring.  `budget` caps how many packets this call
 * processes and 0 means no cap, which is what the IRQ top-half and the
 * scheduler path want: both are the primary reason the ring gets drained.
 *
 * Returns 0 when the budget ran out with packets still queued.  A caller that
 * stops early must leave the RX pending flag set, because the interrupt that
 * would have drained the remainder has already been consumed.
 *
 * AT MORE THAN ONE LANE THIS IS SPLIT IN TWO, and the split is the whole of
 * stage D.  The code below is the one-lane version and is compiled unchanged;
 * a20_lwip_rx_enqueue_locked() and a20_lwip_lane_drain_locked() above it are
 * what a multi-lane build runs instead:
 *
 *   - enqueue copies each frame into the queue of the lane that owns the
 *     connection it belongs to, and returns.  No pbuf, no protocol stack, no
 *     allocation: the interrupt path's per-packet cost becomes a bounded memcpy.
 *   - processing pops a lane's queue and runs netif input on it, with that lane
 *     declared as the current one, so lwIP's allocator and the pcb buckets it
 *     touches belong to the connection's own lane.
 *
 * Two callers, two shapes.  a20_lwip_process_netif_irq_locked() enqueues only,
 * so the interrupt does no protocol work; every other caller enqueues and then
 * drains inline, because those callers arrived to get packets delivered (a
 * blocked reader, a socket's send path, the timer-tick safety net) and a poll
 * point elsewhere is not a substitute for the thing they asked for.
 *
 * WHICH MEANS THIS FUNCTION IS THE ONE-LANE VERSION ONLY, and is compiled out
 * above one lane.  That is not an omission: with the split in place nothing
 * calls it, and leaving it there would be a second, dead, subtly different
 * receive path for a reader to find.  Its loopback drain, its link-state sync
 * and its per-frame accounting all still exist -- inside
 * a20_lwip_rx_enqueue_locked(), which is the same code doing the same things in
 * the same order, minus the n->input() call.
 */
#if CONFIG_NET_LANES == 1
static int a20_lwip_process_netif_rx_tx_locked(struct netif *n, unsigned budget)
{
    if (!n || !n->state)
        return 1;

    /*
     * Scoped, not a plain assignment: this runs under a caller's lane too --
     * a socket's send path reaches it through a20_lwip_poll_locked() -- and the
     * caller has no reason to give up its lane for the drain.  Restoring on the
     * way out is what keeps a loopback echo released here from being charged to
     * the netif instead of to the socket that will consume it.
     *
     * The whole statement, not just the helper, is what the N=1 build drops.
     * Leaving `net_lane_ctx_push(a20_lwip_netif_lane(n))` to fold away on its
     * own would be a bet on -O3 seeing through an inlined static, and the N=1
     * equivalence rule is not a bet.  At one lane the drain's lane is 0, which
     * is also what net_lane_ctx_push() returns, so `prev_lane = 0` is the same
     * value by construction rather than by optimisation.
     */
#if CONFIG_NET_LANES > 1
    unsigned prev_lane = net_lane_ctx_push(a20_lwip_netif_lane(n));
#else
    unsigned prev_lane = 0;
#endif

    a20_lwip_netif_state_t *st = (a20_lwip_netif_state_t *)n->state;
    a20_lwip_sync_link_state(n);

    if (!netif_is_link_up(n)) {
        netif_poll(n);
        net_lane_ctx_pop(prev_lane);
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
        /*
         * The filter runs on rx_frame, before the pbuf is filled from it, and
         * that ordering is load-bearing rather than incidental.  A DNAT rewrites
         * the destination in place; if the hook ran after pbuf_take() the copy
         * lwIP would go on to parse would still hold the pre-NAT destination and
         * the translation would silently do nothing -- the rule would report a
         * match and the connection would never be made.  Running first also
         * means the drop path has no pbuf to release.
         */
        if (netfilter_input(st->rx_frame, (size_t)len, st->idx) ==
            NETFILTER_DROP) {
            LINK_STATS_INC(link.drop);
            st->rx_filtered++;
            continue;
        }
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
         * One pbuf_free() call site brackets the n->input() call below, and it
         * is not redundant -- ownership moves at the call.  After
         * ethernet_input() has taken ownership it frees the pbuf itself on its
         * error paths while still returning ERR_OK (see the "so the caller
         * doesn't have to free it again" note in lwip ethernet.c), so the
         * caller must not free again.  The filter's drop path no longer
         * appears here at all: it runs before the pbuf exists.
         */
        if (n->input(p, n) != ERR_OK) {
            LINK_STATS_INC(link.drop);
            st->rx_dropped++;
        }
    }
    netif_poll(n);
    net_lane_ctx_pop(prev_lane);
    return drained;
}
#endif /* CONFIG_NET_LANES == 1 */

/*
 * IRQ top-half entry for a single virtio-net instance.
 * Runs with g_lwip_lock held; performs bounded work only (descriptor ring
 * drainer, lwIP input, no kmalloc, no net lock).
 *
 * AT MORE THAN ONE LANE IT DOES NOT RUN THE PROTOCOL STACK.  It reads each
 * frame out of the device ring and stages it on its owning lane's queue, and
 * returns; the processing happens at a poll point that runs outside interrupt
 * context.  That is the entire reason for stage D, and it is also the reason
 * this is not simply "a20_lwip_poll_rx_locked(0)": an interrupt that runs the
 * stack still runs it with interrupts disabled, on whatever CPU took the
 * interrupt, holding the one global lock for the whole burst.
 *
 * Unbounded enqueue, matching what this function did before the split: this
 * interrupt is the primary reason the ring needs draining, so bounding it would
 * only move the work.  What bounds the *processing* is the lane queue depth and
 * the poll point's budget; a frame that finds its lane full is dropped and
 * counted, which for TCP means a retransmission.
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
#if CONFIG_NET_LANES > 1
            a20_lwip_rx_enqueue_locked(n, 0);
#else
            /* Unbounded: this interrupt is the primary reason the ring needs
             * draining, so deferring here would only move the work. */
            a20_lwip_process_netif_rx_tx_locked(n, 0);
#endif
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
     *
     * The lane context is deliberately left as the caller established it,
     * rather than being reset to the netif's own lane the way the receive
     * drain does.  loop_first is one FIFO holding packets from every lane's
     * connections, and which of them a given netif_poll() releases is not
     * knowable before the packet is parsed, so there is no per-netif answer to
     * adopt.  Inheriting at least keeps a socket's own loopback echo in that
     * socket's lane, because a socket reaches this through
     * a20_lwip_poll_locked() with its lane already set.  From the timer tick
     * it is lane 0, which a20_lwip_unlock() established.
     */
    for (struct netif *n = netif_list; n; n = n->next) {
        if (n->loop_first != NULL)
            netif_poll(n);
    }

    /*
     * Conntrack idle-timeout sweep.  This is the timers segment precisely
     * because it is the one progress driver that runs unconditionally: a task
     * parked in connect() makes no a20_lwip_poll_* call of its own, so
     * anything that must happen on a wall clock can only live here.  The scan
     * is bounded and resumes where it stopped (see netfilter_conntrack_expire),
     * and it is gated on a one-second interval so an idle system does not walk
     * the table on every tick.
     */
    static uint64_t g_ct_sweep_at;
    uint64_t now_ticks = timer_get_ticks();
    if (now_ticks >= g_ct_sweep_at) {
        g_ct_sweep_at = now_ticks + (TICKS_PER_SEC ? TICKS_PER_SEC : 1000);
        netfilter_conntrack_expire(32);
    }
}

/* Device completions plus the receive drain.  `budget` of 0 means no cap.
 *
 * More than one lane: the device ring is staged onto the owning lanes and then
 * drained here, in the same call and under the same lock, because every caller
 * of this function arrived to get packets delivered -- the reader path, the
 * socket send path, the timer-interrupt safety net.  Asking them to come back
 * later would be a liveness regression for exactly the case the function
 * exists for.  The interrupt is the caller that does *not* drain, because its
 * job is to get off the CPU.
 */
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
#if CONFIG_NET_LANES > 1
            if (!a20_lwip_rx_enqueue_locked(n, budget))
                complete = 0;
#else
            if (!a20_lwip_process_netif_rx_tx_locked(n, budget))
                complete = 0;
#endif
        } else {
            /* No state means a netif the port does not drive, so there is no
             * address to hash and no per-netif lane to adopt.  It still has to
             * be drained, so it runs in whatever lane the caller set. */
            netif_poll(n);
        }
    }
#if CONFIG_NET_LANES > 1
    a20_lwip_lane_drain_all(budget);
    /* "Complete" has to mean the queues are empty too.  Clearing the RX pending
     * flag with frames still staged would let the reader path skip a drain it
     * should have made, and the frames would sit until the next interrupt --
     * which, since this interrupt is what staged them, may not come. */
    if (net_lane_rx_queued_total() != 0)
        complete = 0;
#endif
    if (complete)
        a20_lwip_clear_rx_pending();
}

/*
 * The stage D poll point.  Called from kernel_progress_run_bottom_halves(),
 * which sched() runs on every scheduling decision and every idle pass, on every
 * CPU.
 *
 * WHY IT IS CALLED FROM THERE AND NOT FROM WHEREVER THE READER IS.  This is the
 * red-flagged trap in net-lanes.md, and the deadlock it describes is real: if
 * the only thing that ran the receive queues were a reader waking up, then a
 * blocked reader waits for a wake-up that only the receive path can produce,
 * while the receive path waits for a reader.  A blocked read is woken by the
 * socket bottom half, the bottom half needs the staged frames already gone, and
 * a poll that only runs after the wake-up is waiting for an event that will
 * never arrive.  The fix is not a cleverer gate, it is a poll point that runs
 * unconditionally: sched() is reached on every context switch, on every timer
 * tick that reschedules, and on every idle pass, whether or not any task is
 * blocked on the network at all.
 *
 * The gate here is a counter of frames actually staged, not a prediction about
 * whether some task might care.  It is raised by the producer itself, so it can
 * only ever be a false positive, and a false positive costs one relaxed load.
 *
 * `budget` caps each lane rather than the whole pass: four CPUs reaching four
 * different lanes at the same time is the entire point, and a global cap would
 * hand three of them to a fourth.
 *
 * The whole function is guarded rather than given an empty body at one lane,
 * because a call to an empty out-of-line function is still a call: measured, it
 * added ten bytes to progress.c's .text at CONFIG_NET_LANES == 1.  The header's
 * A20_LWIP_LANE_RX_POLL() macro removes the call site from the preprocessed
 * source instead. */
#if CONFIG_NET_LANES > 1
void a20_lwip_lane_rx_poll(unsigned budget)
{
    if (net_lane_rx_queued_total() == 0)
        return;
    a20_lwip_lane_drain_all(budget);
}
#endif /* CONFIG_NET_LANES > 1 */

void a20_lwip_poll_locked(void) {
    a20_lwip_poll_timers_locked();
    a20_lwip_poll_rx_locked(0);
}

void a20_lwip_poll(void) {
    a20_perf_count(A20_PERF_NET_POLL_CALLS);
    uint64_t flags = a20_lwip_lock();
    a20_lwip_poll_locked();
    a20_lwip_unlock(flags);
    /* With g_lwip_lock dropped, so this is allowed to touch socket buckets. */
    a20_lwip_netlink_flush();
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
 * The bottom-halves are NOT gated: they take a socket lock rather
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
    uint32_t devcaps = 0;
    uint64_t sg_frames = 0, sg_bytes = 0;
    if (netif_default) {
        static char namebuf[8];
        snprintf(namebuf, sizeof(namebuf), "%c%c%d",
                 netif_default->name[0], netif_default->name[1],
                 netif_default->num);
        ifname = namebuf;
        state = netif_is_up(netif_default) ? "up" : "down";
        if (netif_default->state) {
            const a20_lwip_netif_state_t *dst =
                (const a20_lwip_netif_state_t *)netif_default->state;
            devcaps = dst->caps;
            sg_frames = dst->tx_sg_frames;
            sg_bytes = dst->tx_sg_bytes;
        }
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
     * net locks, and no path nests a net lock under g_lwip_lock, so nothing
     * here could deadlock even if it were the other way round.  It runs outside
     * g_lwip_lock anyway, because the census takes a lock per socket and an
     * lwIP critical section must not do that.  A gateway / netconf
     * line, not a hot counter -- this exists so that "did the lanes actually
     * spread the connections" is answerable without attaching a debugger, which
     * is the question every later stage depends on.
     */
    unsigned lanes[CONFIG_NET_LANES];
    unsigned total = 0;
    memset(lanes, 0, sizeof(lanes));
    /* One bucket at a time, only to reach the slot table; each socket found is
     * read under its own lock.  The census never needs a second net lock -- it
     * reads only `lane`, which is fixed for the life of the socket -- so it does
     * not take the shard set as a whole, which the lock rules forbid. */
    for (int b = 0; b < NET_SOCK_BUCKETS; b++) {
        uint64_t nflags = net_bucket_lock(b);
        int base = b << NET_SOCK_BUCKET_SHIFT;
        for (int k = 0; k < NET_SOCK_SLOTS_PER_BUCKET; k++) {
            net_socket_t *s = g_sockets[base + k];
            if (!s)
                continue;
            uint64_t sflags = net_sock_lock(s);
            if (net_socket_is_live(s)) {
                unsigned l = s->lane;
                if (l < CONFIG_NET_LANES)
                    lanes[l]++;
                total++;
            }
            net_sock_unlock(s, sflags);
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
    /*
     * What the driver under this netif actually negotiated.  Printed rather than
     * assumed: a bit set here means a driver implemented the matching path, and
     * the two offload bits read 0 on every current NIC because lwIP 2.2.2 as
     * vendored has no way to be told they are in use -- see the note on
     * NET_DEV_CAP_TX_CSUM_OFFLOAD in drivers/core/driver_class.h.
     */
    snprintf(cell, sizeof(cell),
             "\ndevcaps: %s caps=0x%x [tx_sg:%s][tx_csum_offload:%s]"
             "[rx_csum_offload:%s][mrg_rxbuf:%s] sg_tx=%llu sg_tx_bytes=%llu",
             ifname, devcaps,
             (devcaps & NET_DEV_CAP_TX_SG) ? "on" : "off",
             (devcaps & NET_DEV_CAP_TX_CSUM_OFFLOAD) ? "on" : "off",
             (devcaps & NET_DEV_CAP_RX_CSUM_OFFLOAD) ? "on" : "off",
             (devcaps & NET_DEV_CAP_MRG_RXBUF) ? "on" : "off",
             (unsigned long long)sg_frames, (unsigned long long)sg_bytes);
    a20_lwip_append(buf, bufsz, &off, cell);
#if CONFIG_NET_LOCK_ASSERT
    snprintf(cell, sizeof(cell),
             "\nlwip_lock: armed=%d owner=%u violations=%u sites=%u\n",
             g_lwip_lock_armed ? 1 : 0, g_lwip_lock_owner,
             a20_lwip_lock_violations(), g_lwip_lock_nsites);
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
    /*
     * The net-lock side of the same switch.  Rendered here rather than in
     * net_format_status() because this is the function that already owns the
     * lwIP row and the two are read together; it runs with no lock held (the
     * lane census above dropped both of its own), and net_lock_probe_format()
     * only reads per-CPU counters.
     */
    {
        char lockrow[512];
        int lockn = net_lock_probe_format(lockrow, sizeof(lockrow));
        if (lockn > 0)
            a20_lwip_append(buf, bufsz, &off, lockrow);
    }
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

#if CONFIG_NET_LANES > 1
    /*
     * Per-lane pool accounting, printed after everything above so no gate
     * keying on "^POOLNAME <digits>" can see a row it did not expect.
     *
     * These are two monotonic counters per lane, not a used/free pair, and the
     * reason is in memp.c: memp_free() is handed a pool id and a pointer and
     * nothing that says which lane allocated the element, so alloc-minus-freed
     * would be wrong for any pool that is ever allocated on one lane and
     * released on another -- which is the normal case for a pbuf.  A gauge fed
     * that way underflows stats_mem's u16_t.  What these two numbers do answer
     * is the question /proc/net/status's lanes line is built to answer: is
     * traffic actually being spread over the lanes, or is everything landing on
     * one of them?
     *
     * Summed over every pool rather than broken out per pool: stage C2 made the
     * counters per (lane, pool), and MEMP_MAX pools times CONFIG_NET_LANES rows
     * would bury the one line a reader wants here.  The header changed from
     * "pbuf lane" to "memp lane" because the scope did -- these are no longer
     * just the two pbuf pools.
     */
    {
        char lane_row[64];
        a20_lwip_append(buf, bufsz, &off,
            "memp lane         alloc   freed\n");
        for (unsigned l = 0; l < CONFIG_NET_LANES; l++) {
            unsigned long lalloc = 0, lfreed = 0;
            for (memp_t p = 0; p < MEMP_MAX; p++) {
                const struct memp_lane_count *c = memp_lane_count_get(l, p);
                if (!c)
                    continue;
                lalloc += c->alloc;
                lfreed += c->freed;
            }
            snprintf(lane_row, sizeof(lane_row), "%-15lu%7lu%8lu\n",
                     (unsigned long)l, lalloc, lfreed);
            a20_lwip_append(buf, bufsz, &off, lane_row);
        }
    }

    /*
     * Stage D's dispatch accounting.  `rx` counts frames this lane handed to
     * netif input and `drop` counts frames its queue refused, and the question
     * they answer is the one stage D exists to answer: is the traffic actually
     * being spread across lanes, or is one lane taking everything?
     *
     * Read without the lane claim and without g_lwip_lock, which is why the
     * numbers are monotonic rather than a depth: a per-lane occupancy gauge would
     * have to be sampled under the claim, and /proc is not a place that gets to
     * contend with the receive path.  A drop count that is not zero means a
     * lane's consumer did not keep up, and that is a defect to see rather than
     * load to average over.
     */
    {
        char lane_row[80];
        a20_lwip_append(buf, bufsz, &off,
            "rx lane            rx   drop\n");
        for (unsigned l = 0; l < CONFIG_NET_LANES; l++) {
            snprintf(lane_row, sizeof(lane_row), "%-15lu%6llu%7llu\n",
                     (unsigned long)l,
                     net_lane_rx_stat(l, NET_LANE_RX_PROCESSED),
                     net_lane_rx_stat(l, NET_LANE_RX_DROPPED));
            a20_lwip_append(buf, bufsz, &off, lane_row);
        }
        snprintf(lane_row, sizeof(lane_row), "rx staged (not yet processed): %u\n",
                 net_lane_rx_queued_total());
        a20_lwip_append(buf, bufsz, &off, lane_row);
    }
#endif /* CONFIG_NET_LANES > 1 */

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

    /*
     * The socket table's half of the same account, which no pool row can show
     * either.  These objects come from the socket obj_cache rather than from
     * .bss, so they cost nothing until a socket exists -- but the ceiling is a
     * compile-time number and the cache will fill it, which is exactly why
     * "the netmem page looked small" was never evidence that the tier fitted.
     *
     * `total` is NET_MAX_SOCKETS x sizeof(net_socket_t) -- the worst case the
     * table can reach, not a live count, so it is directly comparable with the
     * static .bss line above and with NET_PROFILE_SOCKET_BUDGET, the ceiling
     * socket_internal.h asserts it against.  The ring and payload columns are
     * the two profile numbers that decide it, printed because a reader looking
     * at a tier that no longer fits has to be able to see which knob moved.
     */
    {
        /* Its own buffer: seven fields on a 128 B row would truncate, and a
         * truncated accounting line is worse than no line. */
        char sock_row[192];
        /*
         * budget= is "n/a" on the tiers that declare no ceiling, not 0.  Only
         * the embedded tier defines NET_PROFILE_SOCKET_BUDGET, and printing a
         * literal 0 there would read as "31 MB of sockets against a zero
         * budget" -- an overrun claim that is not true and that nothing
         * asserts.  A tier without a declared ceiling says so.
         */
#ifdef NET_PROFILE_SOCKET_BUDGET
        const char *budget = NULL;
        char budget_buf[32];
        snprintf(budget_buf, sizeof(budget_buf), "%lu",
                 (unsigned long)NET_PROFILE_SOCKET_BUDGET);
        budget = budget_buf;
#else
        const char *budget = "n/a";
#endif
        snprintf(sock_row, sizeof(sock_row),
                 "socket table: per_socket=%lu slots=%lu bh_ring=%lu "
                 "inline_payload=%lu total=%lu budget=%s\n",
                 (unsigned long)sizeof(net_socket_t),
                 (unsigned long)NET_MAX_SOCKETS,
                 (unsigned long)NET_BH_RING_SIZE,
                 (unsigned long)NET_BH_INLINE_PAYLOAD,
                 (unsigned long)(NET_MAX_SOCKETS * sizeof(net_socket_t)),
                 budget);
        a20_lwip_append(buf, bufsz, &off, sock_row);
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

    /* What the address event will carry.  Read under the lock, before the
     * write, so it is the state that was actually in force -- not a re-read
     * of an interface that has already been cleared.  An all-zero `want` is
     * a delete, and net_netlink_addr_notify() turns it into RTM_DELADDR. */
    uint8_t eff_addr[4], eff_mask[4];
    eff_addr[0] = addr[0]; eff_addr[1] = addr[1];
    eff_addr[2] = addr[2]; eff_addr[3] = addr[3];

    uint64_t flags = a20_lwip_lock();
    struct netif *n = a20_lwip_netif_by_index(ifindex);
    if (!n) {
        a20_lwip_unlock(flags);
        return -ENODEV;
    }
    a20_lwip_copy_ip4(eff_mask, netif_ip4_netmask(n));
    netif_set_ipaddr(n, &want);
    if (ip4_addr_isany_val(want)) {
        ip4_addr_t zero;
        ip4_addr_set_zero(&zero);
        netif_set_netmask(n, &zero);
        netif_set_gw(n, &zero);
        /* The prefix went with the address, so the notification must not carry
         * the old one: RTM_DELADDR names the address that went away, and a
         * stale /24 on it would tell a listener to remove the wrong prefix. */
        memset(eff_mask, 0, sizeof(eff_mask));
    } else if (mask) {
        ip4_addr_t m;
        IP4_ADDR(&m, mask[0], mask[1], mask[2], mask[3]);
        netif_set_netmask(n, &m);
        memcpy(eff_mask, mask, sizeof(eff_mask));
    } else if (gw) {
        ip4_addr_t g;
        IP4_ADDR(&g, gw[0], gw[1], gw[2], gw[3]);
        netif_set_gw(n, &g);
    }
    a20_lwip_unlock(flags);
    /* Outside g_lwip_lock: the notify path takes net locks. */
    net_netlink_addr_notify(ifindex, eff_addr, eff_mask);
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

/*
 * Add one IPv6 address to a netif.  This is the kernel write path behind
 * RTM_NEWADDR with ifa_family == AF_INET6, and it exists so RTNLGRP_IPV6_IFADDR
 * has an event source: without it the group could be defined and bound but
 * never fed, which is exactly the "negotiated and unused" state worth avoiding.
 *
 * Add-only, and deliberately so.  lwIP with LWIP_NETIF_API == 0 exposes no
 * netif_remove_ip6_address(); removal goes through nd6.c's internal pool
 * teardown, and reaching into it from here would be a real divergence for a
 * capability nothing in this tree uses yet.  So RTM_DELADDR for AF_INET6 is
 * refused with -EOPNOTSUPP by the caller rather than half-performed.
 *
 * No DAD either.  netif_add_ip6_address() parks the address TENTATIVE and
 * lwIP's ND6 timer would promote it after the probes; this promotes it straight
 * to IP6_ADDR_VALID instead, because the address came from an explicit
 * administrative request rather than from a router advertisement, and the
 * loopback netif already takes the same shortcut (a20_lwip_loopif_init_cb).
 * The boundary is recorded in docs/net/network-config-design.md: a listener
 * must not read this event as "duplicate address detection passed".
 *
 * prefixlen is accepted and validated but not applied: lwIP's IPv6 subnet
 * membership comes from the prefix-length field carried inside the address
 * itself (ip6_addr_t's zone/subnet encoding), not from a separate netmask, and
 * there is no per-address prefixlen in struct netif.  It is validated so a
 * malformed request is still refused, and it is what the notification reports.
 */
int a20_lwip_if_set_addr6(unsigned ifindex, const uint8_t addr[16],
                          uint8_t prefixlen)
{
    if (!addr)
        return -EINVAL;
    if (prefixlen > 128)
        return -EINVAL;
    ip6_addr_t want;
    /* IP6_ADDR_PART() is lwIP's own byte-part-to-u32 setter and applies the
     * byte-order conversion, so this cannot drift from how the rest of the stack
     * reads an ip6_addr_t. */
    ip6_addr_set_zero(&want);
    IP6_ADDR_PART(&want, 0, addr[0], addr[1], addr[2], addr[3]);
    IP6_ADDR_PART(&want, 1, addr[4], addr[5], addr[6], addr[7]);
    IP6_ADDR_PART(&want, 2, addr[8], addr[9], addr[10], addr[11]);
    IP6_ADDR_PART(&want, 3, addr[12], addr[13], addr[14], addr[15]);
    /* An all-zero address is not a state an interface can be put into, and
     * lwIP treats it as invalid, so it would be stored and then never used.
     * Refuse it here rather than accept a write that reports success. */
    int any = 1;
    for (int i = 0; i < 16; i++)
        if (addr[i]) { any = 0; break; }
    if (any)
        return -EINVAL;

    uint64_t flags = a20_lwip_lock();
    struct netif *n = a20_lwip_netif_by_index(ifindex);
    if (!n) {
        a20_lwip_unlock(flags);
        return -ENODEV;
    }
    s8_t idx = -1;
    /* netif_add_ip6_address() returns ERR_OK with chosen_idx set for an address
     * that is already present, so this is idempotent the same way the IPv4 path
     * is: re-adding an address already configured is a no-op, not a duplicate
     * slot. */
    err_t e = netif_add_ip6_address(n, &want, &idx);
    if (e != ERR_OK || idx < 0) {
        a20_lwip_unlock(flags);
        return -ENOSPC;               /* every slot taken, or no slot for this scope */
    }
    netif_ip6_addr_set_state(n, idx, IP6_ADDR_VALID);
    a20_lwip_unlock(flags);
    /* Outside g_lwip_lock: the notify path takes net locks. */
    net_netlink_addr6_notify(ifindex, addr, prefixlen);
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
    uint32_t index;
    uint8_t up;
    if (flags & A20_LWIP_IF_F_UP)
        netif_set_up(n);
    else
        netif_set_down(n);
    index = (uint32_t)netif_get_index(n);
    up = netif_is_link_up(n) ? 1 : 0;
    a20_lwip_unlock(lf);
    /* An admin up/down is a link change the same way a carrier flip is, and a
     * listener of RTNLGRP_LINK cannot tell the two apart -- neither can the
     * message format.  Published here rather than through link_pending
     * because this call is synchronous with a userspace request: the caller
     * (RTM_NEWLINK) is waiting for a result, and deferring the notification
     * to the next poll would let it observe the new state from a dump before
     * the event arrives. */
    nlrt_link_event_t ev = {
        .index = index,
        .want_up = up,
    };
    net_netlink_link_notify(&ev, 1);
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

