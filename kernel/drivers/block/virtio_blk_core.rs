//! Shared VirtIO block request and queue state for the kernel and UBD driver.
//!
//! Rust owns reservation, publication-state, used-entry validation,
//! completion retirement, cancellation, and reset-state transitions. The C
//! placement adapters retain ring memory access, DMA synchronization, MMIO,
//! IRQ/poll dispatch, and waiter wakeups. Hardware reset must quiesce DMA
//! before the adapter calls queue_reset; epoch is diagnostic state and is not
//! a stale-device-completion guard.
#![no_std]

#[cfg(test)]
extern crate std;


const EINVAL: i32 = -22;
const ENOSPC: i32 = -28;
const DESC_F_NEXT: u16 = 1;
const DESC_F_WRITE: u16 = 2;
const EIO: i32 = -5;
const EBUSY: i32 = -16;
const EPROTO: i32 = -71;
const SLOT_FREE: u8 = 0;
const SLOT_RESERVED: u8 = 1;
const SLOT_PREPARED: u8 = 2;
const SLOT_SUBMITTED: u8 = 3;
const SLOT_COMPLETED: u8 = 4;
const MAX_QUEUE_SIZE: u16 = 64;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct A20VirtioBlkDesc {
    pub addr: u64,
    pub len: u32,
    pub flags: u16,
    pub next: u16,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct A20VirtioBlkSegment {
    pub addr: u64,
    pub len: u32,
}

/// Queue and request lifecycle state shared by the kernel and UBD adapters.
/// The caller serializes every operation with its existing queue lock. Ring
/// memory, DMA synchronization, and MMIO remain in the C placement adapter.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20VirtioBlkRequestState {
    head: u16,
    descriptor_count: u16,
    expected_len: u32,
    write: u8,
    state: u8,
    reserved: [u8; 2],
}

impl Default for A20VirtioBlkRequestState {
    fn default() -> Self {
        Self { head: 0, descriptor_count: 0, expected_len: 0,
               write: 0, state: SLOT_FREE, reserved: [0; 2] }
    }
}

#[repr(C)]
pub struct A20VirtioBlkQueueState {
    queue_size: u16,
    slot_count: u16,
    avail_idx: u16,
    last_used: u16,
    descriptor_cursor: u16,
    active_count: u16,
    epoch: u32,
    descriptor_bitmap: u64,
    requests: [A20VirtioBlkRequestState; 64],
}

impl Default for A20VirtioBlkQueueState {
    fn default() -> Self {
        Self { queue_size: 0, slot_count: 0, avail_idx: 0, last_used: 0,
               descriptor_cursor: 0, active_count: 0, epoch: 0,
               descriptor_bitmap: 0,
               requests: [A20VirtioBlkRequestState::default(); 64] }
    }
}

const _: [(); 12] = [(); core::mem::size_of::<A20VirtioBlkRequestState>()];
const _: [(); 792] = [(); core::mem::size_of::<A20VirtioBlkQueueState>()];

#[no_mangle]
pub unsafe extern "C" fn a20_virtio_blk_queue_init(
    queue: *mut A20VirtioBlkQueueState, queue_size: u16, slot_count: u16,
) -> i32 {
    if queue.is_null() || queue_size < 4 || queue_size > MAX_QUEUE_SIZE
        || (queue as usize) % core::mem::align_of::<A20VirtioBlkQueueState>() != 0
        || !queue_size.is_power_of_two() || slot_count == 0 || slot_count > queue_size
    { return EINVAL; }
    // Callers may pass uninitialized storage or old state after a hardware
    // reset. Write the complete POD state without first creating a reference
    // to potentially uninitialized fields.
    unsafe {
        core::ptr::write(
            queue,
            A20VirtioBlkQueueState { queue_size, slot_count, epoch: 1, ..Default::default() },
        );
    }
    0
}

