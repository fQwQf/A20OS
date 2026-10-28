/* Rust owns FIB values; the kernel owns IRQ-safe exclusion. Configuration
 * writers never enter a protocol stack while holding this lock. */
#include "net/route.h"
#include "core/lock.h"

static spinlock_t route_lock = SPINLOCK_INIT;

uint64_t a20_net_route_lock(void) {
    return spin_lock_irqsave(&route_lock);
}

void a20_net_route_unlock(uint64_t flags) {
    spin_unlock_irqrestore(&route_lock, flags);
}

_Static_assert(sizeof(net_route_v4_t) == 24, "route C/Rust ABI");
_Static_assert(sizeof(net_rule_v4_t) == 28, "rule C/Rust ABI");
