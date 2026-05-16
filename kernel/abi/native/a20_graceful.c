/*
 * A20 graceful degradation — error handling for resource exhaustion
 * and cascading failure scenarios.
 *
 * Implements the error handling patterns described in errors.md:
 * - Handle table OOM → shrink instead of panic
 * - Channel peer death → notify + clean up
 * - Cascading destroy depth limit → abort cascade at A20_CASCADE_DEPTH_MAX
 */

#include "abi/native/errno.h"
#include "abi/native/resource.h"
#include "abi/native/ipc_internal.h"
#include "core/sync.h"

/*
 * a20_graceful_ht_oom — handle table out-of-memory during grow.
 *
 * Instead of panicking, return -A20_ERR_NO_MEMORY and let the syscall
 * fail. The task can retry or free handles.
 *
 * @return: negative A20 error code.
 */
static inline int a20_graceful_ht_oom(void)
{
    return -A20_ERR_NO_MEMORY;
}

/*
 * a20_graceful_channel_peer_died — called when a channel peer closes.
 *
 * Wakes any thread blocked in channel_recv on the surviving endpoint,
 * returning -A20_ERR_CANCELED so it doesn't block forever.
 *
 * @ep: the surviving endpoint whose peer just closed.
 */
static inline void a20_graceful_channel_peer_died(struct a20_channel_ep *ep)
{
    if (!ep) return;
    ep->peer_closed = 1;
    wait_queue_wake_one(&ep->waiters);
}

/*
 * a20_cascade_check — check if cascading destroy should continue.
 *
 * @depth: current cascade depth (starts at 0).
 * @return: 0 if OK to continue, -1 if depth limit exceeded.
 */
static inline int a20_cascade_check(int depth)
{
    return depth >= A20_CASCADE_DEPTH_MAX ? -1 : 0;
}

/*
 * a20_graceful_eventq_full — handle event_queue ring buffer overflow.
 *
 * Per docs/native-abi/05-ipc.md §3.7: when the ring buffer is full, wake a consumer
 * instead of discarding the event. The consumer drains events,
 * making room for the new one.
 *
 * @eq: the event queue whose ring is full.
 */
static inline void a20_graceful_eventq_full(struct a20_eventq *eq)
{
    if (!eq) return;
    wait_queue_wake_one(&eq->waiters);
}
