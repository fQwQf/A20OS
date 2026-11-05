/* A20OS-side TUN slot/index manager for the BSD backend.
 *
 * Public interface indices are stable: 1 is a20n0, 2 is loopback, and TUN
 * instances start at 3.  All actual BSD calls are executor jobs; the emit
 * callback is the A20OS TUN driver's queue writer and runs on that task.
 */
#include "executor.h"
#include "protocol.h"
#include "tun.h"
#include "net/tun.h"
#include "net/interface.h"
#include "net/profile.h"
#include "net/route.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/stdio.h"

#define BSD_TUN_BASE A20_BSD_TUN_BASE
#define BSD_TUN_MAX NET_PROFILE_TUN_MAX_DEVS

typedef struct bsd_tun_slot {
    int used;
    int unit;
    unsigned index;
    void *context;
    char name[16];
} bsd_tun_slot_t;

static bsd_tun_slot_t g_tuns[BSD_TUN_MAX];
/* used: 0 free, 2 reserved by a creator, 1 live. */
static spinlock_t g_tun_lock = SPINLOCK_INIT;

static int valid_index(unsigned index)
{
    return index >= BSD_TUN_BASE && index < BSD_TUN_BASE + BSD_TUN_MAX;
}

int a20_bsd_tun_is_index(unsigned index)
{
    return valid_index(index) &&
           __atomic_load_n(&g_tuns[index - BSD_TUN_BASE].used, __ATOMIC_ACQUIRE) == 1;
}

static bsd_tun_slot_t *slot_of(unsigned index)
{
    if (!a20_bsd_tun_is_index(index)) return NULL;
    return &g_tuns[index - BSD_TUN_BASE];
}

typedef struct tun_create_job {
    int unit;
    a20_bsd_tun_emit_t emit;
    void *ctx;
    void *result;
} tun_create_job_t;

static int tun_create_job_run(void *argument)
{
    tun_create_job_t *job = argument;
    job->result = a20_bsd_port_tun_create(
        job->unit, NET_PROFILE_NETIF_MAX_DEVS + job->unit, job->emit, job->ctx);
    return job->result ? 0 : -ENOMEM;
}

int a20_bsd_tun_create(net_tun_emit_t emit, void *ctx)
{
    if (!emit) return -EINVAL;
    int unit = -1;
    uint64_t flags = spin_lock_irqsave(&g_tun_lock);
    for (int i = 0; i < BSD_TUN_MAX; ++i) {
        if (!g_tuns[i].used) {
            g_tuns[i].used = 2; /* reserve; no index is visible yet */
            unit = i;
            break;
        }
    }
    spin_unlock_irqrestore(&g_tun_lock, flags);
    if (unit < 0) return -ENOSPC;

    tun_create_job_t job = {unit, (a20_bsd_tun_emit_t)emit, ctx, NULL};
    int rc = a20_bsd_executor_call(tun_create_job_run, &job);
    if (rc < 0 || !job.result) {
        flags = spin_lock_irqsave(&g_tun_lock);
        g_tuns[unit].used = 0;
        spin_unlock_irqrestore(&g_tun_lock, flags);
        return rc < 0 ? rc : -ENOMEM;
    }

    bsd_tun_slot_t *slot = &g_tuns[unit];
    const char *name = a20_bsd_port_tun_name(job.result);
    flags = spin_lock_irqsave(&g_tun_lock);
    memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->unit = unit;
    slot->index = (unsigned)(BSD_TUN_BASE + unit);
    slot->context = job.result;
    if (name && name[0])
        strncpy(slot->name, name, sizeof(slot->name) - 1);
    else
        snprintf(slot->name, sizeof(slot->name), "tun%d", unit);
    spin_unlock_irqrestore(&g_tun_lock, flags);
    return (int)slot->index;
}

typedef struct tun_ctx_job { void *context; } tun_ctx_job_t;

static int tun_destroy_job_run(void *argument)
{
    tun_ctx_job_t *job = argument;
    a20_bsd_port_tun_destroy(job->context);
    return 0;
}

void a20_bsd_tun_destroy(unsigned index)
{
    bsd_tun_slot_t *slot = slot_of(index);
    if (!slot) return;
    uint64_t flags = spin_lock_irqsave(&g_tun_lock);
    void *context = slot->context;
    slot->used = 0;
    slot->context = NULL;
    spin_unlock_irqrestore(&g_tun_lock, flags);
    a20_net_route_flush_if((uint16_t)index);
    if (context && a20_bsd_executor_ready()) {
        tun_ctx_job_t job = {context};
        (void)a20_bsd_executor_call(tun_destroy_job_run, &job);
    }
}

