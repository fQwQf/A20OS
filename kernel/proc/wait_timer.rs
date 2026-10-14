#![no_std]

//! Wait-timeout heap ownership.
//!
//! The C timer module supplies the IRQ-safe heap lock, task reference
//! callbacks, and timer re-arm primitive. This module owns heap membership,
//! index publication, capacity checks, cancellation, and expiration batching.
//! Expired references stay owned by the batch until the park transition has
//! completed outside the heap lock.

use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::ptr;

// The maximum supported kernel timer profile is 8192 entries. Smaller
// profiles are enforced by the C capacity callback without changing this
// fixed backing array's ABI or requiring cfg plumbing through every build.
const STORAGE_CAPACITY: usize = 8192;

const MAX_EXPIRE_BATCH: usize = 128;
const SCHED_NO_DEADLINE: u64 = u64::MAX;
const PREPARE_INVALID: i32 = -1;
const PREPARE_CAPACITY: i32 = -2;
const PREPARE_DUPLICATE: i32 = -3;
const PREPARE_REF: i32 = -4;

#[derive(Clone, Copy)]
struct Entry {
    deadline: u64,
    task: *mut c_void,
    seq: u64,
}

const EMPTY_ENTRY: Entry = Entry {
    deadline: 0,
    task: ptr::null_mut(),
    seq: 0,
};

struct Heap {
    entries: [Entry; STORAGE_CAPACITY],
    len: usize,
    full_failures: u64,
    duplicate_rejections: u64,
    stale_expirations: u64,
}

impl Heap {
    const fn new() -> Self {
        Self {
            entries: [EMPTY_ENTRY; STORAGE_CAPACITY],
            len: 0,
            full_failures: 0,
            duplicate_rejections: 0,
            stale_expirations: 0,
        }
    }
}

// All accesses are bounded by heap.len/capacity or a fixed bounded loop.
// Avoid generating Rust's panicking bounds-check runtime in the kernel object.
unsafe fn entry(heap: &Heap, index: usize) -> Entry {
    *heap.entries.get_unchecked(index)
}

unsafe fn set_entry(heap: &mut Heap, index: usize, value: Entry) {
    *heap.entries.get_unchecked_mut(index) = value;
}

struct SharedHeap(UnsafeCell<Heap>);

// SAFETY: every access to the cell is serialized by the C IRQ-saving lock
// returned from a20_wait_timer_lock(). No reference escapes an exported call.
unsafe impl Sync for SharedHeap {}

static HEAP: SharedHeap = SharedHeap(UnsafeCell::new(Heap::new()));

unsafe extern "C" {
    fn a20_wait_timer_lock() -> u64;
    fn a20_wait_timer_unlock(flags: u64);
    fn a20_wait_timer_capacity() -> u32;
    fn a20_wait_timer_task_ref(task: *mut c_void) -> *mut c_void;
    fn a20_wait_timer_task_put(task: *mut c_void);
    fn a20_wait_timer_task_index(task: *mut c_void) -> i32;
    fn a20_wait_timer_set_task_index(task: *mut c_void, index: i32);
    fn a20_wait_timer_set_wake_time(task: *mut c_void, deadline: u64);
    fn a20_wait_timer_publish_next(deadline: u64);
    fn a20_wait_timer_rearm_if_earlier(deadline: u64);
    fn a20_wait_timer_entry_valid(
        task: *mut c_void,
        index: u32,
        deadline: u64,
        seq: u64,
    ) -> i32;
    fn a20_wait_timer_fire_timeout(task: *mut c_void, seq: u64) -> i32;
    fn a20_wait_timer_after_dequeue(task: *mut c_void, seq: u64);
    fn proc_wait_timer_test_after_fire(task: *mut c_void, seq: u64, result: i32);
}

struct HeapLock(u64);

impl HeapLock {
    fn acquire() -> Self {
        // SAFETY: the matching unlock is owned by this guard.
        Self(unsafe { a20_wait_timer_lock() })
    }
}

impl Drop for HeapLock {
    fn drop(&mut self) {
        // SAFETY: this guard uniquely owns the IRQ-saving lock state.
        unsafe { a20_wait_timer_unlock(self.0) };
    }
}

fn with_heap<R>(f: impl FnOnce(&mut Heap) -> R) -> R {
    let _lock = HeapLock::acquire();
    // SAFETY: _lock excludes every other access; the mutable reference does
    // not leave this function and callbacks made by f cannot re-enter Rust.
    f(unsafe { &mut *HEAP.0.get() })
}

fn capacity() -> usize {
    // SAFETY: this callback returns the same validated build-time capacity as
    // the C reference heap. Clamp to storage as a defensive ABI check.
    (unsafe { a20_wait_timer_capacity() } as usize).min(STORAGE_CAPACITY)
}

