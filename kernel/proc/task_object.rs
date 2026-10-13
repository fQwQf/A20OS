#![no_std]

//! Native TASK/THREAD/DEBUG identity-object lifetime.
//!
//! The task pointer is a weak binding. The per-task object slot and the
//! destroy transition are serialized by the C task-object lock; a successful
//! `proc_get` performed while that lock is held converts the weak pointer to
//! a strong task reference. Handle-table, scheduler and task layouts remain
//! owned by their existing C subsystems.

use core::ffi::c_void;
use core::mem::{align_of, offset_of, size_of};
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

const A20_ERR_BAD_HANDLE: i64 = 5;
const A20_ERR_NO_MEMORY: i64 = 6;
const A20_ERR_NO_SPACE: i64 = 13;

const A20_OBJ_TASK: u16 = 1;
const A20_OBJ_THREAD: u16 = 2;
const A20_OBJ_DEBUG: u16 = 13;

#[repr(C)]
struct TaskObject {
    refs: AtomicI32,
    task: *mut c_void,
    serial: u64,
}

// Keep the object ABI checked in Rust as well as by the C adapter. The 32-bit
// layout is retained for future matching-ABI targets; current Rust kernel
// selection intentionally excludes RV32 because its C ABI is ILP32D.
const _: () = {
    assert!(size_of::<AtomicI32>() == 4);
    assert!(align_of::<AtomicI32>() == 4);
    assert!(offset_of!(TaskObject, refs) == 0);
    assert!(offset_of!(TaskObject, task) == if usize::BITS == 64 { 8 } else { 4 });
    assert!(offset_of!(TaskObject, serial) == if usize::BITS == 64 { 16 } else { 8 });
    assert!(size_of::<TaskObject>() == if usize::BITS == 64 { 24 } else { 16 });
};

unsafe extern "C" {
    fn kcalloc(count: usize, size: usize) -> *mut c_void;
    fn kfree(object: *mut c_void);
    fn a20_task_object_lock() -> u64;
    fn a20_task_object_unlock(flags: u64);
    fn a20_task_object_slot_get(task: *mut c_void) -> *mut c_void;
    fn a20_task_object_slot_set(task: *mut c_void, object: *mut c_void);
    fn a20_task_object_destroy_started(task: *mut c_void) -> i32;
    fn a20_task_object_try_pin_task(task: *mut c_void) -> *mut c_void;
    fn a20_handle_install(
        table: *mut c_void,
        object: *mut c_void,
        object_type: u16,
        rights: u64,
    ) -> i64;
    fn a20_event_notify(
        object: *mut c_void,
        object_type: u16,
        event: u32,
        data0: u64,
        data1: u64,
    );
    fn a20_eventq_on_object_destroy(object: *mut c_void, object_type: u16);
}

static mut NEXT_SERIAL: u64 = 0;

#[inline]
unsafe fn object_ref(object: *mut c_void) {
    if !object.is_null() {
        // Match the existing refcount_inc() relaxed fetch-add behavior,
        // including its wrapping semantics on overflow.
        unsafe { (*object.cast::<TaskObject>()).refs.fetch_add(1, Ordering::Relaxed) };
    }
}

#[inline]
unsafe fn object_put(object: *mut c_void) {
    if object.is_null() {
        return;
    }
    let previous = unsafe {
        (*object.cast::<TaskObject>())
            .refs
            .fetch_sub(1, Ordering::AcqRel)
    };
    if previous == 1 {
        // Callbacks and free can acquire unrelated locks or re-enter object
        // code. All callers release task_object_lock before reaching here.
        unsafe {
            a20_eventq_on_object_destroy(object, A20_OBJ_TASK);
            a20_eventq_on_object_destroy(object, A20_OBJ_THREAD);
            a20_eventq_on_object_destroy(object, A20_OBJ_DEBUG);
            kfree(object);
        }
    }
}

/// Read the immutable identity serial.
///
/// # Safety
/// `object` must be null or point to a live identity held by the caller.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_rust_task_object_serial(object: *mut c_void) -> u64 {
    if object.is_null() {
        0
    } else {
        // Serial is immutable after publication and the caller owns a ref.
        unsafe { (*object.cast::<TaskObject>()).serial }
    }
}

/// Acquire an identity reference.
///
/// # Safety
/// `object` must be null or point to a live identity. A non-null pointer must
/// already be protected by an owned reference or by task_object_lock.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_rust_task_object_ref(object: *mut c_void) {
    unsafe { object_ref(object) }
}

/// Release one identity reference.
///
/// # Safety
/// `object` must be null or point to a live identity for which the caller
/// owns exactly one reference.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_rust_task_object_put(object: *mut c_void) {
    unsafe { object_put(object) }
}