typedef struct tun_inject_job {
    void *context;
    const uint8_t *packet;
    size_t length;
} tun_inject_job_t;

static int tun_inject_job_run(void *argument)
{
    tun_inject_job_t *job = argument;
    int rc = a20_bsd_port_tun_inject(job->context, job->packet, job->length);
    return rc < 0 ? rc : -rc;
}

int a20_bsd_tun_inject(unsigned index, const uint8_t *packet, size_t length)
{
    bsd_tun_slot_t *slot = slot_of(index);
    if (!slot) return -ENODEV;
    tun_inject_job_t job = {slot->context, packet, length};
    return a20_bsd_executor_call(tun_inject_job_run, &job);
}

typedef struct tun_mtu_job { void *context; uint16_t mtu; } tun_mtu_job_t;
static int tun_mtu_job_run(void *argument)
{
    tun_mtu_job_t *job = argument;
    int rc = a20_bsd_port_tun_set_mtu(job->context, job->mtu);
    return rc < 0 ? rc : -rc;
}

int a20_bsd_tun_set_mtu(unsigned index, uint16_t mtu)
{
    bsd_tun_slot_t *slot = slot_of(index);
    if (!slot) return -ENODEV;
    tun_mtu_job_t job = {slot->context, mtu};
    return a20_bsd_executor_call(tun_mtu_job_run, &job);
}

typedef struct tun_flags_job {
    void *context;
    uint32_t value, mask;
} tun_flags_job_t;
static int tun_flags_job_run(void *argument)
{
    tun_flags_job_t *job = argument;
    int rc = a20_bsd_port_tun_set_flags(job->context, job->value, job->mask);
    return rc < 0 ? rc : -rc;
}

int a20_bsd_tun_set_flags(unsigned index, uint32_t value, uint32_t mask)
{
    bsd_tun_slot_t *slot = slot_of(index);
    if (!slot) return -ENODEV;
    tun_flags_job_t job = {slot->context, value, mask};
    return a20_bsd_executor_call(tun_flags_job_run, &job);
}

typedef struct tun_info_job {
    void *context;
    a20_bsd_tun_info_t *out;
} tun_info_job_t;

static int tun_info_job_run(void *argument)
{
    tun_info_job_t *job = argument;
    int rc = a20_bsd_port_tun_snapshot(job->context, job->out);
    return rc < 0 ? rc : -rc;
}

static int tun_info(unsigned index, a20_bsd_tun_info_t *out)
{
    bsd_tun_slot_t *slot = slot_of(index);
    if (!slot || !out) return -ENODEV;
    tun_info_job_t job = {slot->context, out};
    return a20_bsd_executor_call(tun_info_job_run, &job);
}

int a20_bsd_tun_mtu(unsigned index)
{
    a20_bsd_tun_info_t info;
    if (tun_info(index, &info) < 0) return -ENODEV;
    return (int)info.mtu;
}

int a20_bsd_tun_up(unsigned index)
{
    a20_bsd_tun_info_t info;
    if (tun_info(index, &info) < 0) return 0;
    return (info.flags & 1u) != 0;
}

int a20_bsd_tun_get_addr(unsigned index, uint8_t address[4], uint8_t mask[4],
                         uint8_t gateway[4])
{
    a20_bsd_tun_info_t info;
    if (tun_info(index, &info) < 0) return -ENODEV;
    if (address) memcpy(address, info.ip, 4);
    if (mask) memcpy(mask, info.mask, 4);
    if (gateway) memcpy(gateway, info.gw, 4);
    return 0;
}

typedef struct tun_addr_job {
    void *context;
    uint8_t address[4], mask[4];
    int add;
} tun_addr_job_t;

static int tun_addr_job_run(void *argument)
{
    tun_addr_job_t *job = argument;
    int rc = a20_bsd_port_tun_set_addr(job->context, job->address,
                                       job->mask, job->add);
    return rc < 0 ? rc : -rc;
}

