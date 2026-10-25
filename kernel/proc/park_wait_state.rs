#![no_std]

use core::ffi::c_void;
use core::sync::atomic::{AtomicI32, Ordering};
#[cfg(not(target_arch = "riscv32"))]
use core::sync::atomic::AtomicU64;

const PARK_IDLE: i32 = 0;
const PARK_PREPARING: i32 = 1;
const PARK_PARKED: i32 = 2;
const PARK_WOKEN: i32 = 3;
const WAIT_UNINTERRUPTIBLE: i32 = 0;
const WAIT_INTERRUPTIBLE: i32 = 1;
const WAIT_KILLABLE: i32 = 2;
const WAKE_SIGNAL: i32 = 4;
const WAKE_FATAL_SIGNAL: i32 = 5;
const WAKE_TASK_EXIT: i32 = 6;
const WAKE_CANCEL: i32 = 8;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ParkWaitFields {
    wait_seq: *mut u64,
    wait_deadline: *mut u64,
    park_state: *mut i32,
    wait_mode: *mut i32,
    wake_reason: *mut i32,
}

type CancelTimer = unsafe extern "C" fn(*mut c_void, u64);

#[cfg(target_arch = "riscv32")]
unsafe extern "C" {
    // RV32 has no native 64-bit atomic instruction. These adapters use the
    // kernel's existing IRQ-safe atomic64 implementation. Rust still owns
    // wait-state transitions; the C bridge performs only indivisible field
    // accesses with the requested memory ordering.
    fn a20_park_wait_load_u64_relaxed(ptr: *mut u64) -> u64;
    fn a20_park_wait_load_u64_acquire(ptr: *mut u64) -> u64;
    fn a20_park_wait_store_u64_relaxed(ptr: *mut u64, value: u64);
    fn a20_park_wait_store_u64_release(ptr: *mut u64, value: u64);
}

#[inline]
unsafe fn fields_from(raw: *mut ParkWaitFields) -> Option<ParkWaitFields> {
    if raw.is_null() { return None; }
    // SAFETY: the C adapter passes a live, aligned stack view. Copying this
    // pointer-only value does not borrow any task fields across callbacks.
    let fields = unsafe { raw.read() };
    if fields.wait_seq.is_null() || fields.wait_deadline.is_null() ||
       fields.park_state.is_null() || fields.wait_mode.is_null() ||
       fields.wake_reason.is_null() { return None; }
    Some(fields)
}

#[inline]
unsafe fn load_u64(ptr: *mut u64, order: Ordering) -> u64 {
    // SAFETY: the C layout gives u64 fields native alignment and validity.
    #[cfg(target_arch = "riscv32")]
    {
        return if order == Ordering::Acquire {
            unsafe { a20_park_wait_load_u64_acquire(ptr) }
        } else {
            unsafe { a20_park_wait_load_u64_relaxed(ptr) }
        };
    }
    #[cfg(not(target_arch = "riscv32"))]
    unsafe { (&*(ptr as *const AtomicU64)).load(order) }
}

#[inline]
unsafe fn store_u64(ptr: *mut u64, value: u64, order: Ordering) {
    // SAFETY: the C layout gives u64 fields native alignment and validity.
    #[cfg(target_arch = "riscv32")]
    {
        if order == Ordering::Release {
            unsafe { a20_park_wait_store_u64_release(ptr, value) }
        } else {
            unsafe { a20_park_wait_store_u64_relaxed(ptr, value) }
        }
        return;
    }
    #[cfg(not(target_arch = "riscv32"))]
    unsafe { (&*(ptr as *const AtomicU64)).store(value, order) }
}

#[inline]
unsafe fn load_i32(ptr: *mut i32) -> i32 {
    // SAFETY: i32 task fields are naturally aligned and valid.
    unsafe { (&*(ptr as *const AtomicI32)).load(Ordering::Relaxed) }
}