fn swap(heap: &mut Heap, a: usize, b: usize) {
    let a_entry = unsafe { entry(heap, a) };
    let b_entry = unsafe { entry(heap, b) };
    unsafe {
        set_entry(heap, a, b_entry);
        set_entry(heap, b, a_entry);
    }
    let a_task = b_entry.task;
    let b_task = a_entry.task;
    // SAFETY: live heap entries own valid task references; null is ignored by
    // the C adapter so corrupted entries cannot cause a Rust dereference.
    unsafe {
        a20_wait_timer_set_task_index(a_task, a as i32);
        a20_wait_timer_set_task_index(b_task, b as i32);
    }
}

fn sift_up(heap: &mut Heap, mut index: usize) {
    while index > 0 {
        let parent = (index - 1) / 2;
        if unsafe { entry(heap, parent).deadline <= entry(heap, index).deadline } {
            break;
        }
        swap(heap, parent, index);
        index = parent;
    }
}

fn sift_down(heap: &mut Heap, mut index: usize) {
    loop {
        let left = index.saturating_mul(2).saturating_add(1);
        let right = left.saturating_add(1);
        let mut smallest = index;
        if left < heap.len &&
            unsafe { entry(heap, left).deadline < entry(heap, smallest).deadline }
        {
            smallest = left;
        }
        if right < heap.len &&
            unsafe { entry(heap, right).deadline < entry(heap, smallest).deadline }
        {
            smallest = right;
        }
        if smallest == index {
            break;
        }
        swap(heap, index, smallest);
        index = smallest;
    }
}

fn remove(heap: &mut Heap, index: usize) -> Entry {
    if index >= heap.len {
        return EMPTY_ENTRY;
    }
    let removed = unsafe { entry(heap, index) };
    // SAFETY: removed.task's timer reference is transferred to the caller.
    unsafe { a20_wait_timer_set_task_index(removed.task, -1) };
    heap.len -= 1;
    if index != heap.len {
        let moved = unsafe { entry(heap, heap.len) };
        unsafe { set_entry(heap, index, moved) };
        // SAFETY: the moved entry remains owned by the heap.
        unsafe {
            a20_wait_timer_set_task_index(moved.task, index as i32)
        };
        if index > 0 &&
            unsafe { entry(heap, index).deadline < entry(heap, (index - 1) / 2).deadline }
        {
            sift_up(heap, index);
        } else {
            sift_down(heap, index);
        }
    }
    unsafe { set_entry(heap, heap.len, EMPTY_ENTRY) };
    // SAFETY: only the lock-owning thread can change the root here.
    unsafe {
        a20_wait_timer_publish_next(
            if heap.len == 0 { SCHED_NO_DEADLINE }
            else { entry(heap, 0).deadline },
        );
    }
    removed
}

#[no_mangle]
pub extern "C" fn a20_wait_timer_init_rust() {
    with_heap(|heap| {
        // Called during scheduler initialization before tasks or timer entries
        // are published, matching the C heap's startup-only reset contract.
        heap.entries.fill(EMPTY_ENTRY);
        heap.len = 0;
        heap.full_failures = 0;
        heap.duplicate_rejections = 0;
        heap.stale_expirations = 0;
    });
}

#[no_mangle]
pub extern "C" fn a20_wait_timer_register_rust(
    task: *mut c_void,
    deadline: u64,
    seq: u64,
) -> i32 {
    if task.is_null() || deadline == 0 || seq == 0 {
        return PREPARE_INVALID;
    }
    let mut owned = ptr::null_mut();
    let result = with_heap(|heap| {
        // SAFETY: task is a live current task protected by its park lock; this
        // atomic index read is serialized with all heap membership updates.
        if unsafe { a20_wait_timer_task_index(task) } >= 0 {
            heap.duplicate_rejections = heap.duplicate_rejections.wrapping_add(1);
            return PREPARE_DUPLICATE;
        }
        let limit = capacity();
        if heap.len >= limit || heap.len >= STORAGE_CAPACITY {
            heap.full_failures = heap.full_failures.wrapping_add(1);
            return PREPARE_CAPACITY;
        }
        // Capacity is checked before pinning, so a failed full registration
        // cannot acquire or leak a timer-owned task reference.
        owned = unsafe { a20_wait_timer_task_ref(task) };
        if owned.is_null() {
            return PREPARE_REF;
        }
        let old_first = if heap.len == 0 { SCHED_NO_DEADLINE }
                        else { unsafe { entry(heap, 0).deadline } };
        let index = heap.len;
        unsafe { set_entry(heap, index, Entry { deadline, task: owned, seq }) };
        heap.len += 1;
        // SAFETY: owned is a successful proc_get reference.
        unsafe {
            a20_wait_timer_set_task_index(owned, index as i32);
            a20_wait_timer_set_wake_time(task, deadline);
        }
        sift_up(heap, index);
        let first = unsafe { entry(heap, 0).deadline };
        if first < old_first {
            // SAFETY: this callback only publishes/rearms the kernel timer.
            unsafe { a20_wait_timer_rearm_if_earlier(first) };
        }
        0
    });
    if result != 0 && !owned.is_null() {
        // Defensive only: the current transaction has no post-pin failure.
        unsafe { a20_wait_timer_task_put(owned) };
    }
    result
}

