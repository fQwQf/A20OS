/* Public network-domain facade.
 *
 * This file is the only place that knows how a net_backend_t is laid out.
 * Every entry point is NULL-safe: an absent backend or an absent optional
 * module turns into a precise error or a no-op, never into a crash and never
 * into a silent success.
 */
#include "net/stack.h"
#include "core/errno.h"
#include "core/stdio.h"
#include "core/string.h"

#ifndef CONFIG_NET_BACKEND_NAME
#define CONFIG_NET_BACKEND_NAME "none"
#endif

static const net_backend_t *selected(void) {
    return net_backend_current();
}

void net_stack_init(void) {
    /* An MCU instance may intentionally omit TCP/IP.  That is a configured
     * absence, not a missing backend, so it must not look like an error. */
    if (!strcmp(CONFIG_NET_BACKEND_NAME, "none"))
        return;

    net_backend_registry_init();

    int rc = net_backend_select(NULL);
    if (rc < 0) {
        /* Not fatal for images that compile no TCP/IP at all, but it must be
         * visible: a configured-but-missing backend is a build/instance
         * error, not a reason to run a different stack. */
        printf("[NET] backend '%s' is not compiled in; network unavailable\n",
               CONFIG_NET_BACKEND_NAME);
        return;
    }

    const net_backend_t *b = selected();
    if (b->cycle && b->cycle->init)
        b->cycle->init();
}

void net_stack_attach_interfaces(void) {
    const net_backend_t *b = selected();
    if (b && b->cycle && b->cycle->attach)
        b->cycle->attach();
}

const net_backend_t *net_stack_backend(void) {
    return selected();
}

int net_stack_available(void) {
    return selected() != NULL;
}

const char *net_stack_name(void) {
    const net_backend_t *b = selected();
    return b ? b->name : "none";
}

uint32_t net_stack_caps(void) {
    const net_backend_t *b = selected();
    return b ? b->caps : 0;
}

int net_stack_has(uint32_t caps) {
    return net_backend_has(caps);
}

int net_stack_format_caps(char *buf, size_t len) {
    return net_backend_format_caps(buf, len);
}

void net_stack_receive_progress(unsigned budget) {
    const net_backend_t *b = selected();
    if (b && b->event && b->event->rx_progress)
        b->event->rx_progress(budget);
}

void net_stack_device_interrupt(const struct device *dev) {
    const net_backend_t *b = selected();
    if (b && b->event && b->event->irq)
        b->event->irq(dev);
}

void net_stack_protocol_progress(unsigned budget) {
    const net_backend_t *b = selected();
    if (b && b->event && b->event->protocol_progress)
        b->event->protocol_progress(budget);
}

void net_stack_timer_tick(void) {
    const net_backend_t *b = selected();
    if (b && b->event && b->event->timer_tick)
        b->event->timer_tick();
}

int net_stack_work_pending(void) {
    const net_backend_t *b = selected();
    return (b && b->event && b->event->work_pending) ? b->event->work_pending()
                                                     : 0;
}

void net_stack_poll_waiter(void) {
    const net_backend_t *b = selected();
    if (b && b->event && b->event->poll_waiter)
        b->event->poll_waiter();
}

int net_stack_tun_create(net_tun_emit_t emit, void *ctx) {
    const net_backend_t *b = selected();
    if (b && b->packet && b->packet->tun_create)
        return b->packet->tun_create(emit, ctx);
    return -EOPNOTSUPP;
}

void net_stack_tun_destroy(unsigned index) {
    const net_backend_t *b = selected();
    if (b && b->packet && b->packet->tun_destroy)
        b->packet->tun_destroy(index);
}

int net_stack_tun_inject(unsigned index, const uint8_t *packet, size_t len) {
    const net_backend_t *b = selected();
    if (b && b->packet && b->packet->tun_inject)
        return b->packet->tun_inject(index, packet, len);
    return -EOPNOTSUPP;
}

int net_stack_packet_tx(unsigned index, const uint8_t *frame, size_t len) {
    const net_backend_t *b = selected();
    if (b && b->packet && b->packet->tx)
        return b->packet->tx(index, frame, len);
    return -EOPNOTSUPP;
}

int net_stack_interface_snapshot(void *out, int maximum) {
    const net_backend_t *b = selected();
    if (b && b->iface && b->iface->snapshot)
        return b->iface->snapshot(out, maximum);
    return -EOPNOTSUPP;
}

int net_stack_addr6_snapshot(nl_addr6_out_t *out, int maximum)
{
    const net_backend_t *b = selected();
    if (b && b->iface && b->iface->snapshot_addr6)
        return b->iface->snapshot_addr6(out, maximum);
    return 0;
}

int net_stack_if_up(unsigned index) {
    const net_backend_t *b = selected();
    /* Boolean by contract: 0 means down or unavailable.  Never return a
     * negative errno here, because callers use it as a truth value. */
    if (!b || !b->iface || !b->iface->up)
        return 0;
    return b->iface->up(index) ? 1 : 0;
}

int net_stack_if_default_index(void) {
    const net_backend_t *b = selected();
    return (b && b->iface && b->iface->default_index)
               ? b->iface->default_index()
               : 0;
}

int net_stack_owns_ipv4(uint32_t wire_address) {
    const net_backend_t *b = selected();
    return (b && b->iface && b->iface->owns_ipv4)
               ? b->iface->owns_ipv4(wire_address)
               : 0;
}

