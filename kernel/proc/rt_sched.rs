#![no_std]

//! Real-time runqueue policy. The C scheduler owns task/runqueue lifetime,
//! references, IRQ masking, and the runqueue lock; this module owns the
//! bucket/bitmap transitions, RR quantum accounting, and bandwidth arithmetic.

pub const SCHED_FIFO_VALUE: u32 = 1;
pub const SCHED_RR_VALUE: u32 = 2;

#[repr(C)]
pub struct RtNode {
    pub next: *mut RtNode,
    pub prev: *mut RtNode,
    /// Priority bucket while queued; -1 means detached.
    pub slot: i32,
    pub policy: u32,
}

#[cfg(target_pointer_width = "64")]
const _: () = {
    assert!(core::mem::offset_of!(RtNode, slot) == 16);
    assert!(core::mem::offset_of!(RtNode, policy) == 20);
    assert!(core::mem::size_of::<RtNode>() == 24);
    assert!(core::mem::align_of::<RtNode>() == 8);
};
#[cfg(target_pointer_width = "32")]
const _: () = {
    assert!(core::mem::offset_of!(RtNode, slot) == 8);
    assert!(core::mem::offset_of!(RtNode, policy) == 12);
    assert!(core::mem::size_of::<RtNode>() == 16);
    assert!(core::mem::align_of::<RtNode>() == 4);
};

#[repr(C)]
pub struct RtBucket {
    pub head: *mut RtNode,
    pub tail: *mut RtNode,
}

const _: () = {
    assert!(core::mem::offset_of!(RtBucket, tail) == core::mem::size_of::<usize>());
    assert!(core::mem::size_of::<RtBucket>() == 2 * core::mem::size_of::<usize>());
};

#[inline]
fn valid_state(buckets: *mut RtBucket, bitmap: *mut u32, levels: u32) -> bool {
    !buckets.is_null() && !bitmap.is_null() && levels != 0 && levels <= 100
}

#[inline]
unsafe fn bucket_at(buckets: *mut RtBucket, slot: u32) -> *mut RtBucket {
    // SAFETY: callers validate `slot < levels` and own the runqueue lock.
    unsafe { buckets.add(slot as usize) }
}

#[inline]
unsafe fn set_bit(bitmap: *mut u32, slot: u32) {
    unsafe { *bitmap.add((slot >> 5) as usize) |= 1u32 << (slot & 31) };
}

#[inline]
unsafe fn clear_bit(bitmap: *mut u32, slot: u32) {
    unsafe { *bitmap.add((slot >> 5) as usize) &= !(1u32 << (slot & 31)) };
}

/// Clamp and map the public RT priority into the configured bucket range.
#[no_mangle]
pub extern "C" fn a20_rt_priority_slot(priority: i32, levels: u32) -> u32 {
    if levels == 0 {
        return 0;
    }
    let p = if priority < 0 { 0 } else { priority as u32 };
    p.min(levels - 1)
}

#[no_mangle]
pub extern "C" fn a20_rt_policy_is_rt(policy: u32) -> u32 {
    (policy == SCHED_FIFO_VALUE || policy == SCHED_RR_VALUE) as u32
}

/// Strict RT ordering used when comparing a candidate with a running task.
/// RT always outranks non-RT; among RT tasks, larger numeric priority wins.
#[no_mangle]
pub extern "C" fn a20_rt_strictly_preempts(
    candidate_policy: u32,
    candidate_priority: i32,
    current_policy: u32,
    current_priority: i32,
) -> u32 {
    let candidate_rt = a20_rt_policy_is_rt(candidate_policy) != 0;
    let current_rt = a20_rt_policy_is_rt(current_policy) != 0;
    if candidate_rt != current_rt {
        candidate_rt as u32
    } else if candidate_rt {
        (candidate_priority > current_priority) as u32
    } else {
        0
    }
}