#[no_mangle]
pub extern "C" fn a20_wait_timer_cancel_rust(task: *mut c_void, seq: u64) {
    if task.is_null() {
        return;
    }
    let removed = with_heap(|heap| {
        // SAFETY: caller holds task->park_lock; the index is atomically
        // published and checked against both identity and wait generation.
        let index = unsafe { a20_wait_timer_task_index(task) };
        if index < 0 {
            return EMPTY_ENTRY;
        }
        let index = index as usize;
        if index >= heap.len || unsafe { entry(heap, index).task != task ||
            entry(heap, index).seq != seq }
        {
            return EMPTY_ENTRY;
        }
        let removed = remove(heap, index);
        unsafe { a20_wait_timer_set_wake_time(task, 0) };
        removed
    });
    if !removed.task.is_null() {
        // Release after dropping the heap lock: a final put must not re-enter
        // task teardown while the timer lock is held.
        unsafe { a20_wait_timer_task_put(removed.task) };
    }
}

#[repr(C)]
pub struct WaitTimerStats {
    pub entries: u32,
    pub capacity: u32,
    pub full_failures: u64,
    pub duplicate_rejections: u64,
    pub stale_expirations: u64,
    pub violations: u32,
}

#[no_mangle]
/// # Safety
/// `out` points to a writable, correctly aligned `WaitTimerStats` value.
pub unsafe extern "C" fn a20_wait_timer_stats_rust(out: *mut WaitTimerStats) {
    if out.is_null() {
        return;
    }
    let stats = with_heap(|heap| {
        let limit = capacity();
        let mut violations = u32::from(heap.len > limit || heap.len > STORAGE_CAPACITY);
        let bound = heap.len.min(limit).min(STORAGE_CAPACITY);
        for i in 0..bound {
            let item = unsafe { entry(heap, i) };
            if item.task.is_null() || item.deadline == 0 || item.seq == 0 ||
                unsafe {
                    a20_wait_timer_entry_valid(item.task, i as u32,
                                              item.deadline, item.seq)
                } == 0
            {
                violations = violations.saturating_add(1);
            }
            if i > 0 && unsafe { entry(heap, (i - 1) / 2).deadline > item.deadline } {
                violations = violations.saturating_add(1);
            }
        }
        WaitTimerStats {
            entries: heap.len.min(u32::MAX as usize) as u32,
            capacity: limit.min(u32::MAX as usize) as u32,
            full_failures: heap.full_failures,
            duplicate_rejections: heap.duplicate_rejections,
            stale_expirations: heap.stale_expirations,
            violations,
        }
    });
    // SAFETY: caller guarantees the output buffer contract above.
    unsafe { out.write(stats) };
}

#[derive(Clone, Copy)]
struct Expired {
    task: *mut c_void,
    seq: u64,
}

const EMPTY_EXPIRED: Expired = Expired { task: ptr::null_mut(), seq: 0 };

#[no_mangle]
pub extern "C" fn a20_wait_timer_expire_rust(now: u64) {
    loop {
        let mut expired = [EMPTY_EXPIRED; MAX_EXPIRE_BATCH];
        let count = with_heap(|heap| {
            let mut count = 0;
            while heap.len > 0 && unsafe { entry(heap, 0).deadline <= now } &&
                count < MAX_EXPIRE_BATCH
            {
                let entry = remove(heap, 0);
                if !entry.task.is_null() {
                    expired[count] = Expired { task: entry.task, seq: entry.seq };
                    count += 1;
                }
            }
            count
        });
        for entry in expired.iter().take(count) {
            if entry.seq != 0 {
                // Test hook, when enabled, pauses after collection and before
                // park_lock acquisition to exercise cancel/expire races.
                unsafe { a20_wait_timer_after_dequeue(entry.task, entry.seq) };
                let result = unsafe { a20_wait_timer_fire_timeout(entry.task, entry.seq) };
                unsafe {
                    proc_wait_timer_test_after_fire(entry.task, entry.seq, result)
                };
                if result == 2 {
                    with_heap(|heap| {
                        heap.stale_expirations = heap.stale_expirations.wrapping_add(1);
                    });
                }
            }
            // This batch retained the removed entry's task reference until the
            // park transition has either won or observed a stale sequence.
            unsafe { a20_wait_timer_task_put(entry.task) };
        }
        if count < MAX_EXPIRE_BATCH {
            break;
        }
    }
}
