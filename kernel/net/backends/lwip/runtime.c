/* lwIP owns its execution model. A20OS scheduling only sees bounded protocol
 * progress and pending work, including socket events published by callbacks. */
#include "net/stack.h"
#include "tun_private.h"
#include "core/string.h"
#include "net/lwip_stack.h"
#include "net/socket/socket_internal.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
extern int a20_lwip_interface_snapshot(void *, int);
extern int a20_lwip_interface_mtu(unsigned);
extern int a20_lwip_owns_ipv4(uint32_t);
extern void a20_lwip_config_snapshot(a20_net_config_t *);
extern uint32_t a20_lwip_interface_ipv4(int);
extern const net_socket_ops_t a20_lwip_socket_ops;
extern const net_socket_opt_ops_t a20_lwip_socket_opt_ops;
extern const net_endpoint_ops_t a20_lwip_endpoint_ops;

/* Isolated port extension; upstream exposes only permanent insertion. */
extern err_t a20_etharp_update_dynamic(struct netif *, const ip4_addr_t *,
                                      struct eth_addr *);

static int neighbor_update_v4(uint32_t ifindex, uint32_t address,
                               const uint8_t mac[6]) {
#ifdef CONFIG_LWIP_UPSTREAM
    /* Upstream has no public dynamic insertion API. DHCP broadcast replies
     * work without inventing a second cache or modifying upstream internals. */
    (void)ifindex; (void)address; (void)mac;
    return -EOPNOTSUPP;
#else
    if (!ifindex || ifindex > 255 || !address || (mac[0] & 1))
        return -EINVAL;
    uint8_t any = 0;
    for (unsigned i = 0; i < 6; ++i) any |= mac[i];
    if (!any) return -EINVAL;
    ip4_addr_t ip = { .addr = address };
    struct eth_addr ethernet;
    memcpy(ethernet.addr, mac, sizeof(ethernet.addr));
    uint64_t flags = a20_lwip_lock();
    struct netif *n = netif_get_by_index((u8_t)ifindex);
    int result = -ENODEV;
    if (n) {
        if (!(n->flags & NETIF_FLAG_ETHARP) ||
            !ip4_addr_net_eq(&ip, netif_ip4_addr(n), netif_ip4_netmask(n)))
            result = -EINVAL;
        else {
            err_t err = a20_etharp_update_dynamic(n, &ip, &ethernet);
            result = err == ERR_OK ? 0 : err == ERR_MEM ? -ENOMEM : -EINVAL;
        }
    }
    a20_lwip_unlock(flags);
    return result;
#endif
}

/* The IRQ caller owns a live device through its registered handler. Resolve
 * backend identity rather than assuming driver slot == protocol interface. */
static void device_interrupt(const struct device *dev) {
    uint64_t flags = a20_lwip_ingress_lock();
    int index = a20_lwip_netif_index_for_device(dev);
    if (index >= 0)
        a20_lwip_process_netif_irq_locked(index);
    else
        a20_lwip_signal_rx_pending();
    a20_lwip_ingress_unlock(flags);
}

static void protocol_progress(unsigned budget) {
    (void)budget;
    A20_LWIP_LANE_RX_POLL(budget);
    net_inet_bottom_half_process_all();
}

static void timer_tick(void) {
#if CONFIG_NET_LANES > 1
    a20_lwip_signal_timer_pending();
#else
    uint64_t flags = a20_lwip_lock();
    a20_lwip_poll_timers_locked();
    a20_lwip_unlock(flags);
#endif
}

static int work_pending(void) {
    return a20_lwip_rx_pending_any() || net_bh_any_pending();
}

static const net_backend_cycle_ops_t lwip_cycle_ops = {
    .init = a20_lwip_init,
    .attach = a20_lwip_attach_netifs,
};

static const net_backend_event_ops_t lwip_event_ops = {
    .rx_progress = a20_lwip_rx_progress,
    .irq = device_interrupt,
    .protocol_progress = protocol_progress,
    .timer_tick = timer_tick,
    .work_pending = work_pending,
    .poll_waiter = a20_lwip_poll_waiter,
};

