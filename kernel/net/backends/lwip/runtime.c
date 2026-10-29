/* lwIP owns its execution model. A20OS scheduling only sees bounded protocol
 * progress and pending work, including socket events published by callbacks. */
#include "net/stack.h"
#include "core/string.h"
#include "net/lwip_stack.h"
#include "net/socket/socket_internal.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
extern int a20_lwip_interface_snapshot(void *, int);
extern void a20_lwip_config_snapshot(a20_net_config_t *);
extern const net_transport_ops_t a20_lwip_transport_ops;

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

const net_stack_ops_t a20_network_stack_ops = {
    .transport = &a20_lwip_transport_ops,
    .name =
#ifdef CONFIG_LWIP_UPSTREAM
        "lwip",
#else
        "native",
#endif
    .init = a20_lwip_init,
    .attach_interfaces = a20_lwip_attach_netifs,
    .receive_progress = a20_lwip_rx_progress,
    .device_interrupt = device_interrupt,
    .protocol_progress = protocol_progress,
    .timer_tick = timer_tick,
    .work_pending = work_pending,
    .neighbor_update_v4 = neighbor_update_v4,
    .status = a20_lwip_format_status,
    .format_memory = a20_lwip_format_memp,
    .format_devices = a20_lwip_format_net_dev,
    .format_routes = a20_lwip_format_route,
    .poll_waiter = a20_lwip_poll_waiter,
    .if_up = a20_lwip_if_up,
    .if_default_index = a20_lwip_if_default_index,
    .if_hwaddr = a20_lwip_if_hwaddr,
    .packet_tx = a20_lwip_packet_tx,
    .interface_snapshot = a20_lwip_interface_snapshot,
    .config_snapshot = a20_lwip_config_snapshot,
    .if_set_mtu = a20_lwip_if_set_mtu,
    .if_set_flags = a20_lwip_if_set_flags,
    .if_get_addr = a20_lwip_if_get_addr,
    .if_set_addr = a20_lwip_if_set_addr,
    .if_set_addr6 = a20_lwip_if_set_addr6,
    .if_del_addr6 = a20_lwip_if_del_addr6,
};
