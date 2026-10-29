/* Shared sockaddr representation and software-endpoint identifiers. Real
 * transport port reservation remains with the selected backend. */
#include "net/socket/socket_internal.h"
#include "net/stack.h"
#include "core/string.h"

/*
 * Next ephemeral port to hand out.
 *
 * This needed no synchronisation of its own while every caller held
 * g_net_lock.  Callers now hold the socket's own lock, and
 * two CPUs binding different sockets hold *different* buckets, so the counter
 * is a compare-exchange.  It stays a global counter: it is not per-socket
 * state, so it does not belong in net_socket_t.
 */
static volatile uint16_t g_next_ephemeral = 49152;

static uint16_t net_htons(uint16_t x)
{
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap16(x);
#else
    return x;
#endif
}

uint16_t net_ntohs(uint16_t x)
{
    return net_htons(x);
}

uint16_t net_alloc_ephemeral_port_locked(void)
{
    uint16_t p;
    for (;;) {
        uint16_t cur = __atomic_load_n(&g_next_ephemeral, __ATOMIC_RELAXED);
        /* Same floor and same 16-bit wrap as the plain ++ it replaced: nothing
         * below 49152 is ever handed out. */
        uint16_t next = (uint16_t)(cur + 1);
        if (next < 49152)
            next = 49152;
        if (__atomic_compare_exchange_n(&g_next_ephemeral, &cur, next, 1,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            p = cur;
            break;
        }
    }
    return net_htons(p);
}

/*
 * Implicit bind of an unbound inet socket to the loopback address of its own
 * family.  An AF_INET6 socket used to get a 16-byte sockaddr_in with
 * sin_family = AF_INET6 and the v4 loopback in sin_addr, which is not a valid
 * sockaddr_in6 -- the 16 bytes then read back as sin6_flowinfo/sin6_scope_id.
 * getpeername()/getsockname() and the netlink diag path both read that struct
 * back, so a v6 socket's implicit bind reported a nonsense scope id.
 */
void net_sockaddr_loopback(net_socket_t *s, uint16_t port)
{
    if (s->domain == AF_INET6) {
        net_sockaddr_in6_t in6;
        memset(&in6, 0, sizeof(in6));
        in6.sin6_family = AF_INET6;
        in6.sin6_port = port;
        in6.sin6_addr[15] = 1;      /* ::1 */
        memcpy(s->local, &in6, sizeof(in6));
        s->local_len = sizeof(in6);
        s->bound = 1;
        return;
    }
    net_sockaddr_in_t in;
    memset(&in, 0, sizeof(in));
    in.sin_family = (uint16_t)s->domain;
    in.sin_port = port;
    const uint8_t loopback[4] = {127, 0, 0, 1};
    memcpy(&in.sin_addr, loopback, sizeof(loopback));
    memcpy(s->local, &in, sizeof(in));
    s->local_len = sizeof(in);
    s->bound = 1;
}

int net_sockaddr_port(const void *addr, size_t len, uint16_t *port)
{
    if (!addr || len < sizeof(net_sockaddr_in_t) || !port)
        return -EINVAL;
    int family = *(const uint16_t *)addr;
    if (family != AF_INET && family != AF_INET6)
        return -EAFNOSUPPORT;
    *port = ((const net_sockaddr_in_t *)addr)->sin_port;
    return 0;
}

void net_sockaddr_set_port(void *addr, size_t len, uint16_t port)
{
    if (!addr || len < sizeof(net_sockaddr_in_t))
        return;
    net_sockaddr_in_t *in = (net_sockaddr_in_t *)addr;
    if (in->sin_family == AF_INET || in->sin_family == AF_INET6)
        in->sin_port = port;
}

int net_sockaddr_in_local(const net_sockaddr_in_t *address) {
    return address ? net_stack_owns_ipv4(address->sin_addr) : 0;
}