#[inline]
unsafe fn store_i32(ptr: *mut i32, value: i32) {
    // SAFETY: all shared state fields use atomic accesses, including fields
    // read by diagnostics outside park_lock.
    unsafe { (&*(ptr as *const AtomicI32)).store(value, Ordering::Relaxed) }
}

#[inline]
unsafe fn store_reason(ptr: *mut i32, reason: i32) {
    // SAFETY: same aligned field contract; atomic publication pairs with the
    // lock-free post-sched reader, while all other accesses are lock-ordered.
    unsafe { (&*(ptr as *const AtomicI32)).store(reason, Ordering::Release) }
}

#[inline]
unsafe fn cancel_timer(callback: Option<CancelTimer>, context: *mut c_void, seq: u64) {
    if let Some(callback) = callback {
        // SAFETY: callback contract is park_lock -> timer-lock, no sleep or
        // re-entry. It mutates only timer-heap-owned fields outside this view.
        unsafe { callback(context, seq) };
    }
}

#[no_mangle]
/// Initialize one task's logical wait fields before the task is published.
///
/// # Safety
/// `raw` must point to a live, correctly aligned `ParkWaitFields` view built
/// by the kernel's sole C adapter. Its five pointers must each point to the
/// corresponding live, correctly aligned task field; the fields must be
/// distinct, non-overlapping, and accessed atomically whenever another CPU
/// can observe them. The task must not yet be visible to any other CPU or
/// timer callback.
pub unsafe extern "C" fn a20_rust_park_wait_init(raw: *mut ParkWaitFields) {
    let Some(f) = (unsafe { fields_from(raw) }) else { return; };
    unsafe {
        store_u64(f.wait_seq, 0, Ordering::Relaxed);
        store_u64(f.wait_deadline, 0, Ordering::Relaxed);
        store_i32(f.park_state, PARK_IDLE);
        store_i32(f.wait_mode, WAIT_UNINTERRUPTIBLE);
        store_reason(f.wake_reason, 0);
    }
}

#[no_mangle]
/// Begin a new wait generation while the caller holds the task's `park_lock`.
///
/// # Safety
/// `raw` must satisfy the `ParkWaitFields` contract documented on
/// `a20_rust_park_wait_init`; the task storage must remain live for the call.
/// The caller must hold `park_lock`. `seq_out` must be writable, correctly
/// aligned for `u64`, and must not overlap the view or any of its task fields.
pub unsafe extern "C" fn a20_rust_park_wait_prepare(
    raw: *mut ParkWaitFields, mode: i32, deadline: u64, seq_out: *mut u64,
) -> i32 {
    let Some(f) = (unsafe { fields_from(raw) }) else { return -1; };
    if seq_out.is_null() || !(WAIT_UNINTERRUPTIBLE..=WAIT_KILLABLE).contains(&mode) {
        return -1;
    }
    unsafe {
        if load_i32(f.park_state) != PARK_IDLE { return -1; }
        let mut seq = load_u64(f.wait_seq, Ordering::Relaxed).wrapping_add(1);
        if seq == 0 { seq = 1; }
        store_u64(f.wait_seq, seq, Ordering::Release);
        store_u64(f.wait_deadline, deadline, Ordering::Relaxed);
        store_i32(f.wait_mode, mode);
        store_reason(f.wake_reason, 0);
        store_i32(f.park_state, PARK_PREPARING);
        seq_out.write(seq);
    }
    0
}

#[no_mangle]
/// Roll back a PREPARING generation after timer registration failed.
///
/// # Safety
/// `raw` must satisfy the `ParkWaitFields` contract documented on
/// `a20_rust_park_wait_init`; the task storage must remain live and the caller
/// must hold `park_lock` for the entire call.
pub unsafe extern "C" fn a20_rust_park_wait_prepare_rollback(
    raw: *mut ParkWaitFields, seq: u64,
) {
    let Some(f) = (unsafe { fields_from(raw) }) else { return; };
    if seq == 0 { return; }
    unsafe {
        if load_u64(f.wait_seq, Ordering::Relaxed) != seq ||
           load_i32(f.park_state) != PARK_PREPARING { return; }
        store_u64(f.wait_deadline, 0, Ordering::Relaxed);
        store_i32(f.wait_mode, WAIT_UNINTERRUPTIBLE);
        store_reason(f.wake_reason, 0);
        store_i32(f.park_state, PARK_IDLE);
    }
}

