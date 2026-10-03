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