int a20_bsd_tun_set_addr(unsigned index, const uint8_t address[4],
                         const uint8_t mask[4], int add)
{
    bsd_tun_slot_t *slot = slot_of(index);
    if (!slot) return -ENODEV;
    tun_addr_job_t job;
    memset(&job, 0, sizeof(job));
    job.context = slot->context;
    if (address) memcpy(job.address, address, 4);
    if (mask) memcpy(job.mask, mask, 4);
    job.add = add;
    return a20_bsd_executor_call(tun_addr_job_run, &job);
}

typedef struct tun_route4_job {
    unsigned index;
    uint8_t destination[4];
    uint8_t prefix;
    int remove;
} tun_route4_job_t;

static int tun_route4_job_run(void *argument)
{
    tun_route4_job_t *job = argument;
    uint64_t flags = spin_lock_irqsave(&g_tun_lock);
    bsd_tun_slot_t *slot = slot_of(job->index);
    void *context = slot ? slot->context : NULL;
    spin_unlock_irqrestore(&g_tun_lock, flags);
    if (!context)
        return -ENODEV;
    return -a20_bsd_port_tun_route4(context, job->destination, job->prefix, job->remove);
}

int a20_bsd_tun_route4(unsigned index, const uint8_t destination[4],
                       uint8_t prefix, int remove)
{
    tun_route4_job_t job = {.index = index, .prefix = prefix, .remove = remove};
    memcpy(job.destination, destination, sizeof(job.destination));
    return a20_bsd_executor_call(tun_route4_job_run, &job);
}

typedef struct tun_addr6_job {
    void *context;
    uint8_t address[16];
    uint8_t prefix;
    int add;
} tun_addr6_job_t;

static int tun_addr6_job_run(void *argument)
{
    tun_addr6_job_t *job = argument;
    int rc = a20_bsd_port_tun_set_addr6(job->context, job->address,
                                        job->prefix, job->add);
    return rc < 0 ? rc : -rc;
}

int a20_bsd_tun_set_addr6(unsigned index, const uint8_t address[16],
                          uint8_t prefix, int add)
{
    bsd_tun_slot_t *slot = slot_of(index);
    if (!slot) return -ENODEV;
    tun_addr6_job_t job;
    memset(&job, 0, sizeof(job));
    job.context = slot->context;
    memcpy(job.address, address, 16);
    job.prefix = prefix;
    job.add = add;
    return a20_bsd_executor_call(tun_addr6_job_run, &job);
}

int a20_bsd_tun_del_addr6(unsigned index, const uint8_t address[16])
{
    return a20_bsd_tun_set_addr6(index, address, 128, 0);
}

int a20_bsd_tun_snapshot(nl_link_out_t *out, int max)
{
    int count = 0;
    for (int i = 0; i < BSD_TUN_MAX && count < max; ++i) {
        bsd_tun_slot_t *slot = &g_tuns[i];
        if (!a20_bsd_tun_is_index(slot->index)) continue;
        a20_bsd_tun_info_t info;
        if (tun_info(slot->index, &info) < 0) continue;
        nl_link_out_t *row = &out[count++];
        memset(row, 0, sizeof(*row));
        if (!net_tun_interface_name(slot->index, row->name, sizeof(row->name)))
            strncpy(row->name, info.name[0] ? info.name : slot->name,
                    sizeof(row->name) - 1);
        row->index = slot->index;
        row->mtu = info.mtu;
        row->flags = (info.flags & 1u) ? (NLRT_IF_FLAGS_UP |
                                          NLRT_IF_FLAGS_RUNNING) : 0;
        row->has_ip = info.has_ip;
        memcpy(row->ip, info.ip, 4);
        memcpy(row->mask, info.mask, 4);
        memcpy(row->gw, info.gw, 4);
    }
    return count;
}

int a20_bsd_tun_interface_ipv4(unsigned index)
{
    a20_bsd_tun_info_t info;
    if (tun_info(index, &info) < 0 || !info.has_ip) return 0;
    return (uint32_t)info.ip[0] << 24 | (uint32_t)info.ip[1] << 16 |
           (uint32_t)info.ip[2] << 8 | info.ip[3];
}

int a20_bsd_tun_owns_ipv4(uint32_t address)
{
    for (int i = 0; i < BSD_TUN_MAX; ++i) {
        if (!a20_bsd_tun_is_index(g_tuns[i].index)) continue;
        a20_bsd_tun_info_t info;
        if (tun_info(g_tuns[i].index, &info) < 0 || !info.has_ip)
            continue;
        uint32_t wire;
        memcpy(&wire, info.ip, sizeof(wire));
        if (wire == address)
            return 1;
    }
    return 0;
}
