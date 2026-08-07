#ifndef _KERNEL_NET_NETD_PROTO_H
#define _KERNEL_NET_NETD_PROTO_H

#include "core/types.h"

#define NETD_RING_SLOTS   32
#define NETD_MAX_FRAME    2048
#define NETD_VMO_PAGES    64

typedef struct netd_frame_ring {
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t doorbell;
    uint32_t          slot_mask;
    uint8_t           data[NETD_RING_SLOTS * (4 + NETD_MAX_FRAME)];
} netd_frame_ring_t;

typedef struct netd_rings {
    netd_frame_ring_t rx;
    netd_frame_ring_t tx;
} netd_rings_t;

#define NETD_RING_SIZE  (sizeof(netd_frame_ring_t) + \
                         NETD_RING_SLOTS * (4 + NETD_MAX_FRAME))

void netd_ring_init(void);
int  netd_enabled(void);
int  netd_rx_frame(const void *data, uint32_t len);
uint32_t netd_tx_frame(void *out, uint32_t max);

#endif
