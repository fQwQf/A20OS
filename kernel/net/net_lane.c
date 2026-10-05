#include "net/net_lane.h"

/*
 * Lane table.  Declared here rather than as a static array in every consumer so
 * that there is exactly one object, and CONFIG_NET_LANES stays the only place
 * the lane count appears.  At CONFIG_NET_LANES == 1 this is a single element and
 * every net_lane() call folds to &g_net_lanes[0].
 *
 * Fields are added as the later stages land (pbuf magazine and timeout wheel in
 * C, receive ring in D); the array shape does not change, so nothing that
 * indexes a lane has to be revisited.
 */
struct net_lane g_net_lanes[CONFIG_NET_LANES];

#if CONFIG_NET_LANES > 1
/*
 * Stage C's "which lane owns the work in progress" context.  Declared in
 * net_lane.h with the full rationale; the two facts that matter here are that
 * it is only meaningful under g_lwip_lock, and that lane 0 is the value a
 * section gets when it never establishes one of its own.
 */
unsigned a20_net_lane_cur;
#endif
