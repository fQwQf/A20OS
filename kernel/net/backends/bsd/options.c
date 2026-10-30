/* Small option-policy primitives consumed by the common Rust option machine.
 * Unsupported multicast/congestion changes fail explicitly; no lwIP PCB exists. */
#include "net/socket/socket_internal.h"
#include "core/errno.h"
#include "core/string.h"

int tcp_cong_alg_parse(const char *name)
{
    return name && !strcmp(name, "reno") ? 0 : -1;
}

const char *tcp_cong_alg_name(uint8_t algorithm)
{
    (void)algorithm;
    return "reno";
}

int a20_net_option_group_membership(void *socket, const void *request, int join)
{
    (void)socket; (void)request; (void)join;
    return -EOPNOTSUPP;
}

int a20_net_option_tcp_live(void *socket, int which)
{
    net_socket_t *s = socket;
    if (!s || s->type != SOCK_STREAM || !s->transport_private) return -1;
    /* FreeBSD's default algorithm is Reno in this initial vertical slice.
     * Keepalive timer values are not reported as live until their setsockopt
     * application is implemented; returning the frontend's requested integer
     * would falsely claim the protocol had accepted it. */
    return which == 0 ? 0 : -1;
}
