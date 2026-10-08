/* Run the actual kernel queue/context code with concurrent ingress/consumers.
 * In particular, a peeked frame must survive a producer wrapping its queue. */
#include "net/net_lane.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

__thread unsigned a20_host_cpu;
#define ROUNDS 30000u
static pthread_barrier_t context_barrier;

static void *consumer(void *arg) {
    unsigned lane = (unsigned)(uintptr_t)arg;
    a20_host_cpu = lane;
    net_lane_ctx_push(lane);
    pthread_barrier_wait(&context_barrier);
    assert(net_lane_ctx_get() == lane);
    unsigned prev = net_lane_ctx_push((lane + 1) % CONFIG_NET_LANES);
    assert(prev == lane);
    net_lane_ctx_pop(prev);
    for (unsigned seq = 0; seq < ROUNDS; seq++) {
        while (!net_lane_rx_claim(lane))
            sched_yield();
        const uint8_t *frame;
        unsigned len;
        struct netif *n;
        while (!(n = net_lane_rx_peek(lane, &frame, &len)))
            sched_yield();
        assert(n == (struct netif *)(uintptr_t)(lane + 1));
        assert(len == 128);
        uint32_t received;
        memcpy(&received, frame, sizeof(received));
        assert(received == seq);
        /* Give ingress a chance to lap a slow consumer. */
        if ((seq & 31) == 0)
            sched_yield();
        for (unsigned i = sizeof(received); i < len; i++)
            assert(frame[i] == (uint8_t)(seq ^ lane ^ i));
        net_lane_rx_count_processed(lane);
        net_lane_rx_consume(lane);
        net_lane_rx_release(lane);
    }
    return NULL;
}

int main(void) {
    /* Fill a queue, peek without releasing, then attempt a wrapping enqueue. */
    uint8_t bytes[128];
    memset(bytes, 0x5a, sizeof(bytes));
    for (unsigned i = 0; i < NET_PROFILE_LANE_RXQ_SLOTS; i++)
        assert(net_lane_rx_put(0, (struct netif *)1, bytes, sizeof(bytes)));
    assert(net_lane_rx_claim(0));
    const uint8_t *held;
    unsigned len;
    assert(net_lane_rx_peek(0, &held, &len));
    memset(bytes, 0xa5, sizeof(bytes));
    assert(!net_lane_rx_put(0, (struct netif *)1, bytes, sizeof(bytes)));
    for (unsigned i = 0; i < len; i++)
        assert(held[i] == 0x5a);
    for (unsigned i = 0; i < NET_PROFILE_LANE_RXQ_SLOTS; i++)
        net_lane_rx_consume(0);
    net_lane_rx_release(0);
    assert(net_lane_rx_queued_total() == 0);
    memset(g_net_lanes, 0, sizeof(g_net_lanes));

    pthread_t threads[CONFIG_NET_LANES];
    assert(pthread_barrier_init(&context_barrier, NULL, CONFIG_NET_LANES) == 0);
    for (unsigned lane = 0; lane < CONFIG_NET_LANES; lane++)
        assert(pthread_create(&threads[lane], NULL, consumer,
                              (void *)(uintptr_t)lane) == 0);
    for (unsigned seq = 0; seq < ROUNDS; seq++) {
        for (unsigned lane = 0; lane < CONFIG_NET_LANES; lane++) {
            memcpy(bytes, &seq, sizeof(seq));
            for (unsigned i = sizeof(seq); i < sizeof(bytes); i++)
                bytes[i] = (uint8_t)(seq ^ lane ^ i);
            while (!net_lane_rx_put(lane, (struct netif *)(uintptr_t)(lane + 1),
                                    bytes, sizeof(bytes)))
                sched_yield();
        }
    }
    for (unsigned lane = 0; lane < CONFIG_NET_LANES; lane++) {
        assert(pthread_join(threads[lane], NULL) == 0);
        assert(net_lane_rx_pending(lane) == 0);
        assert(net_lane_rx_stat(lane, NET_LANE_RX_PROCESSED) == ROUNDS);
    }
    assert(net_lane_rx_queued_total() == 0);
    puts("net_lane_concurrency: PASS (120000 frames, held-slot lifetime, "
         "CPU-local context)");
    return 0;
}
