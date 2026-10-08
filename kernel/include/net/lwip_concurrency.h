#ifndef _NET_LWIP_CONCURRENCY_H
#define _NET_LWIP_CONCURRENCY_H

#include "core/types.h"

/* Shared protocol state is separate from a connection's core lane.  Keys are
 * stable object identities, normally PCB pointers; NULL selects a domain-wide
 * cache.  Holders must not acquire core lanes or socket locks.  Short nested
 * calls on the same CPU are supported with interrupts disabled. */
enum a20_lwip_shared_domain {
    A20_LWIP_SHARED_RAW,
    A20_LWIP_SHARED_LISTENER,
    A20_LWIP_SHARED_UDP,
    A20_LWIP_SHARED_ARP,
    A20_LWIP_SHARED_ND6,
    A20_LWIP_SHARED_FRAG4,
    A20_LWIP_SHARED_FRAG6,
    A20_LWIP_SHARED_LOOPBACK,
    A20_LWIP_SHARED_TX,
    A20_LWIP_SHARED_CT,
    A20_LWIP_SHARED_DOMAIN_COUNT
};

uint64_t a20_lwip_shared_lock(unsigned domain, const void *key);
void a20_lwip_shared_unlock(unsigned domain, const void *key, uint64_t flags);
unsigned a20_lwip_core_lane(void);
void a20_lwip_parallel_probe_point(void);

#endif
