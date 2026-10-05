#include "net/net_lane.h"

#if CONFIG_NET_LANES > 1
#include "core/string.h"
#endif

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

/*
 * Stage D's receive queue.
 *
 * THE CLAIM IS A FLAG, NOT A SPINLOCK, AND THAT IS THE POINT.  A spinlock in
 * core/lock.h is taken with interrupts disabled, which is right for the locks in
 * this tree because every one of them can be taken from an interrupt handler
 * that has just interrupted a holder.  This one is not: only net_lane_rx_claim()
 * sets it, and that is reached from kernel_progress_run_bottom_halves() --
 * process context, on every sched() and idle pass -- and never from the device
 * IRQ, because the producer needs no mutual exclusion at all (head and tail are
 * the whole of the synchronisation).  Leaving interrupts enabled while the claim
 * is held is therefore both safe and required: the protocol processing it
 * guards is exactly the work that must not run with interrupts off, which is
 * the reason it left the interrupt handler in the first place.
 *
 * It is non-blocking for the other reason the poll point needs it to be.  Every
 * CPU runs that poll point, so two of them reaching the same lane at the same
 * time is the normal case rather than an error, and the loser must move on to
 * the next lane instead of queueing behind a winner.
 *
 * A holder that gets preempted keeps the claim until it gives it up, which can
 * delay that lane.  That is a latency question and not a correctness one: the
 * work is bounded by the caller's budget, and no other CPU waits on the flag --
 * it tries, fails, and takes a different lane.
 */
static volatile int g_lane_rx_claim[CONFIG_NET_LANES];

/* Frames staged and not yet popped, across every lane.  A counter rather than a
 * scan of the lanes because the poll point asks the question on the scheduler
 * hot path, on every CPU, and one relaxed load has to be the whole answer. */
static volatile uint32_t g_lane_rx_queued_total;

/*
 * The size of the per-lane arrays, asserted against the profile's budget for
 * them rather than left to the reader of net_profile.h.  The header can only
 * approximate a struct's layout; sizeof() is what the budget is checked
 * against here, exactly as lwip_stack.c does for the netif state.
 */
_Static_assert(sizeof(g_net_lanes) <= NET_PROFILE_LANE_RXQ_BUDGET,
               "the per-lane receive queues exceed their profile budget; they "
               "are unconditional .bss in every multi-lane build, so the "
               "ceiling has to be a compile-time one");

bool net_lane_rx_put(unsigned lane, struct netif *nif,
                     const uint8_t *frame, unsigned len)
{
    struct net_lane *l = net_lane(lane);
    uint32_t head = __atomic_load_n(&l->rx_head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&l->rx_tail, __ATOMIC_ACQUIRE);

    if (head - tail >= NET_PROFILE_LANE_RXQ_SLOTS) {
        /* The frame is already off the device by now, so there is nothing to
         * push back on.  Counting it is the honest thing to do: a lane that
         * drops means its consumer is not running, and that is a bug to see
         * rather than a statistic to hide.  TCP retransmits, and a UDP loss is
         * the loss the application would have seen anyway. */
        __atomic_fetch_add(&l->rx_dropped, 1, __ATOMIC_RELAXED);
        return false;
    }
    if (len > NET_PROFILE_NETIF_FRAME_SIZE)
        len = NET_PROFILE_NETIF_FRAME_SIZE;

    struct net_lane_rx_slot *slot =
        &l->rxq[head % NET_PROFILE_LANE_RXQ_SLOTS];
    slot->nif = nif;
    slot->len = (uint16_t)len;
    memcpy(slot->frame, frame, len);

    __atomic_fetch_add(&l->rx_queued, 1, __ATOMIC_RELAXED);
    /* Publish last: a consumer that sees this head also sees the bytes. */
    __atomic_store_n(&l->rx_head, head + 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_lane_rx_queued_total, 1, __ATOMIC_RELAXED);
    return true;
}

unsigned net_lane_rx_queued_total(void)
{
    return __atomic_load_n(&g_lane_rx_queued_total, __ATOMIC_RELAXED);
}

int net_lane_rx_claim(unsigned lane)
{
    return __atomic_exchange_n(&g_lane_rx_claim[lane % CONFIG_NET_LANES], 1,
                               __ATOMIC_ACQUIRE) == 0;
}

void net_lane_rx_release(unsigned lane)
{
    __atomic_store_n(&g_lane_rx_claim[lane % CONFIG_NET_LANES], 0,
                     __ATOMIC_RELEASE);
}

int net_lane_rx_ready(unsigned lane)
{
    const struct net_lane *l = net_lane(lane);
    uint32_t head = __atomic_load_n(&l->rx_head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&l->rx_tail, __ATOMIC_ACQUIRE);
    return head != tail;
}

struct netif *net_lane_rx_pop(unsigned lane, const uint8_t **frame,
                              unsigned *len)
{
    struct net_lane *l = net_lane(lane);
    uint32_t tail = __atomic_load_n(&l->rx_tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&l->rx_head, __ATOMIC_ACQUIRE);

    if (head == tail)
        return NULL;

    struct net_lane_rx_slot *slot =
        &l->rxq[tail % NET_PROFILE_LANE_RXQ_SLOTS];

    struct netif *nif = slot->nif;
    *frame = slot->frame;
    *len = slot->len;

    /*
     * The slot is released as it is popped rather than after the caller has
     * finished with the bytes, so a producer that wraps around immediately can
     * reuse it.  That is safe only because the caller copies the frame into a
     * pbuf before asking for the next one, and it is the reason this is one
     * "pop and use" call rather than a peek followed by a release.
     */
    __atomic_store_n(&l->rx_tail, tail + 1, __ATOMIC_RELEASE);
    __atomic_fetch_sub(&g_lane_rx_queued_total, 1, __ATOMIC_RELAXED);
    return nif;
}

unsigned net_lane_rx_pending(unsigned lane)
{
    const struct net_lane *l = net_lane(lane);
    uint32_t head = __atomic_load_n(&l->rx_head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&l->rx_tail, __ATOMIC_ACQUIRE);
    return head - tail;
}

unsigned long long net_lane_rx_stat(unsigned lane, int which)
{
    const struct net_lane *l = net_lane(lane);
    switch (which) {
    case NET_LANE_RX_QUEUED:
        return (unsigned long long)
            __atomic_load_n(&l->rx_queued, __ATOMIC_RELAXED);
    case NET_LANE_RX_PROCESSED:
        return (unsigned long long)
            __atomic_load_n(&l->rx_processed, __ATOMIC_RELAXED);
    case NET_LANE_RX_DROPPED:
        return (unsigned long long)
            __atomic_load_n(&l->rx_dropped, __ATOMIC_RELAXED);
    default:
        return 0;
    }
}

void net_lane_rx_count_processed(unsigned lane)
{
    __atomic_fetch_add(&net_lane(lane)->rx_processed, 1, __ATOMIC_RELAXED);
}

#endif /* CONFIG_NET_LANES > 1 */