/// Reserve a free request slot and a non-overlapping circular descriptor run.
/// `expected_len` is the minimum used length accepted at completion.
#[no_mangle]
pub unsafe extern "C" fn a20_virtio_blk_queue_reserve(
    queue: *mut A20VirtioBlkQueueState, descriptor_count: u16,
    expected_len: u32, write: u32, out_slot: *mut u16, out_head: *mut u16,
) -> i32 {
    if queue.is_null() || out_slot.is_null() || out_head.is_null() || write > 1 {
        return EINVAL;
    }
    let q = unsafe { &mut *queue };
    if q.queue_size < 4 || descriptor_count < 2 || descriptor_count > q.queue_size
        || expected_len == 0
    { return EINVAL; }
    let mut free_slot = None;
    for i in 0..q.slot_count as usize {
        if unsafe { q.requests.get_unchecked(i).state } == SLOT_FREE {
            free_slot = Some(i as u16);
            break;
        }
    }
    let slot = match free_slot { Some(i) => i, None => return ENOSPC };
    let mask = q.queue_size - 1;
    let mut found = None;
    for offset in 0..q.queue_size {
        let head = q.descriptor_cursor.wrapping_add(offset) & mask;
        let mut bits = 0u64;
        let mut free = true;
        for i in 0..descriptor_count {
            let bit = 1u64 << (head.wrapping_add(i) & mask);
            if q.descriptor_bitmap & bit != 0 { free = false; break; }
            bits |= bit;
        }
        if free { found = Some((head, bits)); break; }
    }
    let (head, bits) = match found { Some(v) => v, None => return ENOSPC };
    q.descriptor_bitmap |= bits;
    q.descriptor_cursor = head.wrapping_add(descriptor_count) & mask;
    q.active_count = q.active_count.saturating_add(1);
    unsafe {
        *q.requests.get_unchecked_mut(slot as usize) = A20VirtioBlkRequestState {
            head, descriptor_count, expected_len, write: write as u8,
            state: SLOT_RESERVED, reserved: [0; 2],
        };
    }
    unsafe { out_slot.write(slot); out_head.write(head); }
    0
}

/// Mark the request ready for ring publication and return the avail slot.
#[no_mangle]
pub unsafe extern "C" fn a20_virtio_blk_queue_prepare_publish(
    queue: *mut A20VirtioBlkQueueState, slot: u16, out_ring_slot: *mut u16,
) -> i32 {
    if queue.is_null() || out_ring_slot.is_null() { return EINVAL; }
    let q = unsafe { &mut *queue };
    if slot >= q.slot_count { return EINVAL; }
    if unsafe { q.requests.get_unchecked(slot as usize).state } != SLOT_RESERVED {
        return EPROTO;
    }
    unsafe { q.requests.get_unchecked_mut(slot as usize).state = SLOT_PREPARED; }
    unsafe { out_ring_slot.write(q.avail_idx & (q.queue_size - 1)); }
    0
}

/// Commit the avail index after the adapter has stored the ring entry and
/// performed the required DMA sync/release fence. The returned value is the
/// new device-visible index for the adapter to write before notifying MMIO.
#[no_mangle]
pub unsafe extern "C" fn a20_virtio_blk_queue_commit_publish(
    queue: *mut A20VirtioBlkQueueState, slot: u16, out_avail_idx: *mut u16,
) -> i32 {
    if queue.is_null() || out_avail_idx.is_null() { return EINVAL; }
    let q = unsafe { &mut *queue };
    if slot >= q.slot_count { return EINVAL; }
    if unsafe { q.requests.get_unchecked(slot as usize).state } != SLOT_PREPARED {
        return EPROTO;
    }
    unsafe { q.requests.get_unchecked_mut(slot as usize).state = SLOT_SUBMITTED; }
    q.avail_idx = q.avail_idx.wrapping_add(1);
    unsafe { out_avail_idx.write(q.avail_idx); }
    0
}

