/*
 * netd frame-plane protocol: kernel virtio-net <-> userspace netd.
 *
 * Two SPSC frame rings live in one shared VMO allocated by the kernel and
 * mapped by netd.  The kernel owns the RX ring (writes frames from the NIC),
 * netd owns the TX ring (writes frames from lwIP's linkoutput).  Cursors are
 * monotonic; slots hold one ethernet frame each.  Doorbell is a polled
 * futex word per ring so either side can sleep without losing events.
 */
#ifndef _NETD_PROTO_H
#define _NETD_PROTO_H

#include <stdint.h>

#define NETD_RING_SLOTS   32          /* power of two */
#define NETD_MAX_FRAME    2048
#define NETD_VMO_PAGES    64          /* 256 KiB: two rings of 32x2KiB + hdr */

typedef struct netd_frame_ring {
    volatile uint32_t head;           /* producer cursor (monotonic) */
    volatile uint32_t tail;           /* consumer cursor (monotonic) */
    volatile uint32_t doorbell;       /* futex word: 1 = data available */
    uint32_t          slot_mask;
    /* slots: uint32_t len + payload, NETD_MAX_FRAME each, packed */
    uint8_t           data[NETD_RING_SLOTS * (4 + NETD_MAX_FRAME)];
} netd_frame_ring_t;

typedef struct netd_rings {
    netd_frame_ring_t rx;             /* kernel -> netd */
    netd_frame_ring_t tx;             /* netd -> kernel */
} netd_rings_t;

#define NETD_RING_SIZE  (sizeof(netd_frame_ring_t) + \
                         NETD_RING_SLOTS * (4 + NETD_MAX_FRAME))

#define NETD_RX_FUTEX_KEY 0x4e455444u /* "NETD" */
#define NETD_TX_FUTEX_KEY 0x4e455445u

#endif
