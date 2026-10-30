/* Small option-policy primitives consumed by the common Rust option machine.
 * Unsupported multicast/congestion changes fail explicitly; no lwIP PCB exists. */
#include "net/socket/socket_internal.h"
#include "core/errno.h"
#include "core/string.h"

int tcp_cong_alg_parse(const char *name)
{
    if (!name) return -1;
    if (!strcmp(name, "reno") || !strcmp(name, "newreno")) return 0;
    if (!strcmp(name, "cubic")) return 2;
    return -1;
}

const char *tcp_cong_alg_name(uint8_t algorithm)
{
    return algorithm == 2 ? "cubic" : "reno";
}

int a20_net_option_tcp_live(void *socket, int which)
{
    net_socket_t *s = socket;
    if (!s || s->type != SOCK_STREAM || !s->transport_private) return -1;
    /* Live values are only reported after options_apply() pushed them to the
     * BSD socket.  which: 0 congestion, 1 keep_idle, 2 keep_intvl, 3 keep_cnt. */
    switch (which) {
    case 0: return s->tcp_congestion == 2 ? 2 : 0;
    case 1: return s->keep_idle;
    case 2: return s->keep_intvl;
    case 3: return s->keep_cnt;
    default: return -1;
    }
}
