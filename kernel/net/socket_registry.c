#include "net/socket_internal.h"

#include "core/string.h"

net_socket_t *g_sockets[NET_MAX_SOCKETS];
volatile int g_net_bh_pending[NET_MAX_SOCKETS];
/* Number of slots whose pending flag is currently set.  Every 0->1 and 1->0
 * flag transition goes through exchange-based helpers in socket_inet.c, so
 * the count tracks the flags exactly; sched() early-outs on it instead of
 * scanning all NET_MAX_SOCKETS slots per context switch. */
volatile int g_net_bh_pending_count;

/* Free-slot bitmap for O(1) allocation.  Bit n == 0 -> slot n is free. */
static uint32_t g_sock_free[NET_MAX_SOCKETS / 32];

void net_bh_slot_mark(int idx)
{
    if (idx < 0 || idx >= NET_MAX_SOCKETS)
        return;
    if (__atomic_exchange_n(&g_net_bh_pending[idx], 1,
                            __ATOMIC_ACQ_REL) == 0)
        __atomic_fetch_add(&g_net_bh_pending_count, 1, __ATOMIC_ACQ_REL);
}

int net_bh_slot_clear(int idx)
{
    if (idx < 0 || idx >= NET_MAX_SOCKETS)
        return 0;
    if (__atomic_exchange_n(&g_net_bh_pending[idx], 0,
                            __ATOMIC_ACQ_REL) != 0) {
        __atomic_fetch_sub(&g_net_bh_pending_count, 1, __ATOMIC_ACQ_REL);
        return 1;
    }
    return 0;
}

void net_socket_registry_init(void) {
    memset(g_sockets, 0, sizeof(g_sockets));
    memset((void *)g_net_bh_pending, 0, sizeof(g_net_bh_pending));
    g_net_bh_pending_count = 0;
    /* All slots free */
    memset(g_sock_free, 0, sizeof(g_sock_free));
}

int net_register_socket_locked(net_socket_t *s) {
    for (int w = 0; w < (int)(NET_MAX_SOCKETS / 32); w++) {
        uint32_t free_bits = ~g_sock_free[w];
        if (!free_bits)
            continue;
        int bit, idx;
        for (bit = 0; bit < 32; bit++) {
            if (free_bits & (1U << bit))
                break;
        }
        idx = w * 32 + bit;
        if (idx >= NET_MAX_SOCKETS)
            break;
        g_sockets[idx] = s;
        g_sock_free[w] |= (1U << bit);
        s->in_registry = 1;
        s->reg_idx = idx;
        net_bh_slot_clear(idx);
        return 0;
    }
    return -ENFILE;
}

void net_unregister_socket_locked(net_socket_t *s) {
    if (!s || !s->in_registry)
        return;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (g_sockets[i] == s) {
            g_sockets[i] = NULL;
            net_bh_slot_clear(i);
            int w = i / 32;
            int bit = i % 32;
            g_sock_free[w] &= ~(1U << bit);
            s->in_registry = 0;
            s->reg_idx = -1;
            return;
        }
    }
    s->in_registry = 0;
    s->reg_idx = -1;
}

int net_socket_is_valid_locked(net_socket_t *s) {
    if (!s)
        return 0;
    return s->in_registry;
}
