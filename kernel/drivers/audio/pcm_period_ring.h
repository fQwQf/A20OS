/* SPDX-License-Identifier: Apache-2.0
 * DMA-owned period bookkeeping. The IRQ retires only published periods;
 * an empty next period requests immediate DMA stop and an explicit XRUN. */
#ifndef A20_PCM_RING_H
#define A20_PCM_RING_H
#include "core/types.h"
#define PCM_PERIODS 8U
#define PCM_PERIOD_BYTES 4096U
#define PCM_BYTES (PCM_PERIODS * PCM_PERIOD_BYTES)
typedef struct pcm_period_ring {
    unsigned producer, consumer, count;
    unsigned ready[PCM_PERIODS];
    int running, draining, error;
} pcm_period_ring_t;
static inline int pcm_period_publish(pcm_period_ring_t *r) {
    if (r->count == PCM_PERIODS || r->ready[r->producer])
        return -1;
    r->ready[r->producer] = 1;
    r->producer = (r->producer + 1) % PCM_PERIODS;
    r->count++;
    return 0;
}
/* IRQ package completion: 1=continue, 0=drained, -1=underrun. */
static inline int pcm_period_complete(pcm_period_ring_t *r) {
    if (!r->running || !r->count || !r->ready[r->consumer])
        return -1;
    r->ready[r->consumer] = 0;
    r->consumer = (r->consumer + 1) % PCM_PERIODS;
    r->count--;
    if (r->count)
        return 1;
    r->running = 0;
    return r->draining ? 0 : -1;
}
#endif
