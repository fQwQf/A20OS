#![no_std]

//! Sleeping mutex ownership and recursion state.
//!
//! The C shim retains IRQ-save locking, task parking, and wake queues. It calls
//! these transitions only while `mutex_t::lock` is held.

use core::ffi::c_void;

#[repr(C)]
pub struct MutexState {
    pub locked: i32,
    pub depth: i32,
    pub owner: *mut c_void,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 16] = [(); core::mem::size_of::<MutexState>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 12] = [(); core::mem::size_of::<MutexState>()];

/// # Safety
/// `state` must be valid and exclusively protected by the mutex spin lock.
#[no_mangle]
pub unsafe extern "C" fn a20_mutex_state_init_rust(state: *mut MutexState) {
    if let Some(state) = unsafe { state.as_mut() } {
        state.locked = 0;
        state.depth = 0;
        state.owner = core::ptr::null_mut();
    }
}

/// Try the unlocked-to-owned transition. Returns one on admission.
///
/// # Safety
/// `state` must be valid and exclusively protected by the mutex spin lock.
#[no_mangle]
pub unsafe extern "C" fn a20_mutex_try_acquire_rust(
    state: *mut MutexState,
    owner: *mut c_void,
) -> i32 {
    let Some(state) = (unsafe { state.as_mut() }) else { return 0 };
    if state.locked != 0 {
        return 0;
    }
    state.locked = 1;
    state.depth = 1;
    state.owner = owner;
    1
}

/// Handle the kernel's existing bounded-use recursive-acquisition workaround.
/// Returns one for recursion, zero for a different owner, and -75 if the depth
/// cannot be incremented without signed overflow.
///
/// # Safety
/// `state` must be valid and exclusively protected by the mutex spin lock.
#[no_mangle]
pub unsafe extern "C" fn a20_mutex_reenter_rust(
    state: *mut MutexState,
    owner: *mut c_void,
) -> i32 {
    let Some(state) = (unsafe { state.as_mut() }) else { return 0 };
    if owner.is_null() || state.locked == 0 || state.owner != owner {
        return 0;
    }
    let Some(depth) = state.depth.checked_add(1) else { return -75 };
    state.depth = depth;
    1
}

/// Release ownership; returns one when the mutex became available.
///
/// # Safety
/// `state` must be valid and exclusively protected by the mutex spin lock.
#[no_mangle]
pub unsafe extern "C" fn a20_mutex_release_rust(state: *mut MutexState) -> i32 {
    let Some(state) = (unsafe { state.as_mut() }) else { return 0 };
    if state.depth > 1 {
        state.depth -= 1;
        return 0;
    }
    state.locked = 0;
    state.depth = 0;
    state.owner = core::ptr::null_mut();
    1
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn admission_recursion_and_final_release() {
        let mut state = MutexState { locked: 0, depth: 0, owner: core::ptr::null_mut() };
        let first = 1usize as *mut c_void;
        let second = 2usize as *mut c_void;
        unsafe { a20_mutex_state_init_rust(&mut state) };
        assert_eq!(unsafe { a20_mutex_try_acquire_rust(&mut state, first) }, 1);
        assert_eq!(unsafe { a20_mutex_try_acquire_rust(&mut state, second) }, 0);
        assert_eq!(unsafe { a20_mutex_reenter_rust(&mut state, second) }, 0);
        assert_eq!(unsafe { a20_mutex_reenter_rust(&mut state, first) }, 1);
        assert_eq!(state.depth, 2);
        assert_eq!(unsafe { a20_mutex_release_rust(&mut state) }, 0);
        assert_eq!(state.locked, 1);
        assert_eq!(unsafe { a20_mutex_release_rust(&mut state) }, 1);
        assert_eq!(state.locked, 0);
        assert!(state.owner.is_null());
    }

    #[test]
    fn recursion_depth_overflow_does_not_corrupt_owner() {
        let owner = 7usize as *mut c_void;
        let mut state = MutexState { locked: 1, depth: i32::MAX, owner };
        assert_eq!(unsafe { a20_mutex_reenter_rust(&mut state, owner) }, -75);
        assert_eq!(state.depth, i32::MAX);
        assert_eq!(state.owner, owner);
    }
}
