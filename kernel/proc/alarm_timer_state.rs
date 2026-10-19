#![no_std]

//! Per-task SIGALRM (`ITIMER_REAL`) deadline heap.
//!
//! Rust owns the heap itself: the array and count, the sift/remove mechanics,
//! the per-task index publication, the capacity decision, and the expiry scan
//! that pops due entries in bounded batches and reports whether more remain.
//! C supplies the lock, the task reference, the task's published alarm
//! deadline, the scheduler re-arm and the signal delivery of one expired
//! entry.
//!
//! The heap root is the next alarm deadline, so expiry pops only due entries
//! instead of scanning every task.  Entries hold a task reference that is
//! released when the entry is removed — including the reference that rides
//! along to the delivery callback.

#[cfg(test)]
extern crate std;

use core::ffi::c_void;
use core::ptr;

/// Matches `WAIT_TIMER_HEAP_MAX` on the non-MCU profiles this selector is
/// allowed on; C reports its effective capacity at runtime.
const HEAP_CAPACITY: usize = 8192;
/// Bounded delivery batch per lock window, matching the C scan.
const SIGNAL_BATCH: usize = 128;

/// No deadline armed.  The C side uses the same sentinel.
const NO_DEADLINE: u64 = u64::MAX;

#[derive(Clone, Copy)]
struct AlarmTimer {
    deadline: u64,
    task: *mut c_void,
}

const EMPTY: AlarmTimer = AlarmTimer {
    deadline: 0,
    task: ptr::null_mut(),
};

static mut HEAP: [AlarmTimer; HEAP_CAPACITY] = [EMPTY; HEAP_CAPACITY];
static mut COUNT: usize = 0;

unsafe extern "C" {
    fn a20_alarm_lock() -> u64;
    fn a20_alarm_unlock(flags: u64);
    fn a20_alarm_capacity() -> u32;
    fn a20_alarm_task_ref(task: *mut c_void) -> *mut c_void;
    fn a20_alarm_task_put(task: *mut c_void);
    fn a20_alarm_task_index(task: *mut c_void) -> i32;
    fn a20_alarm_task_set_index(task: *mut c_void, index: i32);
    fn a20_alarm_task_set_expire(task: *mut c_void, expire: u64);
    fn a20_alarm_note_deadline(deadline: u64);
    fn a20_alarm_deliver(task: *mut c_void, now: u64);
}

#[inline]
fn capacity() -> usize {
    let reported = unsafe { a20_alarm_capacity() } as usize;
    if reported == 0 || reported > HEAP_CAPACITY {
        HEAP_CAPACITY
    } else {
        reported
    }
}

#[inline]
unsafe fn slot(index: usize) -> *mut AlarmTimer {
    (core::ptr::addr_of_mut!(HEAP) as *mut AlarmTimer).add(index)
}

#[inline]
unsafe fn entry(index: usize) -> AlarmTimer {
    unsafe { ptr::read(slot(index)) }
}

#[inline]
unsafe fn set_entry(index: usize, value: AlarmTimer) {
    unsafe { ptr::write(slot(index), value) };
}

#[inline]
fn count() -> usize {
    unsafe { ptr::read(core::ptr::addr_of!(COUNT)) }
}

#[inline]
fn set_count(value: usize) {
    unsafe { ptr::write(core::ptr::addr_of_mut!(COUNT), value) };
}

#[inline]
fn publish_index(task: *mut c_void, index: usize) {
    unsafe { a20_alarm_task_set_index(task, index as i32) };
}

fn swap(a: usize, b: usize) {
    let left = unsafe { entry(a) };
    let right = unsafe { entry(b) };
    unsafe {
        set_entry(a, right);
        set_entry(b, left);
    }
    publish_index(left.task, b);
    publish_index(right.task, a);
}

fn sift_up(mut index: usize) {
    while index > 0 {
        let parent = (index - 1) / 2;
        if unsafe { entry(parent) }.deadline <= unsafe { entry(index) }.deadline {
            break;
        }
        swap(parent, index);
        index = parent;
    }
}

fn sift_down(mut index: usize) {
    let total = count();
    loop {
        let left = index * 2 + 1;
        let right = left + 1;
        let mut smallest = index;
        if left < total
            && unsafe { entry(left) }.deadline < unsafe { entry(smallest) }.deadline
        {
            smallest = left;
        }
        if right < total
            && unsafe { entry(right) }.deadline < unsafe { entry(smallest) }.deadline
        {
            smallest = right;
        }
        if smallest == index {
            break;
        }
        swap(index, smallest);
        index = smallest;
    }
}

