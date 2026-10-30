/* Selectable BSD stack: class NIC ownership, real-time callout progress and
 * the public net_backend contract. Protocol execution itself remains
 * in the isolated core. */
#include "executor.h"
#include "protocol.h"
#include "runtime/memory.h"
#include "net/stack.h"
#include "net/profile.h"
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
extern int a20_bsd_socket_format_status(char *, size_t);
extern void a20_bsd_port_memory_snapshot(struct bsd_memory_stats *);

typedef struct bsd_nic_slot {
    device_t *device;
    class_device_t *binding;
    void *context;
    unsigned attached;
    unsigned rx_pending;
    unsigned up;
    uint16_t mtu;
    uint8_t mac[6];
} bsd_nic_slot_t;

static bsd_nic_slot_t nics[BSD_MAX_NICS];
static spinlock_t nic_lock = SPINLOCK_INIT;
static spinlock_t config_lock = SPINLOCK_INIT;
static unsigned progress_budget = BSD_RX_BUDGET;
static uint64_t clock_last;
static unsigned clock_due;
static void config_snapshot(a20_net_config_t *out);
static int stack_ready;

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

    unsigned budget = __atomic_exchange_n(&progress_budget, BSD_RX_BUDGET,
                                          __ATOMIC_ACQ_REL);
    for (unsigned i = 0; i < BSD_MAX_NICS; ++i) {
        if (!nics[i].context) continue;
        __atomic_store_n(&nics[i].rx_pending, 0, __ATOMIC_RELEASE);
        a20_bsd_port_nic_progress(nics[i].context, budget);
    }
    a20_bsd_port_protocol_progress();
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
    if (nics[0].attached) return;

    a20_net_config_t config;
    uint64_t flags = spin_lock_irqsave(&config_lock);
    config = g_a20_net_config;
    spin_unlock_irqrestore(&config_lock, flags);
    if (!config.ip.addr || !config.netmask.addr || !config.gateway.addr) {
        printf("[NET] BSD initial backend requires static IPv4 address/netmask/gateway\n");
        return;
    }

    attach_job_t job;
    memset(&job, 0, sizeof(job));
    job.config.port_id = 0;
    memcpy(job.config.ifname, "a20n0", 6);
    result = copy_mac(0, job.config.mac);
    if (result < 0) {
        printf("[NET] BSD found no usable class NIC: %d\n", result);
        return;
    }
    memcpy(nics[0].mac, job.config.mac, 6);
    ipv4_text(job.config.address, &config.ip);
    ipv4_text(job.config.netmask, &config.netmask);
    ipv4_text(job.config.gateway, &config.gateway);
    uint8_t broadcast[4];
    const uint8_t *ip = (const uint8_t *)&config.ip;
    const uint8_t *mask = (const uint8_t *)&config.netmask;
    for (unsigned i = 0; i < 4; ++i) broadcast[i] = ip[i] | (uint8_t)~mask[i];
    ipv4_text(job.config.broadcast, broadcast);
    result = a20_bsd_executor_call(attach_job_run, &job);
    if (result < 0) {
        printf("[NET] BSD ifnet attach failed: %d\n", result);
        return;
    }
    nics[0].context = job.context;
    nics[0].attached = 1;
    nics[0].up = 1;
    nics[0].mtu = 1500;
    clock_last = timer_get_ticks();
    printf("[NET] BSD attached a20n0 %s/%s gateway %s\n",
           job.config.address, job.config.netmask, job.config.gateway);
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
    if (count < maximum && nics[0].attached) {
        nl_link_out_t *entry = &out[count++];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->name, "a20n0", 6);
        entry->index = 1;
        entry->mtu = nics[0].mtu;
        entry->flags = (nics[0].up ? NLRT_IF_FLAGS_UP | NLRT_IF_FLAGS_RUNNING : 0);
        memcpy(entry->mac, nics[0].mac, 6);
        entry->has_ip = entry->has_gw = 1;
        memcpy(entry->ip, &g_a20_net_config.ip, 4);
        memcpy(entry->mask, &g_a20_net_config.netmask, 4);
        memcpy(entry->gw, &g_a20_net_config.gateway, 4);
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
    return count;
}