static const net_backend_if_ops_t lwip_iface_ops = {
    .up = a20_lwip_if_up,
    .default_index = a20_lwip_if_default_index,
    .owns_ipv4 = a20_lwip_owns_ipv4,
    .mtu = a20_lwip_interface_mtu,
    .hwaddr = a20_lwip_if_hwaddr,
    .snapshot = a20_lwip_interface_snapshot,
    .config_snapshot = a20_lwip_config_snapshot,
    .set_mtu = a20_lwip_if_set_mtu,
    .set_flags = a20_lwip_if_set_flags,
    .get_addr = a20_lwip_if_get_addr,
    .set_addr = a20_lwip_if_set_addr,
    .set_addr6 = a20_lwip_if_set_addr6,
    .del_addr6 = a20_lwip_if_del_addr6,
    .neighbor_update_v4 = neighbor_update_v4,
    .interface_ipv4 = a20_lwip_interface_ipv4,
};

static const net_backend_packet_ops_t lwip_packet_ops = {
    .tx = a20_lwip_packet_tx,
    .tun_create = a20_lwip_tun_create,
    .tun_destroy = a20_lwip_tun_destroy,
    .tun_inject = a20_lwip_tun_inject,
};

static const net_backend_obs_ops_t lwip_obs_ops = {
    .status = a20_lwip_format_status,
    .memory = a20_lwip_format_memp,
    .devices = a20_lwip_format_net_dev,
    .routes = a20_lwip_format_route,
};

/*
 * Capabilities are the Linux-ABI promises this backend can keep.  The upstream
 * lwIP port intentionally does not advertise neighbour injection or CUBIC:
 * its own modules return -EOPNOTSUPP for those, and a capability bit without a
 * working module would be a lie the frontend forwards to userspace.
 */
#ifdef CONFIG_LWIP_UPSTREAM
#define A20_LWIP_CAPS                                                   \
    (NET_CAP_STREAM | NET_CAP_DGRAM | NET_CAP_RAW | NET_CAP_IPV6 |      \
     NET_CAP_LISTEN | NET_CAP_SHUTDOWN | NET_CAP_SNDBUF |               \
     NET_CAP_RCVBUF | NET_CAP_PKTINFO | NET_CAP_BINDTODEVICE |          \
     NET_CAP_UDP_CONTROL | NET_CAP_PMTU | NET_CAP_RAW_TX | NET_CAP_TUN |\
     NET_CAP_IF_ADDR6 | NET_CAP_IF_DYNAMIC | NET_CAP_CONGESTION |         \
     NET_CAP_DEVICE_IRQ | NET_CAP_OFFLOAD_SG)
#else
#define A20_LWIP_CAPS                                                   \
    (NET_CAP_STREAM | NET_CAP_DGRAM | NET_CAP_RAW | NET_CAP_IPV6 |      \
     NET_CAP_LISTEN | NET_CAP_SHUTDOWN | NET_CAP_SNDBUF |               \
     NET_CAP_RCVBUF | NET_CAP_PKTINFO | NET_CAP_BINDTODEVICE |          \
     NET_CAP_UDP_CONTROL | NET_CAP_PMTU | NET_CAP_RAW_TX | NET_CAP_TUN |\
     NET_CAP_NEIGH | NET_CAP_IF_ADDR6 | NET_CAP_IF_DYNAMIC |            \
     NET_CAP_CONGESTION | NET_CAP_DEVICE_IRQ | NET_CAP_OFFLOAD_SG)
#endif

const net_backend_t a20_lwip_backend = {
    .name =
#ifdef CONFIG_LWIP_UPSTREAM
        "lwip",
#else
        "native",
#endif
    .caps = A20_LWIP_CAPS,
    .cycle = &lwip_cycle_ops,
    .event = &lwip_event_ops,
    .socket = &a20_lwip_socket_ops,
    .opts = &a20_lwip_socket_opt_ops,
    .endpoint = &a20_lwip_endpoint_ops,
    .iface = &lwip_iface_ops,
    .packet = &lwip_packet_ops,
    .obs = &lwip_obs_ops,
};

void a20_lwip_backend_register(void)
{
    (void)net_backend_register(&a20_lwip_backend);
}