/// Remove one entry, heal the heap and return it.  The caller holds the lock.
fn remove_index(index: usize) -> Option<AlarmTimer> {
    let total = count();
    if index >= total {
        return None;
    }
    let removed = unsafe { entry(index) };
    if !removed.task.is_null() {
        unsafe { a20_alarm_task_set_index(removed.task, -1) };
    }
    let new_total = total - 1;
    set_count(new_total);
    if index != new_total {
        let moved = unsafe { entry(new_total) };
        unsafe { set_entry(index, moved) };
        publish_index(moved.task, index);
        let needs_up = index > 0 && {
            let parent = (index - 1) / 2;
            unsafe { entry(index) }.deadline < unsafe { entry(parent) }.deadline
        };
        if needs_up {
            sift_up(index);
        } else {
            sift_down(index);
        }
    }
    unsafe { set_entry(new_total, EMPTY) };
    Some(removed)
}

/// Remove the entry for `task` if it is the one the task points at.
fn cancel_locked(task: *mut c_void) {
    if task.is_null() {
        return;
    }
    let index = unsafe { a20_alarm_task_index(task) };
    if index < 0 {
        return;
    }
    let index = index as usize;
    if index >= count() || unsafe { entry(index) }.task != task {
        return;
    }
    if let Some(removed) = remove_index(index) {
        if !removed.task.is_null() {
            unsafe { a20_alarm_task_put(removed.task) };
        }
    }
}

/// Arm (or re-arm, or clear) the caller's SIGALRM deadline.
///
/// Re-arming always cancels the previous entry first, so one task owns at most
/// one heap entry.  A zero deadline clears; a task that cannot be referenced
/// also clears rather than tracking a deadline that could never be delivered.
///
/// # Safety
/// `task` must be null or a live task pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_alarm_set(task: *mut c_void, alarm_expire: u64) {
    if task.is_null() {
        return;
    }
    let flags = unsafe { a20_alarm_lock() };
    cancel_locked(task);

    let mut expire = alarm_expire;
    if expire != 0 && count() >= capacity() {
        // Capacity is the PID limit, so a live task can never fill the heap;
        // drop the alarm rather than track a deadline that would never fire.
        expire = 0;
    }
    if expire != 0 {
        let owned = unsafe { a20_alarm_task_ref(task) };
        if !owned.is_null() {
            let index = count();
            set_count(index + 1);
            unsafe { set_entry(index, AlarmTimer { deadline: expire, task: owned }) };
            publish_index(owned, index);
            sift_up(index);
        } else {
            expire = 0;
        }
    }
    unsafe { a20_alarm_task_set_expire(task, expire) };
    unsafe { a20_alarm_unlock(flags) };

    unsafe { a20_alarm_note_deadline(expire) };
}

/// Reset the heap to empty (boot and test re-initialisation).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_alarm_init() {
    set_count(0);
}

/// Cancel the caller's alarm, if any.
///
/// # Safety
/// `task` must be null or a live task pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_alarm_cancel(task: *mut c_void) {
    if task.is_null() {
        return;
    }
    let flags = unsafe { a20_alarm_lock() };
    cancel_locked(task);
    unsafe { a20_alarm_unlock(flags) };
}