/// Return the used-ring slot to read. A device index jump larger than the
/// queue is malformed and must force queue recovery rather than overrun state.
#[no_mangle]
pub unsafe extern "C" fn a20_virtio_blk_queue_used_ring_slot(
    queue: *mut A20VirtioBlkQueueState, device_idx: u16, out_ring_slot: *mut u16,
) -> i32 {
    if queue.is_null() || out_ring_slot.is_null() { return EINVAL; }
    let q = unsafe { &mut *queue };
    let pending = device_idx.wrapping_sub(q.last_used);
    if pending == 0 { return 0; }
    if pending > q.queue_size { return EPROTO; }
    unsafe { out_ring_slot.write(q.last_used & (q.queue_size - 1)); }
    1
}

/// Resolve a device-supplied descriptor head to an in-flight request slot.
/// Adapters must call this before indexing per-head status or request memory.
#[no_mangle]
pub unsafe extern "C" fn a20_virtio_blk_queue_validate_head(
    queue: *mut A20VirtioBlkQueueState, head: u16, out_slot: *mut u16,
) -> i32 {
    if queue.is_null() || out_slot.is_null() { return EINVAL; }
    let q = unsafe { &mut *queue };
    if head >= q.queue_size { return EPROTO; }
    for slot in 0..q.slot_count as usize {
        let request = unsafe { q.requests.get_unchecked(slot) };
        if request.state == SLOT_SUBMITTED && request.head == head {
            unsafe { out_slot.write(slot as u16); }
            return 0;
        }
    }
    EPROTO
}

/// Validate and retire one used element. A nonzero device status or a short
/// used length completes the request as I/O error; an unknown/duplicate head
/// is a protocol error and leaves cursors unchanged for queue reset.
#[no_mangle]
pub unsafe extern "C" fn a20_virtio_blk_queue_complete_used(
    queue: *mut A20VirtioBlkQueueState, device_idx: u16, head: u16,
    used_len: u32, device_status: u32, out_slot: *mut u16, out_result: *mut i32,
) -> i32 {
    if queue.is_null() || out_slot.is_null() || out_result.is_null() { return EINVAL; }
    let q = unsafe { &mut *queue };
    let pending = device_idx.wrapping_sub(q.last_used);
    if pending == 0 || pending > q.queue_size { return EPROTO; }
    let mut found_slot = None;
    for i in 0..q.slot_count as usize {
        let request = unsafe { q.requests.get_unchecked(i) };
        if request.state == SLOT_SUBMITTED && request.head == head {
            found_slot = Some(i);
            break;
        }
    }
    let slot = match found_slot { Some(i) => i, None => return EPROTO };
    let request_copy = unsafe { *q.requests.get_unchecked(slot) };
    let result = if device_status == 0 && used_len >= request_copy.expected_len { 0 } else { EIO };
    for i in 0..request_copy.descriptor_count {
        q.descriptor_bitmap &= !(1u64 << (request_copy.head.wrapping_add(i) & (q.queue_size - 1)));
    }
    unsafe { q.requests.get_unchecked_mut(slot).state = SLOT_COMPLETED; }
    q.last_used = q.last_used.wrapping_add(1);
    q.active_count = q.active_count.saturating_sub(1);
    unsafe { out_slot.write(slot as u16); out_result.write(result); }
    1
}

/// Fail every active transaction during queue reset, returning request-slot
/// indices for the C adapter to mark failed and wake. No callbacks run here.
#[no_mangle]
pub unsafe extern "C" fn a20_virtio_blk_queue_fail_all(
    queue: *mut A20VirtioBlkQueueState, out_slots: *mut u16, capacity: u16,
) -> i32 {
    if queue.is_null() || out_slots.is_null() { return EINVAL; }
    let q = unsafe { &mut *queue };
    let mut count = 0usize;
    for slot in 0..q.slot_count as usize {
        let state = unsafe { q.requests.get_unchecked(slot).state };
        if matches!(state, SLOT_RESERVED | SLOT_PREPARED | SLOT_SUBMITTED) { count += 1; }
    }
    if count > capacity as usize { return ENOSPC; }
    let mut out = 0usize;
    for slot in 0..q.slot_count as usize {
        let state = unsafe { q.requests.get_unchecked(slot).state };
        if matches!(state, SLOT_RESERVED | SLOT_PREPARED | SLOT_SUBMITTED) {
            let request = unsafe { *q.requests.get_unchecked(slot) };
            for i in 0..request.descriptor_count {
                q.descriptor_bitmap &= !(1u64 << (request.head.wrapping_add(i) & (q.queue_size - 1)));
            }
            unsafe { q.requests.get_unchecked_mut(slot).state = SLOT_COMPLETED; }
            unsafe { out_slots.add(out).write(slot as u16); }
            out += 1;
        }
    }
    q.active_count = 0;
    out as i32
}

