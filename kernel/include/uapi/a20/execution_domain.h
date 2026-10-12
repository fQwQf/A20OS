#ifndef A20_EXECUTION_DOMAIN_H
#define A20_EXECUTION_DOMAIN_H
#include "execution.h"

/* One native kernel, regardless of board/chip count. Network peers belong
 * to other execution domains until real shared kernel memory is provided. */
#define A20_DOMAIN_VERSION 1u
#define A20_DOMAIN_BYTES 64u
#define A20_DOMAIN_CURRENT_KERNEL 1u
#define A20_DOMAIN_READY 1u
#define A20_DOMAIN_DEGRADED 2u
#define A20_DOMAIN_SHARED_KERNEL 1u

typedef struct a20_execution_domain_info {
    uint32_t size, version, scope, state;
    uint32_t flags, cpu_capacity, cpu_present, cpu_configured;
    uint32_t cpu_online, online_mask, memory_model, atomic_model;
    uint32_t reserved[4];
} a20_execution_domain_info_t;

static inline int
a20_execution_domain_valid(const a20_execution_domain_info_t *i) {
    if (!i || i->size != A20_DOMAIN_BYTES || i->version != A20_DOMAIN_VERSION ||
        i->scope != A20_DOMAIN_CURRENT_KERNEL || !i->cpu_capacity ||
        i->cpu_capacity > 32 || !i->cpu_present ||
        i->cpu_present > i->cpu_capacity || !i->cpu_configured ||
        i->cpu_configured > i->cpu_present || !i->cpu_online ||
        i->cpu_online > i->cpu_configured || !(i->online_mask & 1u) ||
        (i->cpu_configured < 32 && (i->online_mask >> i->cpu_configured)) ||
        (uint32_t)__builtin_popcount(i->online_mask) != i->cpu_online ||
        i->flags != (i->cpu_configured > 1 ? A20_DOMAIN_SHARED_KERNEL : 0) ||
        i->state != (i->cpu_online == i->cpu_configured
                         ? A20_DOMAIN_READY
                         : A20_DOMAIN_DEGRADED) ||
        (i->memory_model != A20_EXEC_MEMORY_FLAT &&
         i->memory_model != A20_EXEC_MEMORY_PRIVATE_VM) ||
        (i->atomic_model != A20_EXEC_ATOMIC_NATIVE &&
         i->atomic_model != A20_EXEC_ATOMIC_FABRIC_GATE))
        return 0;
    for (unsigned k = 0; k < 4; k++)
        if (i->reserved[k])
            return 0;
    return 1;
}
#endif