/// Resolve the weak task binding while the publication/detach lock is held.
///
/// # Safety
/// `object` must be null or point to a live task identity object held by the
/// caller. The task-object lock must remain held from reading the weak pointer
/// through this call so the task cannot be reclaimed before `proc_get`. The
/// returned task pointer owns a strong reference and must be released by
/// `proc_put` after leaving this critical section.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_rust_task_object_get_task(
    object: *mut c_void,
) -> *mut c_void {
    if object.is_null() {
        return ptr::null_mut();
    }
    unsafe {
        let flags = a20_task_object_lock();
        let task = (*object.cast::<TaskObject>()).task;
        let pinned = a20_task_object_try_pin_task(task);
        a20_task_object_unlock(flags);
        pinned
    }
}

/// Detach a task's weak binding before its storage can be reclaimed.
///
/// # Safety
/// `task` must be a live task whose destroy transition is in progress. The
/// caller must not hold a lock that is acquired by task-object callbacks.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_rust_task_object_detach(task: *mut c_void) {
    if task.is_null() {
        return;
    }
    unsafe {
        let flags = a20_task_object_lock();
        let object = a20_task_object_slot_get(task);
        if !object.is_null() {
            (*object.cast::<TaskObject>()).task = ptr::null_mut();
        }
        a20_task_object_unlock(flags);
    }
}

/// Drop the identity reference owned by the final task release.
///
/// # Safety
/// `task` must be in its final task-release path after its weak identity
/// binding has been detached. No concurrent code may access its task-object
/// slot after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_rust_task_object_task_release(task: *mut c_void) {
    if !task.is_null() {
        unsafe { object_put(a20_task_object_slot_get(task)) };
    }
}

/// Publish or reuse a task identity and install its handle reference.
///
/// # Safety
/// `table` and `task` must be valid kernel objects; the caller must own a
/// strong task reference for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_rust_handle_install_task(
    table: *mut c_void,
    task: *mut c_void,
    object_type: u16,
    rights: u64,
) -> i64 {
    if task.is_null()
        || !(object_type == A20_OBJ_TASK
            || object_type == A20_OBJ_THREAD
            || object_type == A20_OBJ_DEBUG)
    {
        return -A20_ERR_BAD_HANDLE;
    }

    let mut candidate: *mut c_void = ptr::null_mut();
    let object: *mut c_void;
    loop {
        let flags = unsafe { a20_task_object_lock() };
        let mut published = unsafe { a20_task_object_slot_get(task) };
        if unsafe { a20_task_object_destroy_started(task) } != 0 {
            unsafe { a20_task_object_unlock(flags) };
            unsafe { object_put(candidate) };
            return -A20_ERR_BAD_HANDLE;
        }
        if published.is_null() && !candidate.is_null() {
            let serial = unsafe { ptr::addr_of_mut!(NEXT_SERIAL).read() };
            if serial == u64::MAX {
                unsafe { a20_task_object_unlock(flags) };
                unsafe { object_put(candidate) };
                return -A20_ERR_NO_SPACE;
            }
            let next = serial + 1;
            unsafe {
                ptr::addr_of_mut!(NEXT_SERIAL).write(next);
                let identity = candidate.cast::<TaskObject>();
                (*identity).serial = next;
                (*identity).task = task;
                a20_task_object_slot_set(task, candidate);
            }
            published = candidate;
            candidate = ptr::null_mut();
        }
        if !published.is_null() {
            unsafe { object_ref(published) };
        }
        unsafe { a20_task_object_unlock(flags) };
        if !published.is_null() {
            object = published;
            break;
        }

        candidate = unsafe { kcalloc(1, size_of::<TaskObject>()) };
        if candidate.is_null() {
            return -A20_ERR_NO_MEMORY;
        }
        unsafe {
            ptr::write(
                candidate.cast::<TaskObject>(),
                TaskObject {
                    refs: AtomicI32::new(1),
                    task: ptr::null_mut(),
                    serial: 0,
                },
            );
        }
    }

    unsafe { object_put(candidate) };
    let result = unsafe { a20_handle_install(table, object, object_type, rights) };
    if result < 0 {
        unsafe { object_put(object) };
    }
    result
}

/// Deliver task events through the stable identity object, outside its lock.
///
/// # Safety
/// The caller must hold a strong task reference through the callbacks. Event
/// callbacks may re-enter task-object APIs, so this entry must not be called
/// while holding task_object_lock.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_rust_task_object_notify(
    task: *mut c_void,
    event: u32,
    data0: u64,
    data1: u64,
    closing: i32,
) {
    if task.is_null() {
        return;
    }
    let flags = unsafe { a20_task_object_lock() };
    let object = unsafe { a20_task_object_slot_get(task) };
    unsafe { object_ref(object) };
    unsafe { a20_task_object_unlock(flags) };
    if object.is_null() {
        return;
    }

    for object_type in [A20_OBJ_TASK, A20_OBJ_THREAD, A20_OBJ_DEBUG] {
        unsafe { a20_event_notify(object, object_type, event, data0, data1) };
        if closing != 0 {
            unsafe { a20_eventq_on_object_destroy(object, object_type) };
        }
    }
    unsafe { object_put(object) };
}
