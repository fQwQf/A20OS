#![no_std]

//! Completion counter and blocking-wait ownership.
//!
//! C owns the ABI/layout, IRQ-saving spin lock and wait-queue primitives.
//! This module owns the persistent completion count and the wait-token
//! prepare/link/commit/cancel/unlink/finish sequence.

use core::ffi::c_void;

const PROC_WAIT_UNINTERRUPTIBLE: i32 = 0;
const COMPLETION_DONE_ALL: u32 = u32::MAX;

#[repr(C)]
#[derive(Clone, Copy)]
struct ProcWaitToken {
    task: *mut c_void,
    seq: u64,
    prepare_error: i32,
}

#[repr(C)]
struct WaitQueueEntry {
    next: *mut WaitQueueEntry,
    prev: *mut WaitQueueEntry,
    queue: *mut c_void,
    task: *mut c_void,
    wait_seq: u64,
    flags: u32,
    key: usize,
    priv_data: *mut c_void,
    linked: bool,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 24] = [(); core::mem::size_of::<ProcWaitToken>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 24] = [(); core::mem::size_of::<ProcWaitToken>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 72] = [(); core::mem::size_of::<WaitQueueEntry>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 40] = [(); core::mem::size_of::<WaitQueueEntry>()];

extern "C" {
    fn a20_completion_lock(completion: *mut c_void) -> u64;
    fn a20_completion_unlock(completion: *mut c_void, flags: u64);
    fn a20_completion_done_ptr(completion: *mut c_void) -> *mut u32;
    fn a20_completion_waiters(completion: *mut c_void) -> *mut c_void;
    fn proc_park_prepare(mode: i32, deadline: u64) -> ProcWaitToken;
    fn proc_park_cancel(token: ProcWaitToken) -> i32;
    fn proc_park_commit(token: ProcWaitToken) -> i32;
    fn proc_park_finish(token: ProcWaitToken);
    fn proc_yield();
    fn wait_queue_link(
        queue: *mut c_void,
        entry: *mut WaitQueueEntry,
        token: ProcWaitToken,
        key: usize,
    ) -> bool;
    fn wait_queue_unlink(queue: *mut c_void, entry: *mut WaitQueueEntry);
}

unsafe fn done_ptr(completion: *mut c_void) -> *mut u32 {
    unsafe { a20_completion_done_ptr(completion) }
}

#[no_mangle]
/// # Safety
/// `done` points to the completion counter protected by its C lock.
pub unsafe extern "C" fn a20_completion_state_init_rust(done: *mut u32) {
    if let Some(done) = unsafe { done.as_mut() } {
        *done = 0;
    }
}

#[no_mangle]
/// # Safety
/// `done` points to the completion counter and its lock is held.
pub unsafe extern "C" fn a20_completion_state_add_rust(done: *mut u32) {
    if let Some(done) = unsafe { done.as_mut() } {
        *done = done.wrapping_add(1);
    }
}

#[no_mangle]
/// # Safety
/// `done` points to the completion counter and its lock is held.
pub unsafe extern "C" fn a20_completion_state_all_rust(done: *mut u32) {
    if let Some(done) = unsafe { done.as_mut() } {
        *done = COMPLETION_DONE_ALL;
    }
}

unsafe fn consume_done(done: *mut u32) -> bool {
    let Some(done) = (unsafe { done.as_mut() }) else {
        return false;
    };
    if *done == 0 {
        return false;
    }
    if *done != COMPLETION_DONE_ALL {
        *done -= 1;
    }
    true
}

#[no_mangle]
/// # Safety
/// `completion` is a live `completion_t`; its lock serializes the counter and
/// the wait-queue link/recheck protocol.
pub unsafe extern "C" fn a20_completion_wait_rust(completion: *mut c_void) {
    if completion.is_null() {
        return;
    }
    let done = unsafe { done_ptr(completion) };
    let waiters = unsafe { a20_completion_waiters(completion) };
    if done.is_null() || waiters.is_null() {
        return;
    }

    loop {
        let flags = unsafe { a20_completion_lock(completion) };
        let ready = unsafe { consume_done(done) };
        unsafe { a20_completion_unlock(completion, flags) };
        if ready {
            return;
        }

        let token = unsafe { proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, 0) };
        if token.task.is_null() {
            unsafe { proc_yield() };
            continue;
        }
        let mut entry = WaitQueueEntry {
            next: core::ptr::null_mut(),
            prev: core::ptr::null_mut(),
            queue: core::ptr::null_mut(),
            task: core::ptr::null_mut(),
            wait_seq: 0,
            flags: 0,
            key: 0,
            priv_data: core::ptr::null_mut(),
            linked: false,
        };

        let flags = unsafe { a20_completion_lock(completion) };
        if unsafe { consume_done(done) } {
            unsafe { a20_completion_unlock(completion, flags) };
            unsafe {
                proc_park_cancel(token);
                proc_park_finish(token);
            }
            return;
        }
        let linked = unsafe { wait_queue_link(waiters, &mut entry, token, 0) };
        unsafe { a20_completion_unlock(completion, flags) };

        if linked {
            unsafe { proc_park_commit(token) };
        } else {
            unsafe { proc_park_cancel(token) };
        }
        unsafe {
            wait_queue_unlink(waiters, &mut entry);
            proc_park_finish(token);
        }
    }
}