#[no_mangle]
/// Publish a signal, timeout, exit, or other wake for the matching generation.
///
/// # Safety
/// `raw` must satisfy the `ParkWaitFields` contract documented on
/// `a20_rust_park_wait_init`; the task storage must remain live and the caller
/// must hold `park_lock`. If `callback` is present, `context` must remain live
/// and the callback must obey the C header's park-lock-to-timer-lock contract:
/// it may not sleep or re-enter park logic and may mutate only timer-owned
/// fields outside this view.
pub unsafe extern "C" fn a20_rust_park_wait_wake(
    raw: *mut ParkWaitFields, seq: u64, reason: i32,
    callback: Option<CancelTimer>, context: *mut c_void,
) -> i32 {
    let Some(f) = (unsafe { fields_from(raw) }) else { return 0; };
    if seq == 0 { return 0; }
    // PARK_SIGNAL_MODE_PROTOCOL: a plain signal wake only lands on
    // WAIT_INTERRUPTIBLE parkers; a fatal signal or task exit bypasses the
    // mode check but still loses to WAIT_UNINTERRUPTIBLE.  The generation
    // (seq) comparison above decides which wake wins the race.
    unsafe {
        if load_u64(f.wait_seq, Ordering::Relaxed) != seq { return 0; }
        let mode = load_i32(f.wait_mode);
        if reason == WAKE_SIGNAL && mode != WAIT_INTERRUPTIBLE { return 0; }
        if (reason == WAKE_FATAL_SIGNAL || reason == WAKE_TASK_EXIT) &&
           mode == WAIT_UNINTERRUPTIBLE { return 0; }
        let action = match load_i32(f.park_state) {
            PARK_PREPARING => 1,
            PARK_PARKED => 2,
            _ => 0,
        };
        if action == 0 { return 0; }
        // No reference to task fields is live across this callback; only the
        // copied pointer view is held. Timer indices are outside the view.
        cancel_timer(callback, context, seq);
        store_i32(f.park_state, PARK_WOKEN);
        store_reason(f.wake_reason, reason);
        store_u64(f.wait_deadline, 0, Ordering::Relaxed);
        action
    }
}

#[no_mangle]
/// Cancel a PREPARING generation and publish its cancellation reason.
///
/// # Safety
/// `raw` must satisfy the `ParkWaitFields` contract documented on
/// `a20_rust_park_wait_init`; the task storage must remain live and the caller
/// must hold `park_lock`. Any callback/context pair must satisfy the same
/// timer-callback contract as `a20_rust_park_wait_wake`.
pub unsafe extern "C" fn a20_rust_park_wait_cancel(
    raw: *mut ParkWaitFields, seq: u64,
    callback: Option<CancelTimer>, context: *mut c_void,
) -> i32 {
    let Some(f) = (unsafe { fields_from(raw) }) else { return 0; };
    if seq == 0 { return 0; }
    // PARK_SIGNAL_MODE_PROTOCOL: a plain signal wake only lands on
    // WAIT_INTERRUPTIBLE parkers; a fatal signal or task exit bypasses the
    // mode check but still loses to WAIT_UNINTERRUPTIBLE.  The generation
    // (seq) comparison above decides which wake wins the race.
    unsafe {
        if load_u64(f.wait_seq, Ordering::Relaxed) != seq ||
           load_i32(f.park_state) != PARK_PREPARING { return 0; }
        cancel_timer(callback, context, seq);
        store_u64(f.wait_deadline, 0, Ordering::Relaxed);
        store_reason(f.wake_reason, WAKE_CANCEL);
        store_i32(f.park_state, PARK_WOKEN);
        1
    }
}