/// Reinitialize indices after the hardware reset has completed. Completed
/// slots remain owned by their waiters until `release` is called.
#[no_mangle]
pub unsafe extern "C" fn a20_virtio_blk_queue_reset(queue: *mut A20VirtioBlkQueueState) -> i32 {
    if queue.is_null() { return EINVAL; }
    let q = unsafe { &mut *queue };
    if q.active_count != 0 || q.descriptor_bitmap != 0 { return EBUSY; }
    q.avail_idx = 0;
    q.last_used = 0;
    q.descriptor_cursor = 0;
    q.epoch = q.epoch.wrapping_add(1);
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_virtio_blk_queue_release(
    queue: *mut A20VirtioBlkQueueState, slot: u16,
) -> i32 {
    if queue.is_null() { return EINVAL; }
    let q = unsafe { &mut *queue };
    if slot >= q.slot_count || unsafe { q.requests.get_unchecked(slot as usize).state } != SLOT_COMPLETED {
        return EPROTO;
    }
    unsafe { *q.requests.get_unchecked_mut(slot as usize) = A20VirtioBlkRequestState::default(); }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_virtio_blk_queue_cancel(
    queue: *mut A20VirtioBlkQueueState, slot: u16,
) -> i32 {
    if queue.is_null() { return EINVAL; }
    let q = unsafe { &mut *queue };
    if slot >= q.slot_count { return EINVAL; }
    let request = unsafe { *q.requests.get_unchecked(slot as usize) };
    if !matches!(request.state, SLOT_RESERVED | SLOT_PREPARED) { return EPROTO; }
    for i in 0..request.descriptor_count {
        q.descriptor_bitmap &= !(1u64 << (request.head.wrapping_add(i) & (q.queue_size - 1)));
    }
    unsafe { *q.requests.get_unchecked_mut(slot as usize) = A20VirtioBlkRequestState::default(); }
    q.active_count = q.active_count.saturating_sub(1);
    0
}

/// Build one block request chain in a power-of-two descriptor table.
///
/// `device_writes` marks data segments writable by the device (block reads).
/// Header and status descriptors are always provided by the caller's DMA
/// storage. The function validates all geometry before touching `descs`.
#[no_mangle]
pub unsafe extern "C" fn a20_virtio_blk_build_chain(
    descs: *mut A20VirtioBlkDesc,
    queue_size: u16,
    head: u16,
    header_addr: u64,
    header_len: u32,
    segments: *const A20VirtioBlkSegment,
    segment_count: u16,
    status_addr: u64,
    device_writes: u32,
) -> i32 {
    if descs.is_null() || queue_size < 2 || !queue_size.is_power_of_two()
        || head >= queue_size || header_len == 0 || status_addr == 0
        || device_writes > 1
    {
        return EINVAL;
    }
    let needed = match segment_count.checked_add(2) {
        Some(n) => n,
        None => return EINVAL,
    };
    if needed > queue_size {
        return ENOSPC;
    }
    if segment_count != 0 && segments.is_null() {
        return EINVAL;
    }

    // Validate every segment first, so malformed input leaves the table intact.
    for i in 0..segment_count as usize {
        let segment = unsafe { segments.add(i).read() };
        if segment.len == 0 {
            return EINVAL;
        }
    }

    let mask = queue_size - 1;
    let mut slot = head;
    unsafe {
        descs.add(slot as usize).write(A20VirtioBlkDesc {
            addr: header_addr,
            len: header_len,
            flags: DESC_F_NEXT,
            next: slot.wrapping_add(1) & mask,
        });
    }
    for i in 0..segment_count as usize {
        let next = slot.wrapping_add(1) & mask;
        let segment = unsafe { segments.add(i).read() };
        let flags = DESC_F_NEXT | if device_writes != 0 { DESC_F_WRITE } else { 0 };
        unsafe {
            descs.add(next as usize).write(A20VirtioBlkDesc {
                addr: segment.addr,
                len: segment.len,
                flags,
                next: next.wrapping_add(1) & mask,
            });
        }
        slot = next;
    }
    let status_slot = slot.wrapping_add(1) & mask;
    unsafe {
        descs.add(status_slot as usize).write(A20VirtioBlkDesc {
            addr: status_addr,
            len: 1,
            flags: DESC_F_WRITE,
            next: 0,
        });
    }
    needed as i32
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec;
    use std::ptr;

    #[test]
    fn builds_wrapping_read_and_write_chains() {
        let mut descs = vec![A20VirtioBlkDesc::default(); 8];
        let segs = [A20VirtioBlkSegment { addr: 0x2000, len: 512 },
                    A20VirtioBlkSegment { addr: 0x3000, len: 512 }];
        let n = unsafe {
            a20_virtio_blk_build_chain(descs.as_mut_ptr(), 8, 7, 0x1000, 16,
                                       segs.as_ptr(), 2, 0x4000, 1)
        };
        assert_eq!(n, 4);
        assert_eq!(descs[7], A20VirtioBlkDesc { addr: 0x1000, len: 16, flags: 1, next: 0 });
        assert_eq!(descs[0], A20VirtioBlkDesc { addr: 0x2000, len: 512, flags: 3, next: 1 });
        assert_eq!(descs[1], A20VirtioBlkDesc { addr: 0x3000, len: 512, flags: 3, next: 2 });
        assert_eq!(descs[2], A20VirtioBlkDesc { addr: 0x4000, len: 1, flags: 2, next: 0 });
        assert_eq!(unsafe { a20_virtio_blk_build_chain(descs.as_mut_ptr(), 8, 3, 0x1000, 16,
            segs.as_ptr(), 2, 0x4000, 0) }, 4);
        assert_eq!(descs[4].flags, 1);
        assert_eq!(descs[5].flags, 1);
    }

    #[test]
    fn flush_chain_has_no_data_segment() {
        let mut descs = vec![A20VirtioBlkDesc::default(); 4];
        assert_eq!(unsafe { a20_virtio_blk_build_chain(descs.as_mut_ptr(), 4, 3, 0x1000, 16,
            ptr::null(), 0, 0x2000, 0) }, 2);
        assert_eq!(descs[3].next, 0);
        assert_eq!(descs[0].flags, DESC_F_WRITE);
    }

    #[test]
    fn rejects_bad_geometry_without_writing() {
        let mut descs = vec![A20VirtioBlkDesc { addr: 9, len: 9, flags: 9, next: 9 }; 4];
        let too_many = [A20VirtioBlkSegment { addr: 1, len: 1 }; 3];
        assert_eq!(unsafe { a20_virtio_blk_build_chain(descs.as_mut_ptr(), 4, 0, 1, 16,
            too_many.as_ptr(), 3, 2, 0) }, ENOSPC);
        assert!(descs.iter().all(|d| d.addr == 9));
        let invalid = [A20VirtioBlkSegment { addr: 1, len: 0 }];
        assert_eq!(unsafe { a20_virtio_blk_build_chain(descs.as_mut_ptr(), 4, 0, 1, 16,
            invalid.as_ptr(), 1, 2, 1) }, EINVAL);
        assert!(descs.iter().all(|d| d.addr == 9));
    }

    #[test]
    fn reservation_publication_completion_release_and_wrap() {
        let mut q = A20VirtioBlkQueueState::default();
        assert_eq!(unsafe { a20_virtio_blk_queue_init(&mut q, 8, 2) }, 0);
        let (mut slot, mut head) = (u16::MAX, u16::MAX);
        assert_eq!(unsafe { a20_virtio_blk_queue_reserve(&mut q, 3, 513, 0,
                                                        &mut slot, &mut head) }, 0);
        assert_eq!((slot, head), (0, 0));
        let mut ring_slot = u16::MAX;
        assert_eq!(unsafe { a20_virtio_blk_queue_prepare_publish(&mut q, slot,
                                                                 &mut ring_slot) }, 0);
        assert_eq!(ring_slot, 0);
        let mut avail_idx = u16::MAX;
        assert_eq!(unsafe { a20_virtio_blk_queue_commit_publish(&mut q, slot,
                                                                &mut avail_idx) }, 0);
        assert_eq!(avail_idx, 1);
        let mut used_slot = u16::MAX;
        assert_eq!(unsafe { a20_virtio_blk_queue_used_ring_slot(&mut q, 1,
                                                               &mut used_slot) }, 1);
        assert_eq!(used_slot, 0);
        let (mut completed_slot, mut result) = (u16::MAX, 99);
        assert_eq!(unsafe { a20_virtio_blk_queue_complete_used(&mut q, 1, head, 513, 0,
                  &mut completed_slot, &mut result) }, 1);
        assert_eq!((completed_slot, result), (slot, 0));
        assert_eq!(unsafe { a20_virtio_blk_queue_release(&mut q, slot) }, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_reset(&mut q) }, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_reserve(&mut q, 3, 1, 1,
                                                        &mut slot, &mut head) }, 0);
        assert_eq!(head, 0);
    }

    #[test]
    fn initializes_uninitialized_aligned_storage() {
        let mut storage = core::mem::MaybeUninit::<A20VirtioBlkQueueState>::uninit();
        assert_eq!(unsafe { a20_virtio_blk_queue_init(storage.as_mut_ptr(), 16, 1) }, 0);
        let mut queue = unsafe { storage.assume_init() };
        let (mut slot, mut head) = (u16::MAX, u16::MAX);
        assert_eq!(unsafe { a20_virtio_blk_queue_reserve(&mut queue, 2, 1, 0,
                                                        &mut slot, &mut head) }, 0);
        assert_eq!((slot, head), (0, 0));
    }

    #[test]
    fn exhaustion_invalid_completion_and_reset_failure_reclaim() {
        let mut q = A20VirtioBlkQueueState::default();
        assert_eq!(unsafe { a20_virtio_blk_queue_init(&mut q, 4, 2) }, 0);
        let (mut slot0, mut head0) = (0, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_reserve(&mut q, 3, 513, 0,
                                                        &mut slot0, &mut head0) }, 0);
        let (mut slot1, mut head1) = (0, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_reserve(&mut q, 2, 1, 1,
                                                        &mut slot1, &mut head1) }, ENOSPC);
        assert_eq!(unsafe { a20_virtio_blk_queue_prepare_publish(&mut q, slot0,
                                                                 &mut slot1) }, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_commit_publish(&mut q, slot0,
                                                                &mut slot1) }, 0);
        let mut ring = 0;
        assert_eq!(unsafe { a20_virtio_blk_queue_used_ring_slot(&mut q, 5, &mut ring) }, EPROTO);
        let (mut completed_slot, mut result) = (0, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_complete_used(&mut q, 1, 3, 513, 0,
                  &mut completed_slot, &mut result) }, EPROTO);
        assert_eq!(unsafe { a20_virtio_blk_queue_validate_head(&mut q, 4,
                                                                &mut completed_slot) }, EPROTO);
        assert_eq!(unsafe { a20_virtio_blk_queue_validate_head(&mut q, 3,
                                                                &mut completed_slot) }, EPROTO);
        assert_eq!(unsafe { a20_virtio_blk_queue_validate_head(&mut q, head0,
                                                                &mut completed_slot) }, 0);
        assert_eq!(completed_slot, slot0);
        assert_eq!(unsafe { a20_virtio_blk_queue_reset(&mut q) }, EBUSY);
        let mut failed = [u16::MAX; 2];
        assert_eq!(unsafe { a20_virtio_blk_queue_fail_all(&mut q, failed.as_mut_ptr(), 2) }, 1);
        assert_eq!(failed[0], slot0);
        assert_eq!(unsafe { a20_virtio_blk_queue_reset(&mut q) }, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_release(&mut q, slot0) }, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_reserve(&mut q, 3, 1, 0,
                                                        &mut slot0, &mut head0) }, 0);
    }

    #[test]
    fn failed_status_or_short_length_returns_io_error_and_cancels_rollback() {
        let mut q = A20VirtioBlkQueueState::default();
        assert_eq!(unsafe { a20_virtio_blk_queue_init(&mut q, 8, 2) }, 0);
        let (mut slot, mut head, mut ring_slot, mut avail_idx) = (0, 0, 0, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_reserve(&mut q, 2, 1, 1,
                                                        &mut slot, &mut head) }, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_cancel(&mut q, slot) }, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_reserve(&mut q, 2, 1, 1,
                                                        &mut slot, &mut head) }, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_prepare_publish(&mut q, slot,
                                                                 &mut ring_slot) }, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_commit_publish(&mut q, slot,
                                                                &mut avail_idx) }, 0);
        let (mut completed_slot, mut result) = (0, 0);
        assert_eq!(unsafe { a20_virtio_blk_queue_complete_used(&mut q, 1, head, 0, 1,
                  &mut completed_slot, &mut result) }, 1);
        assert_eq!(result, EIO);
        assert_eq!(unsafe { a20_virtio_blk_queue_release(&mut q, slot) }, 0);
    }

    #[test]
    fn descriptor_run_wraps_without_reusing_live_bits() {
        let mut q = A20VirtioBlkQueueState::default();
        assert_eq!(unsafe { a20_virtio_blk_queue_init(&mut q, 8, 2) }, 0);
        let (mut slot, mut head, mut ring, mut avail) = (0, 0, 0, 0);
        for expected_head in [0, 3, 6] {
            assert_eq!(unsafe { a20_virtio_blk_queue_reserve(&mut q, 3, 1, 0,
                                                            &mut slot, &mut head) }, 0);
            assert_eq!(head, expected_head);
            if expected_head == 6 {
                assert_eq!(head.wrapping_add(2) & 7, 0);
            }
            assert_eq!(q.descriptor_bitmap.count_ones(), 3);
            assert_eq!(unsafe { a20_virtio_blk_queue_prepare_publish(&mut q, slot,
                                                                     &mut ring) }, 0);
            assert_eq!(unsafe { a20_virtio_blk_queue_commit_publish(&mut q, slot,
                                                                    &mut avail) }, 0);
            let (mut completed, mut result) = (u16::MAX, i32::MIN);
            assert_eq!(unsafe { a20_virtio_blk_queue_complete_used(&mut q, avail,
                      head, 1, 0, &mut completed, &mut result) }, 1);
            assert_eq!((completed, result), (slot, 0));
            assert_eq!(unsafe { a20_virtio_blk_queue_release(&mut q, slot) }, 0);
            assert_eq!(q.descriptor_bitmap, 0);
        }
    }

    #[test]
    fn negotiated_32_and_64_queues_reserve_cancel_and_reuse_descriptors() {
        for queue_size in [32u16, 64u16] {
            let slot_count = queue_size / 3;
            let mut q = A20VirtioBlkQueueState::default();
            assert_eq!(unsafe {
                a20_virtio_blk_queue_init(&mut q, queue_size, slot_count)
            }, 0);

            let run_mask = |head: u16| {
                (0..3).fold(0u64, |mask, offset| {
                    mask | (1u64 << (head.wrapping_add(offset) & (queue_size - 1)))
                })
            };
            let mut live = vec![None; slot_count as usize];
            let mut expected_bitmap = 0u64;

            /* Fill every active request slot with a nonoverlapping three
             * descriptor chain. Queue depth 64 uses bitmap bit 63. */
            for index in 0..slot_count as usize {
                let (mut slot, mut head) = (u16::MAX, u16::MAX);
                assert_eq!(unsafe {
                    a20_virtio_blk_queue_reserve(&mut q, 3, 1, 0, &mut slot, &mut head)
                }, 0);
                assert_eq!(slot as usize, index);
                let mask = run_mask(head);
                assert_eq!(expected_bitmap & mask, 0, "overlapping live chain");
                expected_bitmap |= mask;
                live[index] = Some((slot, head));
                assert_eq!(q.descriptor_bitmap, expected_bitmap);
                assert_eq!(q.descriptor_bitmap.count_ones(), 3 * (index as u32 + 1));
            }
            assert_eq!(q.descriptor_bitmap.count_ones(), 3 * slot_count as u32);

            let (mut extra_slot, mut extra_head) = (u16::MAX, u16::MAX);
            assert_eq!(unsafe {
                a20_virtio_blk_queue_reserve(
                    &mut q, 3, 1, 0, &mut extra_slot, &mut extra_head,
                )
            }, ENOSPC);

            /* Free slot zero, then reserve at the fixed cursor: the chain
             * crosses the queue's last descriptor and wraps to descriptor 0. */
            let (first_slot, first_head) = live[0].take().unwrap();
            assert_eq!(first_head, 0);
            assert_eq!(unsafe { a20_virtio_blk_queue_cancel(&mut q, first_slot) }, 0);
            expected_bitmap &= !run_mask(first_head);
            assert_eq!(q.descriptor_bitmap, expected_bitmap);

            let (mut wrapped_slot, mut wrapped_head) = (u16::MAX, u16::MAX);
            assert_eq!(unsafe {
                a20_virtio_blk_queue_reserve(
                    &mut q, 3, 1, 0, &mut wrapped_slot, &mut wrapped_head,
                )
            }, 0);
            let expected_cursor = (3 * slot_count) & (queue_size - 1);
            assert_eq!(wrapped_head, expected_cursor);
            assert!(wrapped_head + 2 >= queue_size);
            let wrapped_mask = run_mask(wrapped_head);
            assert_eq!(expected_bitmap & wrapped_mask, 0);
            expected_bitmap |= wrapped_mask;
            live[wrapped_slot as usize] = Some((wrapped_slot, wrapped_head));
            assert_eq!(q.descriptor_bitmap, expected_bitmap);

            /* Cancel a non-first live request out of order, then prove the
             * allocator reuses exactly that freed run without touching peers. */
            let middle = (slot_count / 2) as usize;
            let (middle_slot, middle_head) = live[middle].take().unwrap();
            assert_eq!(unsafe { a20_virtio_blk_queue_cancel(&mut q, middle_slot) }, 0);
            expected_bitmap &= !run_mask(middle_head);
            let (mut reused_slot, mut reused_head) = (u16::MAX, u16::MAX);
            assert_eq!(unsafe {
                a20_virtio_blk_queue_reserve(
                    &mut q, 3, 1, 0, &mut reused_slot, &mut reused_head,
                )
            }, 0);
            assert_eq!(reused_slot, middle_slot);
            assert_eq!(reused_head, middle_head);
            assert_eq!(expected_bitmap & run_mask(reused_head), 0);
            expected_bitmap |= run_mask(reused_head);
            live[reused_slot as usize] = Some((reused_slot, reused_head));
            assert_eq!(q.descriptor_bitmap, expected_bitmap);

            for request in live.into_iter().flatten() {
                assert_eq!(unsafe { a20_virtio_blk_queue_cancel(&mut q, request.0) }, 0);
            }
            assert_eq!(q.descriptor_bitmap, 0);
            assert_eq!(q.active_count, 0);
        }
    }
}
