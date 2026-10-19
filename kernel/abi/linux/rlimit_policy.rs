#![no_std]

//! POSIX resource-limit policy.
//!
//! Rust owns which task field each `RLIMIT_*` resource maps to, how a requested
//! limit is clamped, which resources have no enforcement, and the privilege
//! rules for changing another task's limits and for raising `RLIMIT_RTPRIO`.
//! C keeps the usercopy of the two-value pairs, the signal-owned core limit,
//! the park lock around the RT pair and the task lookup.
//!
//! An unenforced resource reports `RLIM_INFINITY`: that is the truthful answer,
//! not a claim that a limit exists and happens to be large.  Installing a limit
//! on such a resource is refused rather than reported as success.

#[cfg(test)]
extern crate std;

pub const RLIMIT_STACK: i32 = 3;
pub const RLIMIT_CORE: i32 = 4;
pub const RLIMIT_NPROC: i32 = 6;
pub const RLIMIT_NOFILE: i32 = 7;
pub const RLIMIT_AS: i32 = 9;
pub const RLIMIT_RTPRIO: i32 = 14;

pub const USER_STACK_MAX_SIZE: u64 = 8 * 1024 * 1024;
pub const MAX_FILES: u64 = 4096;

const CAP_SYS_RESOURCE: i32 = 24;

const ESRCH: i32 = -3;
const EPERM: i32 = -1;
const EINVAL: i32 = -22;
const ENOSYS: i32 = -38;

/// Borrowed view over the live fields one task's limits live in.
#[repr(C)]
pub struct LimitsView {
    pub stack: *mut u64,
    pub nofile: *mut u64,
    pub aspace: *mut u64,
    pub nproc: *mut u64,
    pub rtprio: *mut u64,
    pub rtprio_max: *mut u64,
    pub uid: *const i32,
}

const _: () = {
    assert!(core::mem::size_of::<LimitsView>() == 7 * core::mem::size_of::<usize>());
};

extern "C" {
    fn a20_rl_park_lock(t: *mut core::ffi::c_void) -> u64;
    fn a20_rl_park_unlock(t: *mut core::ffi::c_void, flags: u64);
    fn a20_rl_has_cap(t: *mut core::ffi::c_void, cap: i32) -> i32;
    fn a20_rl_core_set(t: *mut core::ffi::c_void, limit: u64);
    fn a20_rl_current_uid() -> i32;
}

/// Report one limit as a `(cur, max)` pair.  `core` is pre-read by the adapter
/// because that limit is shared with the signal machinery.
///
/// # Safety
/// `pair` points at two writable `u64`s. When `view` is non-null it borrows the
/// live limit fields of `t`, which must outlive the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_rlimit_get(t: *mut core::ffi::c_void, view: *const LimitsView,
                                        resource: i32, core: u64, pair: *mut u64) -> i32 {
    let paired = unsafe { &mut *(pair as *mut [u64; 2]) };
    if view.is_null() {
        // No task: report the defaults the C reference used.
        let value = match resource {
            RLIMIT_STACK => USER_STACK_MAX_SIZE,
            RLIMIT_CORE => core,
            RLIMIT_NOFILE => MAX_FILES,
            RLIMIT_AS | RLIMIT_NPROC => 0,
            RLIMIT_RTPRIO => return ESRCH,
            _ => return ENOSYS,
        };
        *paired = [value, value];
        return 0;
    }
    let view = unsafe { &*view };
    if resource == RLIMIT_RTPRIO {
        let flags = unsafe { a20_rl_park_lock(t) };
        let values = [unsafe { *view.rtprio }, unsafe { *view.rtprio_max }];
        unsafe { a20_rl_park_unlock(t, flags) };
        *paired = values;
        return 0;
    }
    let value = match resource {
        RLIMIT_STACK => unsafe { *view.stack },
        RLIMIT_CORE => core,
        RLIMIT_NOFILE => unsafe { *view.nofile },
        RLIMIT_AS => unsafe { *view.aspace },
        RLIMIT_NPROC => unsafe { *view.nproc },
        _ => return ENOSYS,
    };
    *paired = [value, value];
    0
}

/// Install one limit.
///
/// # Safety
/// Same contract as `a20_rlimit_get`; `t` must be the task `view` borrows.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_rlimit_set(t: *mut core::ffi::c_void, view: *const LimitsView,
                                        resource: i32, cur: u64, max: u64) -> i32 {
    if view.is_null() {
        return ESRCH;
    }
    let view = unsafe { &*view };
    if resource == RLIMIT_RTPRIO {
        if cur > max {
            return EINVAL;
        }
        let flags = unsafe { a20_rl_park_lock(t) };
        if max > unsafe { *view.rtprio_max }
            && unsafe { a20_rl_has_cap(unsafe { a20_rl_current() }, CAP_SYS_RESOURCE) } == 0
        {
            unsafe { a20_rl_park_unlock(t, flags) };
            return EPERM;
        }
        unsafe {
            *view.rtprio = cur;
            *view.rtprio_max = max;
        }
        unsafe { a20_rl_park_unlock(t, flags) };
        return 0;
    }
    match resource {
        RLIMIT_STACK => unsafe {
            *view.stack = clamp(cur, max, USER_STACK_MAX_SIZE)
        },
        RLIMIT_CORE => unsafe { a20_rl_core_set(t, cur) },
        RLIMIT_NOFILE => unsafe {
            *view.nofile = clamp(cur, max, MAX_FILES)
        },
        RLIMIT_AS => unsafe { *view.aspace = cur },
        RLIMIT_NPROC => unsafe { *view.nproc = cur },
        _ => return EINVAL,
    }
    0
}

