/* Compatibility shim for the old socket-only transport boundary.
 *
 * The contract now lives in net/backend.h: socket operations are the
 * `net_socket_ops_t` module of a `net_backend_t`, next to cycle/event/iface/
 * packet/observability modules.  New code must include net/backend.h.
 */
#ifndef A20_NET_TRANSPORT_H
#define A20_NET_TRANSPORT_H
#include "net/backend.h"
#endif
