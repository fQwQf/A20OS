/* Selectable BSD stack: class NIC ownership, real-time callout progress and
 * the public net_backend contract. Protocol execution itself remains
 * in the isolated core. */
#include "executor.h"
#include "protocol.h"
#include "runtime/memory.h"
#include "net/stack.h"
#include "net/profile.h"

#ifndef CONFIG_NET_ROUTER
#define CONFIG_NET_ROUTER 0
#endif
#include "tun.h"
#include "nic.h"
#include "net/interface.h"
#include "net/netfilter.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "core/errno.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"

/* Resource ceilings come from the one shared profile header; this backend
 * must not grow a second, silently different copy. */
#define BSD_MAX_NICS NET_PROFILE_NETIF_MAX_DEVS
#define BSD_RX_BUDGET NET_PROFILE_RX_BUDGET

extern const net_socket_ops_t a20_bsd_socket_ops;
extern const net_socket_opt_ops_t a20_bsd_socket_opt_ops;
extern const net_endpoint_ops_t a20_bsd_endpoint_ops;
extern int a20_bsd_socket_runtime_init(void);
extern void a20_bsd_socket_progress(void);
extern void net_packet_bottom_half_process(void);
extern int a20_bsd_socket_format_status(char *, size_t);
extern void a20_bsd_port_memory_snapshot(struct bsd_memory_stats *);

typedef struct bsd_nic_slot {
    device_t *device;
    class_device_t *binding;
    void *context;
    unsigned present;
    unsigned attached;
    unsigned rx_pending;
    unsigned up;
    uint16_t mtu;
    uint8_t mac[6];
    uint8_t ip[4], mask[4], gw[4];
    unsigned bsd_ifindex;
    int has_ip;
} bsd_nic_slot_t;

static bsd_nic_slot_t nics[BSD_MAX_NICS];
static spinlock_t nic_lock = SPINLOCK_INIT;
static spinlock_t config_lock = SPINLOCK_INIT;
static unsigned progress_budget = BSD_RX_BUDGET;
static uint64_t clock_last;
static unsigned clock_due;
static void config_snapshot(a20_net_config_t *out);
static int stack_ready;

/* Public index layout: 1 = primary NIC, 2 = loopback, 3.. =
 * additional NIC slots, A20_BSD_TUN_BASE.. = TUN.  Slot 0 keeps index 1 so
 * existing a20n0/en2 users are unaffected while a router can address en3 on
 * slot 1. */
static int nic_slot_for_index(unsigned index)
{
    if (index == 1) return 0;
    if (index >= 3 && index < A20_BSD_TUN_BASE) return (int)index - 2;
    return -1;
}

static unsigned public_index_for_slot(int slot)
{
    if (slot < 0) return 2;
    if (slot < BSD_MAX_NICS) return nic_index_for_slot(slot);
    return A20_BSD_TUN_BASE + (unsigned)slot - BSD_MAX_NICS;
}

static void nic_name_for_slot(int slot, char *out, size_t size)
{
    snprintf(out, size, "en%d", slot == 0 ? 2 : slot + 2);
}

static bsd_nic_slot_t *slot_for_context(void *context)
{
    for (unsigned i = 0; i < BSD_MAX_NICS; ++i)
        if (nics[i].context == context) return &nics[i];
    return NULL;
}

static class_device_t *binding_get(bsd_nic_slot_t *slot)
{
    uint64_t flags = spin_lock_irqsave(&nic_lock);
    class_device_t *binding = slot ? slot->binding : NULL;
    class_device_get(binding);
    spin_unlock_irqrestore(&nic_lock, flags);
    return binding;
}

static void nic_offline(class_device_t *binding, void *argument)
{
    bsd_nic_slot_t *slot = argument;
    uint64_t flags = spin_lock_irqsave(&nic_lock);
    int owned = slot->binding == binding;
    if (owned) {
        slot->binding = NULL;
        slot->device = NULL;
        slot->attached = 0;
        slot->context = NULL;
    }
    spin_unlock_irqrestore(&nic_lock, flags);
    if (owned) {
        class_device_put(binding);
        a20_bsd_executor_notify();
    }
}

int a20_bsd_host_nic_bind(void *context, int port_id)
{
    if (port_id < 0 || port_id >= BSD_MAX_NICS) return -ENODEV;
    bsd_nic_slot_t *slot = &nics[port_id];
    device_t *device = device_find_get_by_class(DEV_CLASS_NET, port_id);
    class_device_t *binding = device ? class_device_ref_for_device(device) : NULL;
    if (!binding || !class_net_ready(binding)) {
        class_device_put(binding);
        device_put(device);
        return -ENODEV;
    }
    uint64_t flags = spin_lock_irqsave(&nic_lock);
    slot->device = device;
    slot->binding = binding;
    slot->context = context;
    slot->attached = 1;
    slot->rx_pending = 1;
    spin_unlock_irqrestore(&nic_lock, flags);
    if (class_device_set_offline(binding, nic_offline, slot)) {
        nic_offline(binding, slot);
        device_put(device);
        return -ENODEV;
    }
    return 0;
}