fn clamp(cur: u64, max: u64, ceiling: u64) -> u64 {
    let limit = if cur < max { cur } else { max };
    if limit > ceiling { ceiling } else { limit }
}

/// May the caller change `view`'s limits?  Changing another task's limits is a
/// privilege operation unless it is the caller's own uid.
///
/// # Safety
/// `view` must be a valid view for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_prlimit_may_change(view: *const LimitsView, is_self: i32,
                                                has_cap: i32) -> i32 {
    if is_self != 0 || has_cap != 0 || view.is_null() {
        return 0;
    }
    let target_uid = unsafe { *(*view).uid };
    let self_uid = unsafe { a20_rl_current_uid() };
    if target_uid == self_uid { 0 } else { EPERM }
}

extern "C" {
    fn a20_rl_current() -> *mut core::ffi::c_void;
}


#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    static TEST_LOCK: Mutex<()> = Mutex::new(());

    struct FakeTask {
        stack: u64,
        nofile: u64,
        aspace: u64,
        nproc: u64,
        rtprio: u64,
        rtprio_max: u64,
        uid: i32,
        park_depth: i32,
    }

    static mut TASK: FakeTask = FakeTask {
        stack: 0,
        nofile: 0,
        aspace: 0,
        nproc: 0,
        rtprio: 0,
        rtprio_max: 0,
        uid: 0,
        park_depth: 0,
    };
    static mut SELF_UID: i32 = 1000;
    static mut HAS_CAP: i32 = 0;
    static mut CORE_SET: u64 = 0;

    fn reset() -> std::sync::MutexGuard<'static, ()> {
        let guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        unsafe {
            TASK = FakeTask { stack: 0, nofile: 0, aspace: 0, nproc: 0,
                              rtprio: 0, rtprio_max: 0, uid: 1000, park_depth: 0 };
            SELF_UID = 1000;
            HAS_CAP = 0;
            CORE_SET = 0;
        }
        guard
    }

    fn view() -> LimitsView {
        unsafe {
            LimitsView {
                stack: &mut TASK.stack,
                nofile: &mut TASK.nofile,
                aspace: &mut TASK.aspace,
                nproc: &mut TASK.nproc,
                rtprio: &mut TASK.rtprio,
                rtprio_max: &mut TASK.rtprio_max,
                uid: &TASK.uid,
            }
        }
    }

    fn get(resource: i32, core: u64) -> (i32, [u64; 2]) {
        let v = view();
        let mut pair = [0u64; 2];
        let rc = unsafe { a20_rlimit_get(1 as *mut _, &v, resource, core, pair.as_mut_ptr()) };
        (rc, pair)
    }

    fn set(resource: i32, cur: u64, max: u64) -> i32 {
        let v = view();
        unsafe { a20_rlimit_set(1 as *mut _, &v, resource, cur, max) }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_rl_park_lock(_t: *mut core::ffi::c_void) -> u64 {
        unsafe { TASK.park_depth += 1 };
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_rl_park_unlock(_t: *mut core::ffi::c_void, _flags: u64) {
        unsafe { TASK.park_depth -= 1 };
    }
    #[no_mangle]
    unsafe extern "C" fn a20_rl_has_cap(_t: *mut core::ffi::c_void, _cap: i32) -> i32 {
        unsafe { HAS_CAP }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_rl_core_set(_t: *mut core::ffi::c_void, limit: u64) {
        unsafe { CORE_SET = limit };
    }
    #[no_mangle]
    unsafe extern "C" fn a20_rl_current_uid() -> i32 {
        unsafe { SELF_UID }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_rl_current() -> *mut core::ffi::c_void {
        1 as *mut core::ffi::c_void
    }

    #[test]
    fn stack_is_clamped_to_the_hard_ceiling() {
        let _g = reset();
        assert_eq!(set(RLIMIT_STACK, 4 * 1024 * 1024, 8 * 1024 * 1024), 0);
        assert_eq!(unsafe { TASK.stack }, 4 * 1024 * 1024);
        assert_eq!(set(RLIMIT_STACK, 64 * 1024 * 1024, 64 * 1024 * 1024), 0);
        assert_eq!(unsafe { TASK.stack }, USER_STACK_MAX_SIZE);
        // min(cur, max) applies before the ceiling.
        assert_eq!(set(RLIMIT_STACK, 8 * 1024 * 1024, 2 * 1024 * 1024), 0);
        assert_eq!(unsafe { TASK.stack }, 2 * 1024 * 1024);
        let (rc, pair) = get(RLIMIT_STACK, 0);
        assert_eq!(rc, 0);
        assert_eq!(pair, [2 * 1024 * 1024, 2 * 1024 * 1024]);
    }

    #[test]
    fn nofile_is_clamped_to_max_files() {
        let _g = reset();
        assert_eq!(set(RLIMIT_NOFILE, 100000, 100000), 0);
        assert_eq!(unsafe { TASK.nofile }, MAX_FILES);
        assert_eq!(get(RLIMIT_NOFILE, 0).1, [MAX_FILES, MAX_FILES]);
    }

    #[test]
    fn as_and_nproc_are_stored_as_given() {
        let _g = reset();
        assert_eq!(set(RLIMIT_AS, 123, 456), 0);
        assert_eq!(unsafe { TASK.aspace }, 123);
        assert_eq!(set(RLIMIT_NPROC, 7, 9), 0);
        assert_eq!(unsafe { TASK.nproc }, 7);
        assert_eq!(get(RLIMIT_NPROC, 0).1, [7, 7]);
    }

    #[test]
    fn the_core_limit_is_the_signal_machinerys() {
        let _g = reset();
        assert_eq!(get(RLIMIT_CORE, 99).1, [99, 99]);
        assert_eq!(set(RLIMIT_CORE, 42, 42), 0);
        assert_eq!(unsafe { CORE_SET }, 42);
    }

    #[test]
    fn an_unenforced_resource_is_refused_not_faked() {
        let _g = reset();
        assert_eq!(get(2, 0).0, ENOSYS);
        assert_eq!(set(2, 1, 1), EINVAL);
    }

    #[test]
    fn rtprio_reads_and_writes_under_the_park_lock() {
        let _g = reset();
        unsafe {
            TASK.rtprio = 3;
            TASK.rtprio_max = 5;
        }
        assert_eq!(get(RLIMIT_RTPRIO, 0).1, [3, 5]);
        assert_eq!(set(RLIMIT_RTPRIO, 2, 5), 0);
        assert_eq!(unsafe { TASK.rtprio }, 2);
        assert_eq!(unsafe { TASK.park_depth }, 0);
    }

    #[test]
    fn rtprio_requires_the_capability_to_raise_the_hard_limit() {
        let _g = reset();
        unsafe { TASK.rtprio_max = 5 };
        assert_eq!(set(RLIMIT_RTPRIO, 1, 10), EPERM);
        assert_eq!(unsafe { TASK.rtprio_max }, 5);
        unsafe { HAS_CAP = 1 };
        assert_eq!(set(RLIMIT_RTPRIO, 1, 10), 0);
        assert_eq!(unsafe { TASK.rtprio_max }, 10);
        // Lowering the hard limit needs no capability.
        unsafe { HAS_CAP = 0 };
        assert_eq!(set(RLIMIT_RTPRIO, 1, 4), 0);
    }

    #[test]
    fn rtprio_rejects_a_soft_limit_above_the_hard_one() {
        let _g = reset();
        assert_eq!(set(RLIMIT_RTPRIO, 6, 5), EINVAL);
        assert_eq!(unsafe { TASK.park_depth }, 0);
    }

    #[test]
    fn a_missing_task_reports_the_defaults() {
        let _g = reset();
        let mut pair = [0u64; 2];
        assert_eq!(unsafe { a20_rlimit_get(core::ptr::null_mut(), core::ptr::null(), RLIMIT_STACK, 0, pair.as_mut_ptr()) }, 0);
        assert_eq!(pair, [USER_STACK_MAX_SIZE, USER_STACK_MAX_SIZE]);
        assert_eq!(unsafe { a20_rlimit_get(core::ptr::null_mut(), core::ptr::null(), RLIMIT_NOFILE, 0, pair.as_mut_ptr()) }, 0);
        assert_eq!(pair, [MAX_FILES, MAX_FILES]);
        assert_eq!(unsafe { a20_rlimit_get(core::ptr::null_mut(), core::ptr::null(), RLIMIT_AS, 0, pair.as_mut_ptr()) }, 0);
        assert_eq!(pair, [0, 0]);
        assert_eq!(unsafe { a20_rlimit_get(core::ptr::null_mut(), core::ptr::null(), RLIMIT_RTPRIO, 0, pair.as_mut_ptr()) }, ESRCH);
        assert_eq!(unsafe { a20_rlimit_set(core::ptr::null_mut(), core::ptr::null(), RLIMIT_AS, 1, 1) }, ESRCH);
    }

    #[test]
    fn changing_another_tasks_limits_needs_privilege_or_a_shared_uid() {
        let _g = reset();
        let v = view();
        unsafe {
            assert_eq!(a20_prlimit_may_change(&v, 1, 0), 0, "the caller itself");
            assert_eq!(a20_prlimit_may_change(&v, 0, 1), 0, "CAP_SYS_RESOURCE");
            assert_eq!(a20_prlimit_may_change(&v, 0, 0), 0, "same uid");
            SELF_UID = 2000;
            assert_eq!(a20_prlimit_may_change(&v, 0, 0), EPERM);
        }
    }
}