static int if_default_index(void) { return nics[0].attached ? 1 : 2; }
static int if_up(unsigned index)
{
    if (index == 1) return nics[0].attached && nics[0].up;
    if (index == 2) return 1;
    return 0;
}
static int if_mtu(unsigned index)
{
    if (index == 1) return nics[0].attached ? nics[0].mtu : -ENODEV;
    if (index == 2) return 16384;
    return -ENODEV;
}
static int if_hwaddr(unsigned index, uint8_t *out)
{
    if (!out) return -EINVAL;
    if (index == 1 && nics[0].attached) {
        memcpy(out, nics[0].mac, 6);
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
    if (index == 1 && nics[0].attached) {
        memcpy(address, &g_a20_net_config.ip, 4);
        memcpy(mask, &g_a20_net_config.netmask, 4);
        memcpy(gateway, &g_a20_net_config.gateway, 4);
        return 0;
    }
    if (index == 2) {
        memset(address, 0, 4); memset(mask, 0, 4); memset(gateway, 0, 4);
        address[0] = 127; address[3] = 1; mask[0] = 255;
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
    void *context;
    int up;
} link_job_t;

static int link_job_run(void *argument)
{
    link_job_t *job = argument;
    return a20_bsd_port_nic_set_up(job->context, job->up);
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
    if (!destination || !nics[0].attached) return -ENODEV;
    if (ifindex != 1) return -ENODEV;
    if (prefix > 32) return -EINVAL;
    route4_job_t job;
    memset(&job, 0, sizeof(job));
    job.context = nics[0].context;
    memcpy(job.destination, destination, 4);
    job.prefix = prefix;
    job.remove = remove;
    if (gateway) {
        memcpy(job.gateway, gateway, 4);
        job.have_gateway = 1;
    } else if (prefix == 0 && memcmp(destination, "\0\0\0\0", 4) == 0) {
        /* `ip route del default` commonly carries no RTA_GATEWAY.  Match the
         * backend route installed from the configured default gateway. */
        a20_net_config_t config;
        config_snapshot(&config);
        memcpy(job.gateway, &config.gateway, 4);
        job.have_gateway = config.gateway.addr != 0;
    }
    if (!job.have_gateway && !remove) return 0; /* connected routes are ifaddr-owned */
    return a20_bsd_executor_call(route4_job_run, &job);
}

static int if_set_addr(unsigned index, const uint8_t *address,
                       const uint8_t *mask, const uint8_t *gateway)
{
    if (index != 1 || !nics[0].attached || !address || !mask) return -ENODEV;
    a20_net_config_t config;
    config_snapshot(&config);
    addr4_job_t job;
    memset(&job, 0, sizeof(job));
    job.context = nics[0].context;
    memcpy(job.address, address, 4);
    memcpy(job.mask, mask, 4);
    memcpy(job.old_address, &config.ip, 4);
    memcpy(job.old_mask, &config.netmask, 4);
    memcpy(job.old_gateway, &config.gateway, 4);
    if (gateway) {
        memcpy(job.gateway, gateway, 4);
        job.have_gateway = 1;
    } else {
        memcpy(job.gateway, &config.gateway, 4);
        job.have_gateway = config.gateway.addr != 0;
    }
    int result = a20_bsd_executor_call(addr4_job_run, &job);
    if (result < 0) return result;

    uint64_t flags = spin_lock_irqsave(&config_lock);
    memcpy(&g_a20_net_config.ip, address, 4);
    memcpy(&g_a20_net_config.netmask, mask, 4);
    if (gateway) memcpy(&g_a20_net_config.gateway, gateway, 4);
    spin_unlock_irqrestore(&config_lock, flags);
    return 0;
}

static int if_set_mtu(unsigned index, uint16_t mtu)
{
    if (index != 1 || !nics[0].attached) return -ENODEV;
    if (mtu < 576 || mtu > 1500) return -EINVAL;
    if (mtu == nics[0].mtu) return 0;
    mtu_job_t job = {.context = nics[0].context, .mtu = mtu};
    int result = a20_bsd_executor_call(mtu_job_run, &job);
    if (result == 0) nics[0].mtu = mtu;
    return result;
}

static int if_set_flags(unsigned index, uint32_t value, uint32_t mask)
{
    if (index == 2) return (value & NLRT_IF_FLAGS_UP) ? 0 : -EOPNOTSUPP;
    if (index != 1 || !nics[0].attached) return -ENODEV;
    if (mask & ~(uint32_t)NLRT_IF_FLAGS_UP) return -EOPNOTSUPP;
    int up = (value & NLRT_IF_FLAGS_UP) != 0;
    if (up == (int)nics[0].up) return 0;
    link_job_t job = {.context = nics[0].context, .up = up};
    int result = a20_bsd_executor_call(link_job_run, &job);
    if (result == 0) nics[0].up = up ? 1 : 0;
    return result;
}

typedef struct addr6_job {
    void *context;
    uint8_t address[16];
    uint8_t prefix;
    int add;
} addr6_job_t;

static int addr6_job_run(void *argument)
{
    addr6_job_t *job = argument;
    int rc = a20_bsd_port_nic_set_ipv6(job->context, job->address,
                                       job->prefix, job->add);
    return rc < 0 ? rc : -rc;
}

static int if_addr6(unsigned index, const uint8_t *address, uint8_t prefix,
                    int add)
{
    if (index != 0 || !nics[0].context || !address) return -ENODEV;
    addr6_job_t job;
    memset(&job, 0, sizeof(job));
    job.context = nics[0].context;
    memcpy(job.address, address, 16);
    job.prefix = prefix;
    job.add = add;
    return a20_bsd_executor_call(addr6_job_run, &job);
}

static int if_set_addr6(unsigned index, const uint8_t *address, uint8_t prefix)
{
    return if_addr6(index, address, prefix, 1);
}
static int if_del_addr6(unsigned index, const uint8_t *address)
{
    return if_addr6(index, address, 128, 0);
}
static int neighbor_update_v4(uint32_t index, uint32_t address, const uint8_t mac[6])
{ (void)index; (void)address; (void)mac; return -EOPNOTSUPP; }

static int owns_ipv4(uint32_t address)
{
    /* Linux wildcard bind selects a local address later; it is not an attempt
     * to bind a foreign unicast address. */
    if (address == 0) return 1;
    if (nics[0].attached && address == g_a20_net_config.ip.addr) return 1;
    uint8_t loopback[4] = {127, 0, 0, 1};
    uint32_t value;
    memcpy(&value, loopback, 4);
    return address == value;
}

static int packet_tx(unsigned index, const uint8_t *frame, size_t length)
{
    (void)index; (void)frame; (void)length;
    return -EOPNOTSUPP;
}

static uint32_t interface_ipv4(int index)
{
    if (index != 0 || !nics[0].attached) return 0;
    const uint8_t *a = (const uint8_t *)&g_a20_net_config.ip;
    return (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 |
           (uint32_t)a[2] << 8 | a[3];
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
    if (!nics[0].context) return snprintf(buffer, size, "a20n0: detached\n");
    stats_job_t job = {.context = nics[0].context};
    if (a20_bsd_executor_call(stats_job_run, &job) < 0)
        return snprintf(buffer, size, "a20n0: statistics unavailable\n");
    return snprintf(buffer, size,
                    "a20n0: rx=%lu tx=%lu rx_drop=%lu tx_drop=%lu tx_error=%lu rx_starved=%lu\n",
                    (unsigned long)job.stats.rx_frames, (unsigned long)job.stats.tx_frames,
                    (unsigned long)job.stats.rx_drops, (unsigned long)job.stats.tx_drops,
                    (unsigned long)job.stats.tx_errors,
                    (unsigned long)job.stats.rx_starved);
}

static int format_routes(char *buffer, size_t size)
{
    if (!nics[0].attached) return snprintf(buffer, size, "no BSD route\n");
    char gateway[16];
    ipv4_text(gateway, &g_a20_net_config.gateway);
    return snprintf(buffer, size, "default via %s dev a20n0\n", gateway);
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

static const net_backend_if_ops_t bsd_iface_ops = {
    .up = if_up,
    .default_index = if_default_index,
    .owns_ipv4 = owns_ipv4,
    .mtu = if_mtu,
    .hwaddr = if_hwaddr,
    .snapshot = interface_snapshot,
    .config_snapshot = config_snapshot,
    .set_mtu = if_set_mtu,
    .set_flags = if_set_flags,
    .get_addr = if_get_addr,
    .set_addr = if_set_addr,
    .set_addr6 = if_set_addr6,
    .del_addr6 = if_del_addr6,
    .route_update_v4 = route_update_v4,
    .neighbor_update_v4 = neighbor_update_v4,
    .interface_ipv4 = interface_ipv4,
};

static const net_backend_packet_ops_t bsd_packet_ops = {
    .tx = packet_tx,
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
            NET_CAP_IF_DYNAMIC | NET_CAP_IF_ADDR6 | NET_CAP_ROUTE |
            NET_CAP_PMTU | NET_CAP_RAW | NET_CAP_RAW_TX |
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