void a20_bsd_host_nic_unbind(void *context)
{
    bsd_nic_slot_t *slot = slot_for_context(context);
    if (!slot) return;
    uint64_t flags = spin_lock_irqsave(&nic_lock);
    class_device_t *binding = slot->binding;
    device_t *device = slot->device;
    slot->binding = NULL;
    slot->device = NULL;
    slot->context = NULL;
    slot->attached = 0;
    spin_unlock_irqrestore(&nic_lock, flags);
    class_device_put(binding);
    device_put(device);
}

int a20_bsd_host_nic_recv(void *context, void *frame, int capacity)
{
    class_device_t *binding = binding_get(slot_for_context(context));
    if (!binding) return -ENETDOWN;
    int result = class_net_recv(binding, frame, (size_t)capacity);
    class_device_put(binding);
    return result;
}

int a20_bsd_host_nic_send(void *context, const void *frame, int length)
{
    class_device_t *binding = binding_get(slot_for_context(context));
    if (!binding) return -ENETDOWN;
    int result = class_net_send(binding, frame, (size_t)length);
    class_device_put(binding);
    return result == length ? 0 : result < 0 ? result : -EIO;
}

void a20_bsd_host_nic_poll(void *context)
{
    class_device_t *binding = binding_get(slot_for_context(context));
    if (!binding) return;
    class_net_poll(binding);
    class_device_put(binding);
}

void a20_bsd_host_nic_token_freed(void *context)
{
    bsd_nic_slot_t *slot = slot_for_context(context);
    if (slot) __atomic_store_n(&slot->rx_pending, 1, __ATOMIC_RELEASE);
    a20_bsd_executor_notify();
}

static int copy_mac(unsigned index, uint8_t out[6])
{
    device_t *device = device_find_get_by_class(DEV_CLASS_NET, (int)index);
    class_device_t *binding = device ? class_device_ref_for_device(device) : NULL;
    int result = binding ? class_net_copy_mac(binding, out) : -ENODEV;
    class_device_put(binding);
    device_put(device);
    return result;
}