int net_stack_if_mtu(unsigned index) {
    const net_backend_t *b = selected();
    return (b && b->iface && b->iface->mtu) ? b->iface->mtu(index) : 0;
}

int net_stack_if_hwaddr(unsigned index, uint8_t mac[6]) {
    const net_backend_t *b = selected();
    if (b && b->iface && b->iface->hwaddr)
        return b->iface->hwaddr(index, mac);
    return -EOPNOTSUPP;
}

int net_stack_if_set_mtu(unsigned index, uint16_t mtu) {
    const net_backend_t *b = selected();
    if (b && b->iface && b->iface->set_mtu)
        return b->iface->set_mtu(index, mtu);
    return -EOPNOTSUPP;
}

int net_stack_if_set_flags(unsigned index, uint32_t value, uint32_t mask) {
    const net_backend_t *b = selected();
    if (b && b->iface && b->iface->set_flags)
        return b->iface->set_flags(index, value, mask);
    return -EOPNOTSUPP;
}

int net_stack_if_get_addr(unsigned index, uint8_t *addr, uint8_t *mask,
                          uint8_t *gateway) {
    const net_backend_t *b = selected();
    if (b && b->iface && b->iface->get_addr)
        return b->iface->get_addr(index, addr, mask, gateway);
    return -EOPNOTSUPP;
}

int net_stack_if_set_addr(unsigned index, const uint8_t *addr,
                          const uint8_t *mask, const uint8_t *gateway) {
    const net_backend_t *b = selected();
    if (b && b->iface && b->iface->set_addr)
        return b->iface->set_addr(index, addr, mask, gateway);
    return -EOPNOTSUPP;
}

int net_stack_if_set_addr6(unsigned index, const uint8_t *addr,
                           uint8_t prefix) {
    const net_backend_t *b = selected();
    if (b && b->iface && b->iface->set_addr6)
        return b->iface->set_addr6(index, addr, prefix);
    return -EOPNOTSUPP;
}

int net_stack_if_del_addr6(unsigned index, const uint8_t *addr) {
    const net_backend_t *b = selected();
    if (b && b->iface && b->iface->del_addr6)
        return b->iface->del_addr6(index, addr);
    return -EOPNOTSUPP;
}

int net_stack_route_update_v4(const uint8_t *dst, uint8_t prefix,
                              const uint8_t *gateway, uint16_t index,
                              int remove) {
    const net_backend_t *b = selected();
    if (b && b->iface && b->iface->route_update_v4)
        return b->iface->route_update_v4(dst, prefix, gateway, index, remove);
    return 0; /* no backend RIB: the common FIB is the only database */
}

int net_stack_neighbor_update_v4(uint32_t index, uint32_t address,
                                 const uint8_t mac[6]) {
    const net_backend_t *b = selected();
    if (b && b->iface && b->iface->neighbor_update_v4)
        return b->iface->neighbor_update_v4(index, address, mac);
    return -EOPNOTSUPP;
}

uint32_t net_backend_interface_ipv4(int index) {
    const net_backend_t *b = selected();
    return (b && b->iface && b->iface->interface_ipv4)
               ? b->iface->interface_ipv4(index)
               : 0;
}

void net_stack_config_snapshot(a20_net_config_t *out) {
    if (!out)
        return;
    const net_backend_t *b = selected();
    if (b && b->iface && b->iface->config_snapshot) {
        b->iface->config_snapshot(out);
        return;
    }
    memset(out, 0, sizeof(*out));
}

static int net_stack_obs(int (*fn)(char *, size_t), char *buf, size_t len) {
    if (!buf || !len)
        return 0;
    if (!fn)
        return 0;
    int used = fn(buf, len);
    return used < 0 ? 0 : used;
}

int net_stack_format_memory(char *buf, size_t len) {
    const net_backend_t *b = selected();
    int used = b && b->obs ? net_stack_obs(b->obs->memory, buf, len) : 0;
    if (!len)
        return 0;
    if ((size_t)used >= len)
        return (int)len - 1;
    return used + net_tun_format(buf + used, len - (size_t)used);
}

int net_stack_format_devices(char *buf, size_t len) {
    const net_backend_t *b = selected();
    return b && b->obs ? net_stack_obs(b->obs->devices, buf, len) : 0;
}

int net_stack_format_routes(char *buf, size_t len) {
    const net_backend_t *b = selected();
    return b && b->obs ? net_stack_obs(b->obs->routes, buf, len) : 0;
}

int net_stack_status(char *buf, size_t len) {
    if (!buf || !len)
        return 0;
    const net_backend_t *b = selected();
    int used = snprintf(buf, len, "backend: %s\n", b ? b->name : "none");
    if (used < 0)
        return 0;
    if ((size_t)used >= len)
        return (int)len - 1;

    int n = net_backend_format_caps(buf + used, len - (size_t)used);
    if (n > 0)
        used += n;
    if ((size_t)used >= len)
        return (int)len - 1;

    if (b && b->obs && b->obs->status) {
        int backend_used = b->obs->status(buf + used, len - (size_t)used);
        if (backend_used > 0)
            used += backend_used;
    }
    if ((size_t)used >= len)
        return (int)len - 1;
    return used + net_tun_format(buf + used, len - (size_t)used);
}
