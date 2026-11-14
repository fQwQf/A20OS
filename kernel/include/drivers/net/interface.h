/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_NET_DRIVER_INTERFACE_H
#define A20_NET_DRIVER_INTERFACE_H
#include "drivers/core/protocol.h"
typedef struct net_dev_stats {
    uint64_t rx_packets, rx_drops, tx_packets, tx_drops;
} net_dev_stats_t;
/* DRIVER_PROTOCOL_NET_STATS, version 1. Nonblocking, IRQ-safe snapshot of
 * negotiated driver's counters. 0=success, negative errno on failure. */
struct driver_net_stats_ops {
    int (*read)(struct device *, struct net_dev_stats *out);
};
#endif