#[no_mangle]
/// Commit a prepared generation to PARKED, or return its early wake reason.
///
/// # Safety
/// `raw` must satisfy the `ParkWaitFields` contract documented on
/// `a20_rust_park_wait_init`; the task storage must remain live and the caller
/// must hold `park_lock`. `reason_out` must be writable, correctly aligned for
/// `i32`, and must not overlap the view or any task field.
pub unsafe extern "C" fn a20_rust_park_wait_commit(
    raw: *mut ParkWaitFields, seq: u64, reason_out: *mut i32,
) -> i32 {
    let Some(f) = (unsafe { fields_from(raw) }) else { return -1; };
    if seq == 0 || reason_out.is_null() { return -1; }
    unsafe {
        if load_u64(f.wait_seq, Ordering::Relaxed) != seq { return -1; }
        match load_i32(f.park_state) {
            PARK_WOKEN => {
                reason_out.write((&*(f.wake_reason as *const AtomicI32))
                    .load(Ordering::Acquire));
                0
            }
            PARK_PREPARING => {
                store_i32(f.park_state, PARK_PARKED);
                1
            }
            _ => -1,
        }
    }
}

#[no_mangle]
/// Finish a WOKEN generation and release-publish IDLE.
///
/// # Safety
/// `raw` must satisfy the `ParkWaitFields` contract documented on
/// `a20_rust_park_wait_init`, and the task must remain live. With nonzero
/// `timer_pending`, the caller must hold `park_lock` and provide a valid
/// callback/context pair satisfying the timer-callback contract. With zero
/// `timer_pending`, the caller must be the current task, have no registered
/// timer for this generation, and invoke the lock-free fast path. In either
/// case, the caller must clear `wake_time` before this function publishes IDLE.
pub unsafe extern "C" fn a20_rust_park_wait_finish(
    raw: *mut ParkWaitFields, seq: u64, timer_pending: i32,
    callback: Option<CancelTimer>, context: *mut c_void,
) -> i32 {
    let Some(f) = (unsafe { fields_from(raw) }) else { return 0; };
    if seq == 0 { return 0; }
    // PARK_SIGNAL_MODE_PROTOCOL: a plain signal wake only lands on
    // WAIT_INTERRUPTIBLE parkers; a fatal signal or task exit bypasses the
    // mode check but still loses to WAIT_UNINTERRUPTIBLE.  The generation
    // (seq) comparison above decides which wake wins the race.
    unsafe {
        if load_u64(f.wait_seq, Ordering::Relaxed) != seq ||
           load_i32(f.park_state) != PARK_WOKEN { return 0; }
        if timer_pending != 0 { cancel_timer(callback, context, seq); }
        store_u64(f.wait_deadline, 0, Ordering::Relaxed);
        store_i32(f.wait_mode, WAIT_UNINTERRUPTIBLE);
        store_reason(f.wake_reason, 0);
        (&*(f.park_state as *const AtomicI32)).store(PARK_IDLE, Ordering::Release);
        1
    }
}

#[no_mangle]
/// Read a generation's wake reason after the task resumes from `sched()`.
///
/// # Safety
/// `raw` must point to a live, correctly aligned view built by the kernel's
/// sole C adapter. The sequence and reason pointers must remain valid and be
/// accessed atomically while this lock-free read can race with another CPU's
/// wake transition. The task must remain alive through the call.
pub unsafe extern "C" fn a20_rust_park_wait_reason(
    raw: *const ParkWaitFields, seq: u64,
) -> i32 {
    if raw.is_null() || seq == 0 { return WAKE_CANCEL; }
    // SAFETY: this reads the caller's local pointer view only. Its pointed-to
    // sequence/reason fields are read atomically after the task resumes.
    let f = unsafe { raw.read() };
    if f.wait_seq.is_null() || f.wake_reason.is_null() { return WAKE_CANCEL; }
    unsafe {
        let got_seq = load_u64(f.wait_seq, Ordering::Acquire);
        if got_seq != seq { return WAKE_CANCEL; }
        (&*(f.wake_reason as *const AtomicI32)).load(Ordering::Acquire)
    }
}
