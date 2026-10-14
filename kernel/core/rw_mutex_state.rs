#![no_std]

//! Reader/writer admission state for the kernel sleepable rw mutex.
//!
//! The C shim owns the IRQ-saving spin lock and wait queues. Every exported
//! operation below runs while that lock is held, so reader/writer counters,
//! writer preference, and owner publication form one serialized transition.

use core::ffi::c_void;

#[repr(C)]
pub struct RwMutexState {
    pub readers: u32,
    pub waiting_writers: u32,
    pub writer: i32,
    pub owner: *mut c_void,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 24] = [(); core::mem::size_of::<RwMutexState>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 16] = [(); core::mem::size_of::<RwMutexState>()];

#[no_mangle]
/// # Safety
/// `state` must point to the state fields protected by the caller's rw mutex.
pub unsafe extern "C" fn a20_rw_mutex_state_init_rust(state: *mut RwMutexState) {
    if let Some(state) = state.as_mut() {
        state.readers = 0;
        state.waiting_writers = 0;
        state.writer = 0;
        state.owner = core::ptr::null_mut();
    }
}

#[no_mangle]
/// # Safety
/// `state` must be exclusively protected by its rw mutex spin lock.
pub unsafe extern "C" fn a20_rw_mutex_read_admit_rust(
    state: *mut RwMutexState,
) -> i32 {
    let Some(state) = state.as_mut() else { return 0 };
    if state.writer != 0 || state.waiting_writers != 0 {
        return 0;
    }
    state.readers = state.readers.wrapping_add(1);
    1
}

#[no_mangle]
/// # Safety
/// `state` must be exclusively protected by its rw mutex spin lock.
pub unsafe extern "C" fn a20_rw_mutex_read_release_rust(
    state: *mut RwMutexState,
) -> i32 {
    let Some(state) = state.as_mut() else { return 0 };
    if state.readers != 0 {
        state.readers -= 1;
    }
    i32::from(state.readers == 0 && state.waiting_writers != 0)
}

#[no_mangle]
/// # Safety
/// `state` must be exclusively protected by its rw mutex spin lock.
pub unsafe extern "C" fn a20_rw_mutex_writer_wait_rust(
    state: *mut RwMutexState,
) {
    if let Some(state) = state.as_mut() {
        state.waiting_writers = state.waiting_writers.wrapping_add(1);
    }
}

#[no_mangle]
/// # Safety
/// `state` must be exclusively protected by its rw mutex spin lock. `owner`
/// is the current task identity (or null for an early boot caller).
pub unsafe extern "C" fn a20_rw_mutex_write_admit_rust(
    state: *mut RwMutexState,
    owner: *mut c_void,
) -> i32 {
    let Some(state) = state.as_mut() else { return 0 };
    if state.writer != 0 || state.readers != 0 {
        return 0;
    }
    state.waiting_writers = state.waiting_writers.wrapping_sub(1);
    state.writer = 1;
    state.owner = owner;
    1
}

#[no_mangle]
/// # Safety
/// `state` must be exclusively protected by its rw mutex spin lock.
pub unsafe extern "C" fn a20_rw_mutex_write_release_rust(
    state: *mut RwMutexState,
) -> i32 {
    let Some(state) = state.as_mut() else { return 0 };
    state.writer = 0;
    state.owner = core::ptr::null_mut();
    i32::from(state.waiting_writers != 0)
}