/// Enqueue one detached RT node at its priority FIFO tail. Returns 1 on
/// success and 0 for invalid inputs or an already-linked node.
///
/// # Safety
/// The caller must hold the lock for the runqueue described by `buckets` and
/// `bitmap`; all pointers must be valid, node storage must be unique to this
/// task, and `rr_remaining` must point to its task-owned slice counter.
#[no_mangle]
pub unsafe extern "C" fn a20_rt_enqueue(
    buckets: *mut RtBucket,
    bitmap: *mut u32,
    levels: u32,
    node: *mut RtNode,
    priority: i32,
    policy: u32,
    rr_remaining: *mut u64,
    rr_quantum: u64,
) -> u32 {
    if !valid_state(buckets, bitmap, levels) || node.is_null() || rr_remaining.is_null()
        || (policy != SCHED_FIFO_VALUE && policy != SCHED_RR_VALUE)
    {
        return 0;
    }
    let slot = a20_rt_priority_slot(priority, levels);
    let n = unsafe { &mut *node };
    if n.slot != -1 || !n.next.is_null() || !n.prev.is_null() {
        return 0;
    }
    let bucket = unsafe { &mut *bucket_at(buckets, slot) };
    if bucket.head.is_null() != bucket.tail.is_null()
        || (!bucket.head.is_null() && unsafe { !(*bucket.head).prev.is_null() })
        || (!bucket.tail.is_null() && unsafe { !(*bucket.tail).next.is_null() })
    {
        return 0;
    }
    n.slot = slot as i32;
    n.policy = policy;
    n.prev = bucket.tail;
    if bucket.tail.is_null() {
        bucket.head = node;
    } else {
        unsafe { (*bucket.tail).next = node };
    }
    bucket.tail = node;
    unsafe { set_bit(bitmap, slot) };
    if policy == SCHED_RR_VALUE && unsafe { *rr_remaining == 0 } {
        unsafe { *rr_remaining = rr_quantum.max(1) };
    }
    1
}

/// Remove a queued node using its stored slot. Double removal is rejected.
///
/// # Safety
/// `buckets` and `bitmap` must describe the same live runqueue, `node` must
/// belong to that runqueue or be detached, and all storage must remain valid
/// under the caller-held runqueue lock. The arrays and node storage must not
/// alias each other.
#[no_mangle]
pub unsafe extern "C" fn a20_rt_dequeue(
    buckets: *mut RtBucket,
    bitmap: *mut u32,
    levels: u32,
    node: *mut RtNode,
) -> u32 {
    if !valid_state(buckets, bitmap, levels) || node.is_null() {
        return 0;
    }
    let n = unsafe { &mut *node };
    if n.slot < 0 || n.slot as u32 >= levels {
        return 0;
    }
    let slot = n.slot as u32;
    let bucket = unsafe { &mut *bucket_at(buckets, slot) };
    // Validate every reciprocal edge and the bucket endpoints before the
    // first write, so a detected invariant violation cannot half-unlink node.
    if bucket.head.is_null() != bucket.tail.is_null()
        || (n.prev.is_null() && bucket.head != node)
        || (!n.prev.is_null() && unsafe {
            (*n.prev).slot != slot as i32 || (*n.prev).next != node
        })
        || (n.next.is_null() && bucket.tail != node)
        || (!n.next.is_null() && unsafe {
            (*n.next).slot != slot as i32 || (*n.next).prev != node
        })
    {
        return 0;
    }
    if !n.prev.is_null() {
        unsafe { (*n.prev).next = n.next };
    } else {
        bucket.head = n.next;
    }
    if !n.next.is_null() {
        unsafe { (*n.next).prev = n.prev };
    } else {
        bucket.tail = n.prev;
    }
    n.next = core::ptr::null_mut();
    n.prev = core::ptr::null_mut();
    n.slot = -1;
    n.policy = 0;
    if bucket.head.is_null() {
        unsafe { clear_bit(bitmap, slot) };
    }
    1
}

/// Return the runnable head in the highest priority bucket. FIFO and RR both
/// remain FIFO at this layer; an expired RR task is appended to the tail by
/// ordinary enqueue after the C scheduler requests a switch.
///
/// # Safety
/// Arrays must be valid for `levels` buckets and `ceil(levels/32)` bitmap
/// words, non-aliasing, and protected by the owning runqueue lock.
#[no_mangle]
pub unsafe extern "C" fn a20_rt_pick(
    buckets: *mut RtBucket,
    bitmap: *mut u32,
    levels: u32,
) -> *mut RtNode {
    if !valid_state(buckets, bitmap, levels) {
        return core::ptr::null_mut();
    }
    let words = (levels + 31) / 32;
    let mut wi = words;
    while wi > 0 {
        wi -= 1;
        let mut bits = unsafe { *bitmap.add(wi as usize) };
        while bits != 0 {
            let bit = 31 - bits.leading_zeros();
            let slot = wi * 32 + bit;
            if slot >= levels {
                bits &= !(1u32 << bit);
                unsafe { *bitmap.add(wi as usize) = bits };
                continue;
            }
            let bucket = unsafe { &mut *bucket_at(buckets, slot) };
            let head = bucket.head;
            if !head.is_null() {
                return head;
            }
            // Repair a stale set bit, matching the C reference's defensive
            // stale-bit repair behavior.
            bits &= !(1u32 << bit);
            unsafe { *bitmap.add(wi as usize) = bits };
        }
    }
    core::ptr::null_mut()
}

