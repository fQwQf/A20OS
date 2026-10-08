#include "net/lwip_concurrency.h"
#include "net/net_lane.h"
#include "core/lock.h"
#include "core/panic.h"

#if CONFIG_NET_LANES > 1
#define A20_LWIP_SHARED_SHARDS 16
struct a20_lwip_shared_guard {
    spinlock_t lock;
    unsigned owner_plus_one;
    unsigned depth;
};
static struct a20_lwip_shared_guard g_shared[A20_LWIP_SHARED_DOMAIN_COUNT]
                                            [A20_LWIP_SHARED_SHARDS];

static struct a20_lwip_shared_guard *shared_guard(unsigned domain,
                                                  const void *key) {
    if (domain >= A20_LWIP_SHARED_DOMAIN_COUNT)
        panic("lwip: invalid shared lock domain %u", domain);
    uintptr_t hash = (uintptr_t)key >> 4;
    hash ^= hash >> 9;
    return &g_shared[domain][hash % A20_LWIP_SHARED_SHARDS];
}
#endif

uint64_t a20_lwip_shared_lock(unsigned domain, const void *key) {
    uint64_t flags = arch_irqs_enabled() ? 1 : 0;
    arch_local_irq_disable();
#if CONFIG_NET_LANES > 1
    struct a20_lwip_shared_guard *guard = shared_guard(domain, key);
    unsigned owner = cpu_current_id() + 1;
    if (__atomic_load_n(&guard->owner_plus_one, __ATOMIC_ACQUIRE) != owner) {
        spin_lock(&guard->lock);
        __atomic_store_n(&guard->owner_plus_one, owner, __ATOMIC_RELEASE);
    }
    guard->depth++;
#else
    (void)domain;
    (void)key;
#endif
    return flags;
}

void a20_lwip_shared_unlock(unsigned domain, const void *key, uint64_t flags) {
#if CONFIG_NET_LANES > 1
    struct a20_lwip_shared_guard *guard = shared_guard(domain, key);
    if (__atomic_load_n(&guard->owner_plus_one, __ATOMIC_ACQUIRE) !=
            cpu_current_id() + 1 ||
        guard->depth == 0)
        panic("lwip: shared lock released by non-owner");
    if (--guard->depth == 0) {
        __atomic_store_n(&guard->owner_plus_one, 0, __ATOMIC_RELEASE);
        spin_unlock(&guard->lock);
    }
#else
    (void)domain;
    (void)key;
#endif
    if (flags)
        arch_local_irq_enable();
}

unsigned a20_lwip_core_lane(void) {
    return net_lane_ctx_get();
}