/// Expiry scan: pop due entries in bounded batches and deliver each one
/// outside the lock, then publish the new root deadline.
///
/// # Safety
/// `now` must be the current tick count; the delivery callback must tolerate
/// being called with the lock released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_alarm_scan(now: u64) {
    let mut batch = [ptr::null_mut::<c_void>(); SIGNAL_BATCH];
    loop {
        let mut filled = 0usize;
        let mut more_due = false;
        let next_alarm;

        let flags = unsafe { a20_alarm_lock() };
        while count() > 0 {
            if unsafe { entry(0) }.deadline > now {
                break;
            }
            if filled >= SIGNAL_BATCH {
                more_due = true;
                break;
            }
            match remove_index(0) {
                Some(expired) => {
                    // The heap reference rides along until delivery.
                    batch[filled] = expired.task;
                    filled += 1;
                }
                None => break,
            }
        }
        next_alarm = if count() > 0 {
            unsafe { entry(0) }.deadline
        } else {
            NO_DEADLINE
        };
        unsafe { a20_alarm_unlock(flags) };

        for entry in batch.iter().take(filled) {
            unsafe { a20_alarm_deliver(*entry, now) };
        }

        if !more_due {
            unsafe { a20_alarm_note_deadline(next_alarm) };
            break;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::thread_local;
    use std::vec::Vec;
    use std::boxed::Box;

    #[derive(Clone, Copy)]
    struct FakeTask {
        index: i32,
        expire: u64,
        refs: i32,
        id: i32,
    }

    thread_local! {
        static TASKS: RefCell<Vec<Box<FakeTask>>> = RefCell::new(Vec::new());
        static DELIVERED: RefCell<Vec<i32>> = const { RefCell::new(Vec::new()) };
        static NOTES: RefCell<Vec<u64>> = const { RefCell::new(Vec::new()) };
        static CAPACITY: std::cell::Cell<u32> = const { std::cell::Cell::new(HEAP_CAPACITY as u32) };
    static WINDOWS: std::cell::Cell<u32> = const { std::cell::Cell::new(0) };
    }

    /// The tests drive the one static heap, so they must not run concurrently.
    static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    fn reset() -> std::sync::MutexGuard<'static, ()> {
        let guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        set_count(0);
        for i in 0..HEAP_CAPACITY {
            unsafe { set_entry(i, EMPTY) };
        }
        TASKS.with(|t| t.borrow_mut().clear());
        DELIVERED.with(|d| d.borrow_mut().clear());
        NOTES.with(|n| n.borrow_mut().clear());
        CAPACITY.with(|c| c.set(HEAP_CAPACITY as u32));
        guard
    }

    fn task(id: i32) -> *mut c_void {
        TASKS.with(|t| {
            let mut tasks = t.borrow_mut();
            tasks.push(std::boxed::Box::new(FakeTask { index: -1, expire: 0, refs: 1, id }));
            let ptr = tasks.last_mut().unwrap().as_mut() as *mut FakeTask;
            ptr as *mut c_void
        })
    }

    fn fake(task: *mut c_void) -> &'static mut FakeTask {
        unsafe { &mut *(task as *mut FakeTask) }
    }

    #[unsafe(no_mangle)]
    extern "C" fn a20_alarm_lock() -> u64 {
        WINDOWS.with(|w| w.set(w.get() + 1));
        0
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_alarm_unlock(_flags: u64) {}
    #[unsafe(no_mangle)]
    extern "C" fn a20_alarm_capacity() -> u32 {
        CAPACITY.with(|c| c.get())
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_alarm_task_ref(task: *mut c_void) -> *mut c_void {
        fake(task).refs += 1;
        task
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_alarm_task_put(task: *mut c_void) {
        fake(task).refs -= 1;
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_alarm_task_index(task: *mut c_void) -> i32 {
        fake(task).index
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_alarm_task_set_index(task: *mut c_void, index: i32) {
        fake(task).index = index;
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_alarm_task_set_expire(task: *mut c_void, expire: u64) {
        fake(task).expire = expire;
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_alarm_note_deadline(deadline: u64) {
        if deadline != 0 {
            NOTES.with(|n| n.borrow_mut().push(deadline));
        }
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_alarm_deliver(task: *mut c_void, _now: u64) {
        let f = fake(task);
        DELIVERED.with(|d| d.borrow_mut().push(f.id));
        f.refs -= 1; // the riding reference is released after delivery
    }

    fn deadlines() -> Vec<u64> {
        (0..count()).map(|i| unsafe { entry(i) }.deadline).collect()
    }

    #[test]
    fn arming_orders_entries_by_deadline_and_publishes_the_index() {
        let _guard = reset();
        let a = task(1);
        let b = task(2);
        let c = task(3);
        unsafe { a20_alarm_set(a, 300) };
        unsafe { a20_alarm_set(b, 100) };
        unsafe { a20_alarm_set(c, 200) };
        assert_eq!(count(), 3);
        assert_eq!(unsafe { entry(0) }.deadline, 100, "min-heap root");
        assert_eq!(deadlines(), [100, 300, 200]);
        assert_eq!(fake(a).index, 1);
        assert_eq!(fake(b).index, 0);
        assert_eq!(fake(c).index, 2);
        assert_eq!(fake(a).refs, 2, "the heap holds one reference");
        assert_eq!(fake(a).expire, 300);
    }

    #[test]
    fn re_arming_replaces_the_previous_entry_and_its_reference() {
        let _guard = reset();
        let a = task(1);
        unsafe { a20_alarm_set(a, 300) };
        unsafe { a20_alarm_set(a, 50) };
        assert_eq!(count(), 1, "one task owns at most one entry");
        assert_eq!(unsafe { entry(0) }.deadline, 50);
        assert_eq!(fake(a).refs, 2);
        assert_eq!(fake(a).expire, 50);
    }

    #[test]
    fn clearing_releases_the_entry_and_zeroes_the_published_deadline() {
        let _guard = reset();
        let a = task(1);
        unsafe { a20_alarm_set(a, 300) };
        unsafe { a20_alarm_set(a, 0) };
        assert_eq!(count(), 0);
        assert_eq!(fake(a).refs, 1, "heap reference released");
        assert_eq!(fake(a).expire, 0);
        assert_eq!(fake(a).index, -1);
    }

    #[test]
    fn cancel_removes_only_the_tasks_own_entry() {
        let _guard = reset();
        let a = task(1);
        let b = task(2);
        unsafe { a20_alarm_set(a, 300) };
        unsafe { a20_alarm_set(b, 100) };
        unsafe { a20_alarm_cancel(a) };
        assert_eq!(count(), 1);
        assert_eq!(unsafe { entry(0) }.task, b);
        assert_eq!(fake(a).refs, 1);
        assert_eq!(fake(b).refs, 2);
    }

    #[test]
    fn a_full_heap_drops_the_alarm_instead_of_tracking_it() {
        let _guard = reset();
        CAPACITY.with(|c| c.set(2));
        let a = task(1);
        let b = task(2);
        let c = task(3);
        unsafe { a20_alarm_set(a, 10) };
        unsafe { a20_alarm_set(b, 20) };
        unsafe { a20_alarm_set(c, 30) };
        assert_eq!(count(), 2);
        assert_eq!(fake(c).expire, 0, "the arm was refused");
        assert_eq!(fake(c).refs, 1, "no reference was taken");
    }

    #[test]
    fn the_scan_pops_due_entries_and_delivers_them_in_deadline_order() {
        let _guard = reset();
        let a = task(1);
        let b = task(2);
        let c = task(3);
        unsafe { a20_alarm_set(a, 30) };
        unsafe { a20_alarm_set(b, 10) };
        unsafe { a20_alarm_set(c, 20) };
        NOTES.with(|n| n.borrow_mut().clear());
        unsafe { a20_alarm_scan(25) };
        assert_eq!(DELIVERED.with(|d| d.borrow().clone()), [2, 3]);
        assert_eq!(count(), 1, "the future entry stays queued");
        assert_eq!(unsafe { entry(0) }.task, a);
        assert_eq!(fake(b).refs, 1, "delivery released the riding reference");
        assert_eq!(NOTES.with(|n| n.borrow().clone()), [30], "new root published");
    }

    #[test]
    fn the_scan_publishes_no_deadline_when_the_heap_empties() {
        let _guard = reset();
        let a = task(1);
        unsafe { a20_alarm_set(a, 10) };
        NOTES.with(|n| n.borrow_mut().clear());
        unsafe { a20_alarm_scan(50) };
        assert_eq!(count(), 0);
        assert_eq!(DELIVERED.with(|d| d.borrow().clone()), [1]);
        // The scan notes the sentinel; the bridge ignores zero but the sentinel
        // is what the scheduler uses.
        assert_eq!(NOTES.with(|n| n.borrow().clone()), [NO_DEADLINE]);
    }

    #[test]
    fn the_scan_drains_every_due_entry_across_bounded_lock_windows() {
        let _guard = reset();
        for i in 0..(SIGNAL_BATCH + 5) {
            let t = task(i as i32);
            unsafe { a20_alarm_set(t, 10) };
        }
        WINDOWS.with(|w| w.set(0));
        unsafe { a20_alarm_scan(20) };
        // One scan call delivers everything due; the batch size only bounds
        // how long each lock window is held, so the 133 due entries need two
        // windows (128 + 5).
        assert_eq!(DELIVERED.with(|d| d.borrow().len()), SIGNAL_BATCH + 5);
        assert_eq!(count(), 0);
        assert_eq!(WINDOWS.with(|w| w.get()), 2);
    }

    #[test]
    fn the_scan_leaves_future_entries_alone() {
        let _guard = reset();
        let a = task(1);
        unsafe { a20_alarm_set(a, 100) };
        NOTES.with(|n| n.borrow_mut().clear());
        unsafe { a20_alarm_scan(10) };
        assert!(DELIVERED.with(|d| d.borrow().is_empty()));
        assert_eq!(count(), 1);
        assert_eq!(NOTES.with(|n| n.borrow().clone()), [100]);
    }

    #[test]
    fn removing_an_interior_entry_heals_the_heap() {
        let _guard = reset();
        let a = task(1);
        let b = task(2);
        let c = task(3);
        let d = task(4);
        unsafe { a20_alarm_set(a, 10) };
        unsafe { a20_alarm_set(b, 20) };
        unsafe { a20_alarm_set(c, 30) };
        unsafe { a20_alarm_set(d, 40) };
        unsafe { a20_alarm_cancel(b) };
        assert_eq!(count(), 3);
        // Every parent must be <= its children after the removal.
        for i in 1..count() {
            let parent = (i - 1) / 2;
            assert!(unsafe { entry(parent) }.deadline <= unsafe { entry(i) }.deadline);
        }
        // And each remaining task's published index points at its entry.
        for i in 0..count() {
            let t = unsafe { entry(i) }.task;
            assert_eq!(fake(t).index, i as i32);
        }
    }
}