#[no_mangle]
pub unsafe extern "C" fn a20_rt_is_empty(bitmap: *const u32, levels: u32) -> u32 {
    if bitmap.is_null() || levels == 0 || levels > 100 {
        return 1;
    }
    let words = (levels + 31) / 32;
    for i in 0..words {
        if unsafe { *bitmap.add(i as usize) } != 0 {
            return 0;
        }
    }
    1
}

#[repr(C)]
pub struct RtBandwidthResult {
    pub throttled: u32,
    pub changed: u32,
}

/// Charge the current RT task and roll a fixed period when due. Arithmetic
/// intentionally uses wrapping timer deltas, then saturates usage at budget.
#[no_mangle]
pub unsafe extern "C" fn a20_rt_account(
    period_start: *mut u64,
    last_account: *mut u64,
    used: *mut u64,
    initialized: *mut u32,
    now: u64,
    ticks_per_sec: u64,
    current_is_rt: u32,
) -> RtBandwidthResult {
    if period_start.is_null() || last_account.is_null() || used.is_null() || initialized.is_null() {
        return RtBandwidthResult { throttled: 0, changed: 0 };
    }
    let period = ticks_per_sec.max(2);
    let reserve = (period / 20).max(1);
    let budget = period - reserve;
    let was = unsafe { *used >= budget };
    if unsafe { *initialized == 0 } {
        unsafe {
            *initialized = 1;
            *period_start = now;
            *last_account = now;
        }
    }
    let elapsed_period = now.wrapping_sub(unsafe { *period_start });
    if elapsed_period >= period {
        unsafe {
            *period_start = now.wrapping_sub(elapsed_period % period);
            *used = 0;
        }
    }
    let mut since = unsafe { *last_account };
    if since < unsafe { *period_start } {
        since = unsafe { *period_start };
    }
    if current_is_rt != 0 {
        let elapsed = now.wrapping_sub(since);
        let prior = unsafe { *used }.min(budget);
        unsafe { *used = prior.saturating_add(elapsed).min(budget) };
    }
    unsafe { *last_account = now };
    let throttled = unsafe { *used >= budget };
    RtBandwidthResult {
        throttled: throttled as u32,
        changed: (throttled != was) as u32,
    }
}

/// Account RR execution since its dispatch. The caller triggers reschedule
/// when this returns 1; the task is then re-enqueued at its priority FIFO tail.
///
/// # Safety
/// `remaining` and `last` must be valid, aligned, non-overlapping pointers to
/// the same task's RR fields. The caller must hold that task's park_lock.
#[no_mangle]
pub unsafe extern "C" fn a20_rt_rr_account(remaining: *mut u64, last: *mut u64, now: u64) -> u32 {
    if remaining.is_null() || last.is_null() { return 0; }
    let elapsed = now.wrapping_sub(unsafe { *last });
    unsafe {
        *remaining = (*remaining).saturating_sub(elapsed);
        *last = now;
        (*remaining == 0) as u32
    }
}