static void ipv4_text(char out[16], const void *address)
{
    const uint8_t *a = address;
    snprintf(out, 16, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

typedef struct attach_job {
    a20_bsd_nic_config_t config;
    void *context;
} attach_job_t;

static int attach_job_run(void *argument)
{
    attach_job_t *job = argument;
    job->context = a20_bsd_port_nic_attach(&job->config);
    return job->context ? 0 : -ENODEV;
}

static void stack_progress(void *argument)
{
    (void)argument;
    unsigned due = __atomic_exchange_n(&clock_due, 0, __ATOMIC_ACQ_REL);
    /* Catch up real elapsed ticks, but keep one wakeup bounded. Timer ticks
     * themselves are accounted by the monotonic clock, never by poll count. */
    if (due > 100) {
        __atomic_add_fetch(&clock_due, due - 100, __ATOMIC_RELEASE);
        due = 100;
    }
    while (due--) a20_bsd_port_hardclock();

    /* Sweep from the executor even with idle NICs. At the target cadence,
     * 256 slots per pass visits the largest configured table in 3.2 s. */
    static uint64_t sweep_at;
    uint64_t now = timer_get_ticks();
    if ((int64_t)(now - sweep_at) >= 0) {
        sweep_at = now + MS_TO_TICKS(50);
        if (netfilter_busy())
            netfilter_conntrack_expire(256);
    }

    unsigned budget = __atomic_exchange_n(&progress_budget, BSD_RX_BUDGET,
                                          __ATOMIC_ACQ_REL);
    for (unsigned i = 0; i < BSD_MAX_NICS; ++i) {
        if (!nics[i].context) continue;
        __atomic_store_n(&nics[i].rx_pending, 0, __ATOMIC_RELEASE);
        a20_bsd_port_nic_progress(nics[i].context, budget);
    }
    a20_bsd_port_protocol_progress();
    net_packet_bottom_half_process();
    a20_bsd_socket_progress();
}

static void stack_init(void)
{
    if (stack_ready) return;
    a20_net_config_init();
    netfilter_conntrack_init();
    clock_last = timer_get_ticks();
    a20_bsd_executor_set_progress(stack_progress, NULL);
    stack_ready = 1;
}

typedef struct ifindex_job {
    void *context;
    int result;
} ifindex_job_t;

static int ifindex_job_run(void *argument)
{
    ifindex_job_t *job = argument;
    job->result = a20_bsd_port_nic_ifindex(job->context);
    return job->result < 0 ? job->result : 0;
}

int a20_bsd_nic_public_ifindex(unsigned bsd_ifindex, uint8_t local[4],
                               uint32_t *public_index)
{
    for (int slot = 0; slot < BSD_MAX_NICS; ++slot) {
        if (!nics[slot].attached || nics[slot].bsd_ifindex != bsd_ifindex)
            continue;
        if (public_index) *public_index = nic_index_for_slot(slot);
        if (local) memcpy(local, nics[slot].ip, 4);
        return 0;
    }
    return -ENODEV;
}

typedef struct attach_cleanup {
    void *context;
    uint8_t address[4], mask[4];
} attach_cleanup_t;

static int attach_cleanup_run(void *argument)
{
    attach_cleanup_t *job = argument;
    uint8_t zero[4] = {0, 0, 0, 0};
    return a20_bsd_port_nic_set_ipv4(job->context, zero, zero, NULL,
                                     job->address, job->mask, NULL);
}

int a20_bsd_ifindex_to_bsd(unsigned index)
{
    if (index == 2)
        return a20_bsd_port_ifindex_from_slot(-1);
    if (a20_bsd_tun_is_index(index))
        return a20_bsd_port_ifindex_from_slot(BSD_MAX_NICS + index - A20_BSD_TUN_BASE);
    int slot = nic_slot_for_index(index);
    if (slot < 0 || slot >= BSD_MAX_NICS || !nics[slot].bsd_ifindex)
        return -ENODEV;
    return (int)nics[slot].bsd_ifindex;
}

int a20_bsd_ifindex_to_public(unsigned index)
{
    int slot;
    if (a20_bsd_port_ifindex_to_slot(index, &slot) < 0)
        return -ENODEV;
    return public_index_for_slot(slot);
}

static int attach_one_nic(int slot, const a20_net_config_t *config,
                          int configured)
{
    char address[16] = {0}, netmask[16] = {0}, broadcast[16] = {0};
    char gateway[16] = {0};
    uint8_t mac[6];
    if (copy_mac(slot, mac) < 0) return -ENODEV;
    nics[slot].present = 1;

    attach_job_t job;
    memset(&job, 0, sizeof(job));
    job.config.port_id = slot;
    nic_name_for_slot(slot, job.config.ifname, sizeof(job.config.ifname));
    memcpy(job.config.mac, mac, 6);
    job.config.accept_ra = strcmp(config->ipv6_ra_if, job.config.ifname) == 0;
    job.config.ipv6_linklocal = CONFIG_NET_ROUTER;

    int placeholder = 0;
    if (configured) {
        ipv4_text(address, &config->ip);
        ipv4_text(netmask, &config->netmask);
        ipv4_text(gateway, &config->gateway);
        uint8_t bc[4];
        const uint8_t *ip = (const uint8_t *)&config->ip;
        const uint8_t *mask = (const uint8_t *)&config->netmask;
        for (unsigned i = 0; i < 4; ++i)
            bc[i] = ip[i] | (uint8_t)~mask[i];
        ipv4_text(broadcast, bc);
    } else {
        /* ff_veth_attach always installs cfg.addr.  A zero mask would become
         * a connected 0.0.0.0/0 route and shadow the real default, so give the
         * unconfigured interface a unique /32 and remove it right away. */
        snprintf(address, sizeof(address), "169.254.%d.1", slot + 1);
        snprintf(netmask, sizeof(netmask), "255.255.255.255");
        snprintf(broadcast, sizeof(broadcast), "169.254.%d.1", slot + 1);
        snprintf(gateway, sizeof(gateway), "0.0.0.0");
        placeholder = 1;
    }
    snprintf(job.config.address, sizeof(job.config.address), "%s", address);
    snprintf(job.config.netmask, sizeof(job.config.netmask), "%s", netmask);
    snprintf(job.config.broadcast, sizeof(job.config.broadcast), "%s", broadcast);
    snprintf(job.config.gateway, sizeof(job.config.gateway), "%s", gateway);

    int result = a20_bsd_executor_call(attach_job_run, &job);
    if (result < 0) return result;

    nics[slot].context = job.context;
    nics[slot].attached = 1;
    nics[slot].up = 1;
    nics[slot].mtu = 1500;
    ifindex_job_t index_job = {job.context, -1};
    if (a20_bsd_executor_call(ifindex_job_run, &index_job) == 0)
        nics[slot].bsd_ifindex = (unsigned)index_job.result;
    memcpy(nics[slot].mac, mac, 6);
    if (configured) {
        memcpy(nics[slot].ip, &config->ip, 4);
        memcpy(nics[slot].mask, &config->netmask, 4);
        memcpy(nics[slot].gw, &config->gateway, 4);
        nics[slot].has_ip = 1;
    } else {
        memset(nics[slot].ip, 0, 4);
        memset(nics[slot].mask, 0, 4);
        memset(nics[slot].gw, 0, 4);
        nics[slot].has_ip = 0;
    }

    if (placeholder) {
        attach_cleanup_t cleanup;
        memset(&cleanup, 0, sizeof(cleanup));
        cleanup.context = job.context;
        cleanup.address[0] = 169;
        cleanup.address[1] = 254;
        cleanup.address[2] = (uint8_t)(slot + 1);
        cleanup.address[3] = 1;
        memset(cleanup.mask, 0xff, 4);
        (void)a20_bsd_executor_call(attach_cleanup_run, &cleanup);
    }
    return 0;
}

#if CONFIG_NET_ROUTER
typedef struct forwarding_job {
    int ipv4, ipv6;
} forwarding_job_t;

static int forwarding_job_run(void *argument)
{
    forwarding_job_t *job = argument;
    a20_bsd_port_nic_set_forwarding(job->ipv4, job->ipv6);
    return 0;
}
#endif

static void attach_interfaces(void)
{
    if (!stack_ready) stack_init();
    int result = a20_bsd_socket_runtime_init();
    if (result < 0) {
        printf("[NET] BSD socket runtime allocation failed: %d\n", result);
        return;
    }
    result = a20_bsd_executor_start();
    if (result < 0) {
        printf("[NET] BSD executor initialization failed: %d\n", result);
        return;
    }

    a20_net_config_t config;
    uint64_t flags = spin_lock_irqsave(&config_lock);
    config = g_a20_net_config;
    spin_unlock_irqrestore(&config_lock, flags);
    int primary_configured = config.ip.addr && config.netmask.addr;

    int attached = 0;
    for (int slot = 0; slot < BSD_MAX_NICS; ++slot) {
        if (nics[slot].attached) {
            attached++;
            continue;
        }
        int configured = slot == 0 && primary_configured;
        result = attach_one_nic(slot, &config, configured);
        if (result == -ENODEV)
            continue; /* no class NIC at this slot */
        if (result < 0) {
            printf("[NET] BSD ifnet attach slot %d failed: %d\n", slot, result);
            continue;
        }
        attached++;
        char name[16];
        nic_name_for_slot(slot, name, sizeof(name));
        if (configured) {
            char address[16], netmask[16], gateway[16];
            ipv4_text(address, &config.ip);
            ipv4_text(netmask, &config.netmask);
            ipv4_text(gateway, &config.gateway);
            printf("[NET] BSD attached %s %s/%s gateway %s\n", name,
                   address, netmask, gateway);
        } else {
            printf("[NET] BSD attached %s unconfigured\n", name);
        }
    }
    if (!attached) {
        printf("[NET] BSD found no usable class NIC\n");
        return;
    }
#if CONFIG_NET_ROUTER
    forwarding_job_t forwarding = {1, 1};
    (void)a20_bsd_executor_call(forwarding_job_run, &forwarding);
#endif
    clock_last = timer_get_ticks();
    a20_bsd_executor_notify();
}

static void receive_progress(unsigned budget)
{
    __atomic_store_n(&progress_budget, budget ? budget : BSD_RX_BUDGET, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < BSD_MAX_NICS; ++i)
        if (nics[i].context) __atomic_store_n(&nics[i].rx_pending, 1, __ATOMIC_RELEASE);
    a20_bsd_executor_notify();
}

static void device_interrupt(const struct device *device)
{
    for (unsigned i = 0; i < BSD_MAX_NICS; ++i)
        if (nics[i].device == device)
            __atomic_store_n(&nics[i].rx_pending, 1, __ATOMIC_RELEASE);
    a20_bsd_executor_notify();
}

static void protocol_progress(unsigned budget)
{
    (void)budget;
    a20_bsd_executor_notify();
}

static void timer_tick(void)
{
    uint64_t now = timer_get_ticks();
    uint64_t interval = MS_TO_TICKS(10); /* ff_global_cfg.freebsd.hz = 100 */
    if (!interval || (int64_t)(now - clock_last) < (int64_t)interval) return;
    uint64_t elapsed = (now - clock_last) / interval;
    clock_last += elapsed * interval;
    if (elapsed > 1000) elapsed = 1000;
    __atomic_add_fetch(&clock_due, (unsigned)elapsed, __ATOMIC_RELEASE);
    a20_bsd_executor_notify();
}

static int work_pending(void)
{
    if (__atomic_load_n(&clock_due, __ATOMIC_ACQUIRE)) return 1;
    for (unsigned i = 0; i < BSD_MAX_NICS; ++i)
        if (__atomic_load_n(&nics[i].rx_pending, __ATOMIC_ACQUIRE)) return 1;
    return 0;
}

static void poll_waiter(void) { a20_bsd_executor_notify(); }

static int interface_snapshot(void *outp, int maximum)
{
    nl_link_out_t *out = outp;
    int count = 0;
    for (int slot = 0; slot < BSD_MAX_NICS && count < maximum; ++slot) {
        bsd_nic_slot_t *nic = &nics[slot];
        if (!nic->attached) continue;
        nl_link_out_t *entry = &out[count++];
        memset(entry, 0, sizeof(*entry));
        nic_name_for_slot(slot, entry->name, sizeof(entry->name));
        entry->index = nic_index_for_slot(slot);
        entry->mtu = nic->mtu;
        entry->flags = (nic->up ? NLRT_IF_FLAGS_UP | NLRT_IF_FLAGS_RUNNING : 0);
        memcpy(entry->mac, nic->mac, 6);
        if (nic->has_ip) {
            entry->has_ip = 1;
            memcpy(entry->ip, nic->ip, 4);
            memcpy(entry->mask, nic->mask, 4);
            if (nic->gw[0] || nic->gw[1] || nic->gw[2] || nic->gw[3]) {
                entry->has_gw = 1;
                memcpy(entry->gw, nic->gw, 4);
            }
        }
    }
    if (count < maximum) {
        nl_link_out_t *entry = &out[count++];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->name, "lo0", 4);
        entry->index = 2;
        entry->mtu = 16384;
        entry->flags = NLRT_IF_FLAGS_UP | NLRT_IF_FLAGS_RUNNING |
                       NLRT_IF_FLAGS_LOOPBACK;
        entry->loopback = 1;
        entry->has_ip = 1;
        entry->ip[0] = 127; entry->ip[3] = 1;
        entry->mask[0] = 255;
    }
    if (count < maximum)
        count += a20_bsd_tun_snapshot(&out[count], maximum - count);
    return count;
}

static int if_default_index(void)
{
    for (int slot = 0; slot < BSD_MAX_NICS; ++slot)
        if (nics[slot].attached) return (int)nic_index_for_slot(slot);
    return 2;
}
static int if_up(unsigned index)
{
    if (a20_bsd_tun_is_index(index)) return a20_bsd_tun_up(index);
    int slot = nic_slot_for_index(index);
    if (slot >= 0) return nics[slot].attached && nics[slot].up;
    if (index == 2) return 1;
    return 0;
}
static int if_mtu(unsigned index)
{
    if (a20_bsd_tun_is_index(index)) return a20_bsd_tun_mtu(index);
    int slot = nic_slot_for_index(index);
    if (slot >= 0) return nics[slot].attached ? nics[slot].mtu : -ENODEV;
    if (index == 2) return 16384;
    return -ENODEV;
}
static int if_hwaddr(unsigned index, uint8_t *out)
{
    if (a20_bsd_tun_is_index(index)) return -EOPNOTSUPP;
    if (!out) return -EINVAL;
    int slot = nic_slot_for_index(index);
    if (slot >= 0) {
        if (!nics[slot].attached) return -ENODEV;
        memcpy(out, nics[slot].mac, 6);
        return 0;
    }
    if (index == 2) {
        memset(out, 0, 6);
        return 0;
    }
    return -ENODEV;
}

static int if_get_addr(unsigned index, uint8_t *address, uint8_t *mask, uint8_t *gateway)
{
    if (a20_bsd_tun_is_index(index))
        return a20_bsd_tun_get_addr(index, address, mask, gateway);
    int slot = nic_slot_for_index(index);
    if (slot >= 0) {
        if (!nics[slot].attached) return -ENODEV;
        if (address) memcpy(address, nics[slot].ip, 4);
        if (mask) memcpy(mask, nics[slot].mask, 4);
        if (gateway) memcpy(gateway, nics[slot].gw, 4);
        return 0;
    }
    if (index == 2) {
        if (address) { memset(address, 0, 4); address[0] = 127; address[3] = 1; }
        if (mask) { memset(mask, 0, 4); mask[0] = 255; }
        if (gateway) memset(gateway, 0, 4);
        return 0;
    }
    return -ENODEV;
}

typedef struct mtu_job {
    void *context;
    int mtu;
} mtu_job_t;

static int mtu_job_run(void *argument)
{
    mtu_job_t *job = argument;
    return a20_bsd_port_nic_set_mtu(job->context, job->mtu);
}

typedef struct link_job {
    bsd_nic_slot_t *nic;
    unsigned index;
    int up;
} link_job_t;

static int link_job_run(void *argument)
{
    link_job_t *job = argument;
    if (job->up == (int)job->nic->up)
        return 0;
    int result = a20_bsd_port_nic_set_up(job->nic->context, job->up);
    if (result == 0) {
        job->nic->up = job->up;
        net_netlink_link_notify_one(job->index, job->up);
    }
    return result;
}

typedef struct addr4_job {
    void *context;
    uint8_t address[4], mask[4], gateway[4];
    uint8_t old_address[4], old_mask[4], old_gateway[4];
    int have_gateway;
} addr4_job_t;

typedef struct route4_job {
    void *context;
    uint8_t destination[4], gateway[4];
    uint8_t prefix;
    int remove;
    int have_gateway;
} route4_job_t;

static int addr4_job_run(void *argument)
{
    addr4_job_t *job = argument;
    return a20_bsd_port_nic_set_ipv4(
        job->context, job->address, job->mask,
        job->have_gateway ? job->gateway : NULL,
        job->old_address, job->old_mask, job->old_gateway);
}

static int route4_job_run(void *argument)
{
    route4_job_t *job = argument;
    return a20_bsd_port_nic_route4(job->context, job->destination,
                                   job->prefix,
                                   job->have_gateway ? job->gateway : NULL,
                                   job->remove);
}

static int route_update_v4(const uint8_t *destination, uint8_t prefix,
                           const uint8_t *gateway, uint16_t ifindex,
                           int remove)
{
    if (!destination || prefix > 32) return -EINVAL;
    if (a20_bsd_tun_is_index(ifindex)) {
        if (gateway) return -EOPNOTSUPP;
        return a20_bsd_tun_route4(ifindex, destination, prefix, remove);
    }
    int slot = ifindex ? nic_slot_for_index(ifindex) : 0;
    if (slot < 0 || !nics[slot].attached) return -ENODEV;
    route4_job_t job;
    memset(&job, 0, sizeof(job));
    job.context = nics[slot].context;
    memcpy(job.destination, destination, 4);
    job.prefix = prefix;
    job.remove = remove;
    if (gateway) {
        memcpy(job.gateway, gateway, 4);
        job.have_gateway = 1;
    } else if (prefix == 0 && memcmp(destination, "\0\0\0\0", 4) == 0) {
        /* `ip route del default` commonly carries no RTA_GATEWAY.  Match the
         * gateway observed on the primary or the selected interface. */
        memcpy(job.gateway, nics[slot].gw, 4);
        job.have_gateway = nics[slot].gw[0] || nics[slot].gw[1] ||
                           nics[slot].gw[2] || nics[slot].gw[3];
    }
    if (!job.have_gateway && !remove) return 0; /* connected routes are ifaddr-owned */
    return a20_bsd_executor_call(route4_job_run, &job);
}

static int if_set_addr(unsigned index, const uint8_t *address,
                       const uint8_t *mask, const uint8_t *gateway)
{
    if (a20_bsd_tun_is_index(index))
        return a20_bsd_tun_set_addr(index, address, mask, 1);
    int slot = nic_slot_for_index(index);
    if (slot < 0 || !nics[slot].attached || !address || !mask) return -ENODEV;
    bsd_nic_slot_t *nic = &nics[slot];
    addr4_job_t job;
    memset(&job, 0, sizeof(job));
    job.context = nic->context;
    memcpy(job.address, address, 4);
    memcpy(job.mask, mask, 4);
    memcpy(job.old_address, nic->ip, 4);
    memcpy(job.old_mask, nic->mask, 4);
    memcpy(job.old_gateway, nic->gw, 4);
    if (gateway) {
        memcpy(job.gateway, gateway, 4);
        job.have_gateway = 1;
    } else if (slot == 0) {
        memcpy(job.gateway, nic->gw, 4);
        job.have_gateway = nic->gw[0] || nic->gw[1] || nic->gw[2] || nic->gw[3];
    }
    int result = a20_bsd_executor_call(addr4_job_run, &job);
    if (result < 0) return result;

    memcpy(nic->ip, address, 4);
    memcpy(nic->mask, mask, 4);
    nic->has_ip = address[0] || address[1] || address[2] || address[3];
    if (gateway) memcpy(nic->gw, gateway, 4);

    if (slot == 0) {
        uint64_t flags = spin_lock_irqsave(&config_lock);
        memcpy(&g_a20_net_config.ip, address, 4);
        memcpy(&g_a20_net_config.netmask, mask, 4);
        if (gateway) memcpy(&g_a20_net_config.gateway, gateway, 4);
        spin_unlock_irqrestore(&config_lock, flags);
    }
    return 0;
}

static int if_set_mtu(unsigned index, uint16_t mtu)
{
    if (a20_bsd_tun_is_index(index)) return a20_bsd_tun_set_mtu(index, mtu);
    int slot = nic_slot_for_index(index);
    if (slot < 0 || !nics[slot].attached) return -ENODEV;
    if (mtu < 576 || mtu > 1500) return -EINVAL;
    if (mtu == nics[slot].mtu) return 0;
    mtu_job_t job = {.context = nics[slot].context, .mtu = mtu};
    int result = a20_bsd_executor_call(mtu_job_run, &job);
    if (result == 0) nics[slot].mtu = mtu;
    return result;
}

static int if_set_flags(unsigned index, uint32_t value, uint32_t mask)
{
    if (a20_bsd_tun_is_index(index))
        return a20_bsd_tun_set_flags(index, value, mask);
    if (index == 2) return (value & NLRT_IF_FLAGS_UP) ? 0 : -EOPNOTSUPP;
    int slot = nic_slot_for_index(index);
    if (slot < 0 || !nics[slot].attached) return -ENODEV;
    if (mask & ~(uint32_t)NLRT_IF_FLAGS_UP) return -EOPNOTSUPP;
    int up = (value & NLRT_IF_FLAGS_UP) != 0;
    link_job_t job = {.nic = &nics[slot], .index = index, .up = up};
    return a20_bsd_executor_call(link_job_run, &job);
}

typedef struct addr6_job {
    void *context;
    uint8_t address[16];
    uint8_t prefix;
    int add, exclusive;
    uint32_t valid, preferred;
} addr6_job_t;

static int addr6_job_run(void *argument)
{
    addr6_job_t *job = argument;
    int rc = a20_bsd_port_nic_set_ipv6(job->context, job->address,
                                       job->prefix, job->add, job->valid,
                                       job->preferred, job->exclusive);
    return rc < 0 ? rc : -rc;
}

typedef struct neighbor_job {
    void *context;
    uint32_t address;
    uint8_t mac[6];
} neighbor_job_t;

static int neighbor_job_run(void *argument)
{
    neighbor_job_t *job = argument;
    int rc = a20_bsd_port_nic_set_neighbor(job->context, job->address,
                                           job->mac);
    return rc < 0 ? rc : -rc;
}

static int if_addr6(unsigned index, const uint8_t *address, uint8_t prefix,
                    int add, uint32_t valid, uint32_t preferred, int exclusive)
{
    if (a20_bsd_tun_is_index(index))
        return a20_bsd_tun_set_addr6(index, address, prefix, add, valid, preferred, exclusive);
    int slot = nic_slot_for_index(index);
    if (slot < 0 || slot >= BSD_MAX_NICS || !nics[slot].context || !address)
        return -ENODEV;
    addr6_job_t job;
    memset(&job, 0, sizeof(job));
    job.context = nics[slot].context;
    memcpy(job.address, address, 16);
    job.prefix = prefix;
    job.add = add;
    job.exclusive = exclusive;
    job.valid = valid;
    job.preferred = preferred;
    return a20_bsd_executor_call(addr6_job_run, &job);
}

static int if_set_addr6(unsigned index, const uint8_t *address, uint8_t prefix,
                        uint32_t valid, uint32_t preferred, int exclusive)
{
    return if_addr6(index, address, prefix, 1, valid, preferred, exclusive);
}
static int if_del_addr6(unsigned index, const uint8_t *address)
{
    return if_addr6(index, address, 128, 0, 0, 0, 0);
}
static int neighbor_update_v4(uint32_t index, uint32_t address, const uint8_t mac[6])
{
    if (!mac) return -EINVAL;
    int slot = nic_slot_for_index(index);
    if (slot < 0 || !nics[slot].context) return -ENODEV;
    neighbor_job_t job;
    memset(&job, 0, sizeof(job));
    job.context = nics[slot].context;
    job.address = address;
    memcpy(job.mac, mac, 6);
    return a20_bsd_executor_call(neighbor_job_run, &job);
}

static int owns_ipv4(uint32_t address)
{
    /* Linux wildcard bind selects a local address later; it is not an attempt
     * to bind a foreign unicast address. */
    if (address == 0) return 1;
    if (a20_bsd_tun_owns_ipv4(address)) return 1;
    for (int slot = 0; slot < BSD_MAX_NICS; ++slot) {
        if (!nics[slot].attached || !nics[slot].has_ip) continue;
        uint32_t wire;
        memcpy(&wire, nics[slot].ip, sizeof(wire));
        if (wire == address) return 1;
    }
    uint8_t loopback[4] = {127, 0, 0, 1};
    uint32_t value;
    memcpy(&value, loopback, 4);
    return address == value;
}

typedef struct raw_tx_job {
    void *context;
    const uint8_t *frame;
    size_t length;
} raw_tx_job_t;

static int raw_tx_job_run(void *argument)
{
    raw_tx_job_t *job = argument;
    int rc = a20_bsd_port_nic_send_raw(job->context, job->frame, job->length);
    return rc < 0 ? rc : -rc;
}

static int packet_tx(unsigned index, const uint8_t *frame, size_t length)
{
    int slot = nic_slot_for_index(index);
    if (slot < 0 || !nics[slot].context || !frame) return -ENODEV;
    raw_tx_job_t job = {nics[slot].context, frame, length};
    return a20_bsd_executor_call(raw_tx_job_run, &job);
}

static uint32_t interface_ipv4(int index)
{
    /* netfilter passes the device slot as net_idx; TUN sits after the NIC
     * slots in the same space. */
    if (index >= 0 && index < BSD_MAX_NICS) {
        if (!nics[index].attached || !nics[index].has_ip) return 0;
        const uint8_t *a = nics[index].ip;
        return (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 |
               (uint32_t)a[2] << 8 | a[3];
    }
    int tun_slot = index - BSD_MAX_NICS;
    if (tun_slot >= 0 && tun_slot < NET_PROFILE_TUN_MAX_DEVS)
        return a20_bsd_tun_interface_ipv4(
            (unsigned)(A20_BSD_TUN_BASE + tun_slot));
    return 0;
}

static void config_snapshot(a20_net_config_t *out)
{
    uint64_t flags = spin_lock_irqsave(&config_lock);
    *out = g_a20_net_config;
    spin_unlock_irqrestore(&config_lock, flags);
}

static int format_status(char *buffer, size_t size)
{
    int used = snprintf(buffer, size, "executor: %s\n",
                        a20_bsd_executor_ready() ? "single-owner" : "not-ready");
    if (used < 0 || (size_t)used >= size) return used;
    return used + a20_bsd_socket_format_status(buffer + used, size - (size_t)used);
}

static int format_memory(char *buffer, size_t size)
{
    struct bsd_memory_stats memory;
    a20_bsd_port_memory_snapshot(&memory);
    return snprintf(buffer, size,
                    "BSD backing: limit=%lu used=%lu peak=%lu allocations=%lu failures=%lu\n",
                    (unsigned long)memory.limit, (unsigned long)memory.used,
                    (unsigned long)memory.peak, (unsigned long)memory.allocations,
                    (unsigned long)memory.failures);
}

typedef struct stats_job {
    void *context;
    a20_bsd_nic_stats_t stats;
} stats_job_t;

static int stats_job_run(void *argument)
{
    stats_job_t *job = argument;
    a20_bsd_port_nic_stats(job->context, &job->stats);
    return 0;
}

static int format_devices(char *buffer, size_t size)
{
    size_t used = 0;
    for (int slot = 0; slot < BSD_MAX_NICS; ++slot) {
        char name[16];
        nic_name_for_slot(slot, name, sizeof(name));
        if (!nics[slot].present && slot != 0) continue;
        if (!nics[slot].context) {
            if (slot == 0)
                used += snprintf(buffer + used, size > used ? size - used : 0,
                                 "%s: detached\n", name);
            continue;
        }
        stats_job_t job = {.context = nics[slot].context};
        if (a20_bsd_executor_call(stats_job_run, &job) < 0) {
            used += snprintf(buffer + used, size > used ? size - used : 0,
                             "%s: statistics unavailable\n", name);
            continue;
        }
        used += snprintf(buffer + used, size > used ? size - used : 0,
                         "%s: rx=%lu tx=%lu rx_drop=%lu tx_drop=%lu tx_error=%lu rx_starved=%lu\n",
                         name,
                         (unsigned long)job.stats.rx_frames,
                         (unsigned long)job.stats.tx_frames,
                         (unsigned long)job.stats.rx_drops,
                         (unsigned long)job.stats.tx_drops,
                         (unsigned long)job.stats.tx_errors,
                         (unsigned long)job.stats.rx_starved);
        if (used >= size) break;
    }
    return (int)used;
}

static int format_routes(char *buffer, size_t size)
{
    int slot = -1;
    for (int i = 0; i < BSD_MAX_NICS; ++i)
        if (nics[i].attached) { slot = i; break; }
    if (slot < 0) return snprintf(buffer, size, "no BSD route\n");
    char gateway[16], name[16];
    ipv4_text(gateway, nics[slot].gw);
    nic_name_for_slot(slot, name, sizeof(name));
    return snprintf(buffer, size, "default via %s dev %s\n", gateway, name);
}

static const net_backend_cycle_ops_t bsd_cycle_ops = {
    .init = stack_init,
    .attach = attach_interfaces,
};

static const net_backend_event_ops_t bsd_event_ops = {
    .rx_progress = receive_progress,
    .irq = device_interrupt,
    .protocol_progress = protocol_progress,
    .timer_tick = timer_tick,
    .work_pending = work_pending,
    .poll_waiter = poll_waiter,
};

typedef struct {
    nl_addr6_out_t *out;
    int maximum;
} addr6_snapshot_job_t;

static int addr6_snapshot_run(void *argument)
{
    addr6_snapshot_job_t *job = argument;
    int count = a20_bsd_port_addr6_snapshot(job->out, job->maximum);
    for (int i = 0; i < count; ++i)
        job->out[i].index = public_index_for_slot((int)job->out[i].index - 1);
    return count;
}

static int addr6_snapshot(nl_addr6_out_t *out, int maximum)
{
    if (!a20_bsd_executor_ready())
        return 0;
    addr6_snapshot_job_t job = {out, maximum};
    return a20_bsd_executor_call(addr6_snapshot_run, &job);
}

typedef struct {
    const uint8_t *destination;
    unsigned index;
    nl_route6_out_t *out;
} route_lookup6_job_t;

static int route_lookup6_run(void *argument)
{
    route_lookup6_job_t *job = argument;
    int zone = job->index ? a20_bsd_ifindex_to_bsd(job->index) : 0;
    if (zone < 0)
        return zone;
    int rc = a20_bsd_port_route_lookup_v6(job->destination, zone, job->out);
    if (rc == 0)
        job->out->index = public_index_for_slot((int)job->out->index - 1);
    return rc;
}

static int route_lookup_v6(const uint8_t destination[16], unsigned index, nl_route6_out_t *out)
{
    if (!a20_bsd_executor_ready())
        return -ENETUNREACH;
    route_lookup6_job_t job = {destination, index, out};
    return a20_bsd_executor_call(route_lookup6_run, &job);
}

static const net_backend_if_ops_t bsd_iface_ops = {
    .up = if_up,
    .default_index = if_default_index,
    .owns_ipv4 = owns_ipv4,
    .mtu = if_mtu,
    .hwaddr = if_hwaddr,
    .snapshot = interface_snapshot,
    .snapshot_addr6 = addr6_snapshot,
    .config_snapshot = config_snapshot,
    .set_mtu = if_set_mtu,
    .set_flags = if_set_flags,
    .get_addr = if_get_addr,
    .set_addr = if_set_addr,
    .set_addr6 = if_set_addr6,
    .del_addr6 = if_del_addr6,
    .route_lookup_v6 = route_lookup_v6,
    .route_update_v4 = route_update_v4,
    .neighbor_update_v4 = neighbor_update_v4,
    .interface_ipv4 = interface_ipv4,
};

static int tun_create(net_tun_emit_t emit, void *ctx)
{
    return a20_bsd_tun_create(emit, ctx);
}
static void tun_destroy(unsigned index) { a20_bsd_tun_destroy(index); }
static int tun_inject(unsigned index, const uint8_t *packet, size_t length)
{
    return a20_bsd_tun_inject(index, packet, length);
}

static const net_backend_packet_ops_t bsd_packet_ops = {
    .tx = packet_tx,
    .tun_create = tun_create,
    .tun_destroy = tun_destroy,
    .tun_inject = tun_inject,
};

static const net_backend_obs_ops_t bsd_obs_ops = {
    .status = format_status,
    .memory = format_memory,
    .devices = format_devices,
    .routes = format_routes,
};

/*
 * BSD currently keeps the desktop/host vertical slice: stream, datagram,
 * local IPv6, dynamic IPv4/MTU/link and a backend RIB.  Raw, TUN, per-datagram
 * control, PMTU and neighbour injection are deliberately absent so the
 * frontend returns -EOPNOTSUPP instead of pretending.
 */
const net_backend_t a20_bsd_backend = {
    .name = "bsd",
    .caps = NET_CAP_STREAM | NET_CAP_DGRAM | NET_CAP_IPV6 |
            NET_CAP_LISTEN | NET_CAP_SHUTDOWN |
            NET_CAP_SNDBUF | NET_CAP_RCVBUF |
            NET_CAP_PKTINFO | NET_CAP_UDP_CONTROL | NET_CAP_TUN |
            NET_CAP_BINDTODEVICE |
            NET_CAP_IF_DYNAMIC | NET_CAP_IF_ADDR6 | NET_CAP_ROUTE |
            NET_CAP_PMTU | NET_CAP_RAW | NET_CAP_RAW_TX | NET_CAP_RAW_IPV6_TX |
            NET_CAP_CONGESTION | NET_CAP_DEVICE_IRQ,
    .cycle = &bsd_cycle_ops,
    .event = &bsd_event_ops,
    .socket = &a20_bsd_socket_ops,
    .opts = &a20_bsd_socket_opt_ops,
    .endpoint = &a20_bsd_endpoint_ops,
    .iface = &bsd_iface_ops,
    .packet = &bsd_packet_ops,
    .obs = &bsd_obs_ops,
};

void a20_bsd_backend_register(void)
{
    (void)net_backend_register(&a20_bsd_backend);
}