/// Start a dispatch, preserving a non-zero partial quantum across preemption
/// and blocking. `last` is reset so time spent off-CPU is not charged.
///
/// # Safety
/// `remaining` and `last` must be valid, aligned, non-overlapping pointers to
/// the same task's RR fields. The caller must hold that task's park_lock.
#[no_mangle]
pub unsafe extern "C" fn a20_rt_rr_dispatch(remaining: *mut u64, last: *mut u64, now: u64, quantum: u64) {
    if remaining.is_null() || last.is_null() { return; }
    unsafe {
        if *remaining == 0 { *remaining = quantum.max(1); }
        *last = now;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[repr(C)]
    struct TestNode {
        links: RtNode,
        remaining: u64,
    }

    impl TestNode {
        fn new() -> Self {
            Self { links: RtNode { next: core::ptr::null_mut(), prev: core::ptr::null_mut(), slot: -1, policy: 0 }, remaining: 0 }
        }
    }

    #[test]
    fn priority_fifo_and_preemption_order() {
        let mut buckets: [RtBucket; 8] = core::array::from_fn(|_| RtBucket { head: core::ptr::null_mut(), tail: core::ptr::null_mut() });
        let mut bits = [0u32; 1];
        let mut a = TestNode::new(); let mut b = TestNode::new(); let mut hi = TestNode::new();
        let mut hi_remaining = 0;
        unsafe {
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut a.links, 3, SCHED_FIFO_VALUE, &mut a.remaining, 5), 1);
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut b.links, 3, SCHED_FIFO_VALUE, &mut b.remaining, 5), 1);
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut hi.links, 7, SCHED_RR_VALUE, &mut hi_remaining, 5), 1);
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut a.links, 3, SCHED_FIFO_VALUE, &mut a.remaining, 5), 0);
            assert!(a20_rt_pick(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8) == &mut hi.links);
            assert_eq!(a20_rt_dequeue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut hi.links), 1);
            assert_eq!(a20_rt_dequeue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut hi.links), 0);
            assert!(a20_rt_pick(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8) == &mut a.links);
            assert_eq!(a20_rt_dequeue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut a.links), 1);
            assert!(a20_rt_pick(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8) == &mut b.links);
            assert_eq!(a20_rt_dequeue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut b.links), 1);
            assert_eq!(a20_rt_is_empty(bits.as_ptr(),8),1);
        }
    }

    #[test]
    fn rr_expiration_requeues_at_tail_without_starving_peer() {
        let mut buckets: [RtBucket; 8] = core::array::from_fn(|_| RtBucket { head: core::ptr::null_mut(), tail: core::ptr::null_mut() });
        let mut bits = [0u32; 1];
        let mut a = TestNode::new(); let mut b = TestNode::new();
        let mut a_slice = 0u64; let mut b_slice = 0u64;
        unsafe {
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut a.links, 4, SCHED_RR_VALUE, &mut a_slice, 5), 1);
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut b.links, 4, SCHED_RR_VALUE, &mut b_slice, 5), 1);
            assert!(a20_rt_pick(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8) == &mut a.links);
            assert_eq!(a20_rt_dequeue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut a.links), 1);
            let mut last = 0;
            a20_rt_rr_dispatch(&mut a_slice, &mut last, 10, 5);
            assert_eq!(a20_rt_rr_account(&mut a_slice, &mut last, 15), 1);
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut a.links, 4, SCHED_RR_VALUE, &mut a_slice, 5), 1);
            assert!(a20_rt_pick(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8) == &mut b.links);
            assert_eq!(a20_rt_dequeue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut b.links), 1);
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut b.links, 4, SCHED_RR_VALUE, &mut b_slice, 5), 1);
            assert!(a20_rt_pick(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8) == &mut a.links);
        }
    }

    #[test]
    fn legacy_pick_time_rotation_has_reproducible_two_peer_starvation() {
        // Reproduce the old C sequence exactly: rotate the RR head to tail,
        // return the new head, let that task run, then append it on switch-out.
        let mut queue = [0u8, 1u8];
        let mut selected = [0u8; 8];
        for out in &mut selected {
            let head = queue[0];
            queue[0] = queue[1];
            queue[1] = head;
            *out = queue[0];
            // The selected node is dequeued; the old current is re-enqueued
            // at the tail before the next pick.
            let task = queue[0];
            queue[0] = queue[1];
            queue[1] = task;
        }
        assert!(selected.iter().all(|&task| task == 1));
        assert_eq!(queue, [0, 1]);
    }

    #[test]
    fn fifo_head_is_not_rotated_by_rr_quantum() {
        let mut buckets: [RtBucket; 8] = core::array::from_fn(|_| RtBucket { head: core::ptr::null_mut(), tail: core::ptr::null_mut() });
        let mut bits = [0u32; 1];
        let mut fifo = TestNode::new();
        let mut rr_a = TestNode::new();
        let mut rr_b = TestNode::new();
        let mut fifo_rem = 0;
        let mut a_rem = 0;
        let mut b_rem = 0;
        unsafe {
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut fifo.links, 4, SCHED_FIFO_VALUE, &mut fifo_rem, 5), 1);
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut rr_a.links, 4, SCHED_RR_VALUE, &mut a_rem, 5), 1);
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut rr_b.links, 4, SCHED_RR_VALUE, &mut b_rem, 5), 1);
            assert!(a20_rt_pick(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8) == &mut fifo.links);
            assert_eq!(a20_rt_dequeue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut fifo.links), 1);
            assert!(a20_rt_pick(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8) == &mut rr_a.links);
            assert_eq!(a20_rt_dequeue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut rr_a.links), 1);
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut rr_a.links, 4, SCHED_RR_VALUE, &mut a_rem, 5), 1);
            assert!(a20_rt_pick(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8) == &mut rr_b.links);
        }
    }

    #[test]
    fn rr_partial_slice_survives_preemption_and_sleep_and_wraps() {
        let mut remaining = 0;
        let mut last = 0;
        unsafe {
            a20_rt_rr_dispatch(&mut remaining, &mut last, 100, 10);
            assert_eq!(remaining, 10);
            assert_eq!(a20_rt_rr_account(&mut remaining, &mut last, 104), 0);
            assert_eq!(remaining, 6);
            // Switching out and dispatching again does not charge sleep time.
            a20_rt_rr_dispatch(&mut remaining, &mut last, 1000, 10);
            assert_eq!(remaining, 6);
            assert_eq!(a20_rt_rr_account(&mut remaining, &mut last, 1002), 0);
            assert_eq!(remaining, 4);
            a20_rt_rr_dispatch(&mut remaining, &mut last, 2000, 10);
            assert_eq!(remaining, 4);
            assert_eq!(a20_rt_rr_account(&mut remaining, &mut last, 2004), 1);
            assert_eq!(remaining, 0);
            remaining = 10;
            last = u64::MAX - 2;
            assert_eq!(a20_rt_rr_account(&mut remaining, &mut last, 3), 0);
            assert_eq!(remaining, 4); // wrapping timer delta is 6
        }
    }

    #[test]
    fn corrupt_neighbor_is_rejected_without_partial_unlink() {
        let mut buckets: [RtBucket; 8] = core::array::from_fn(|_| RtBucket { head: core::ptr::null_mut(), tail: core::ptr::null_mut() });
        let mut bits = [0u32; 1];
        let mut a = TestNode::new(); let mut b = TestNode::new();
        let mut rem_a = 0; let mut rem_b = 0;
        unsafe {
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut a.links, 2, SCHED_FIFO_VALUE, &mut rem_a, 1), 1);
            assert_eq!(a20_rt_enqueue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut b.links, 2, SCHED_FIFO_VALUE, &mut rem_b, 1), 1);
            b.links.prev = core::ptr::null_mut();
            assert_eq!(a20_rt_dequeue(buckets.as_mut_ptr(), bits.as_mut_ptr(), 8, &mut a.links), 0);
            assert!(buckets[2].head == &mut a.links && buckets[2].tail == &mut b.links);
            assert!(b.links.prev.is_null());
        }
    }

    #[test]
    fn bandwidth_rollover_and_budget_clamp() {
        let mut start=0; let mut last=0; let mut used=0; let mut init=0u32;
        unsafe {
            let r=a20_rt_account(&mut start,&mut last,&mut used,&mut init,10,100,1);
            assert_eq!((r.throttled,r.changed),(0,0));
            let r=a20_rt_account(&mut start,&mut last,&mut used,&mut init,105,100,1);
            assert_eq!(used,95); assert_eq!((r.throttled,r.changed),(1,1));
            let r=a20_rt_account(&mut start,&mut last,&mut used,&mut init,210,100,0);
            assert_eq!(start,210); assert_eq!(used,0); assert_eq!((r.throttled,r.changed),(0,1));
        }
    }

    #[test]
    fn strict_rt_preemption_and_fifo_tie_semantics() {
        assert_eq!(a20_rt_strictly_preempts(SCHED_FIFO_VALUE, 80, 0, 0), 1);
        assert_eq!(a20_rt_strictly_preempts(0, 0, SCHED_RR_VALUE, 1), 0);
        assert_eq!(a20_rt_strictly_preempts(SCHED_RR_VALUE, 80, SCHED_FIFO_VALUE, 80), 0);
        assert_eq!(a20_rt_strictly_preempts(SCHED_RR_VALUE, 81, SCHED_FIFO_VALUE, 80), 1);
    }

    #[test]
    fn minimum_budget_and_wrapping_period_arithmetic_are_bounded() {
        let mut start=u64::MAX-2; let mut last=u64::MAX-2; let mut used=0; let mut init=1u32;
        unsafe {
            let r=a20_rt_account(&mut start,&mut last,&mut used,&mut init,3,2,1);
            assert_eq!(used,1); // preserved raw comparison can charge wrapped delta
            assert_eq!(r.throttled,1);
            let r=a20_rt_account(&mut start,&mut last,&mut used,&mut init,5,2,0);
            assert_eq!(used,0); // next complete period replenishes while non-RT runs
            assert_eq!(r.throttled,0);
            assert_eq!(r.changed,1);
        }
    }


}
