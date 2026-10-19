//! Mount-namespace lifecycle state engine (Rust-owned split for `vfs/mntns.c`).
//!
//! `mntns.c` gates every VFS mutation and file operation behind a mount
//! namespace that may be shutting down, and shutdown itself is admitted only
//! from a quiescent state.  This module owns that state and those decisions:
//!
//! * the in-flight counters (namespace creation, VFS mutations, VFS file
//!   operations) and the shutdown flag itself;
//! * the mutation/fileop admission decision (`-ESHUTDOWN` once shutdown is
//!   active) and the enter/leave counter transitions;
//! * namespace-creation admission (`-EBUSY` during shutdown);
//! * `mntns_shutdown_begin`: whether shutdown may start at all -- it requires
//!   no shutdown already active, no creation/mutation/fileop in flight, and a
//!   mount registry the adapter can snapshot -- and the ordering rule that the
//!   shutdown flag is published only after the pinned snapshot is ready;
//! * the abort transition (clear the flag, hand the pinned count back);
//! * the teardown-pass counter and the selection of the next namespace to tear
//!   down (the retry loop's policy).
//!
//! The adapter keeps the spinlock, the namespace list, the shutdown snapshot
//! array and the refcounts: it walks the registry and pins the snapshot in the
//! `scan` callback, and collects per-namespace teardown state for
//! `a20_mntns_retry_select`.  Everything here runs inside the adapter's lock;
//! nothing allocates or sleeps.

#![cfg_attr(not(test), no_std)]

use core::ffi::{c_int, c_void};

pub const MNTNS_EBUSY: c_int = -16;
pub const MNTNS_ESHUTDOWN: c_int = -108;
pub const MNTNS_EINVAL: c_int = -22;

/// Rust mirror of the adapter's counter block (`fs/vfs/mntns_lifecycle.h`).
#[repr(C)]
pub struct MntnsLifecycle {
    pub create_inflight: u32,
    pub mutations_inflight: u32,
    pub fileops_inflight: u32,
    pub shutdown_active: u32,
    pub shutdown_count: u32,
    pub shutdown_cap: u32,
    pub teardown_pass: u64,
}

/// Whether the adapter may snapshot the registry.  Runs under the adapter's
/// lock: it validates the registry against `cap` and pins each namespace it
/// reports, so a 0 return means the pinned snapshot is complete.
pub type MntnsScanFn =
    unsafe extern "C" fn(ctx: *mut c_void, cap: u32, out_count: *mut u32) -> c_int;

/// Per-namespace teardown state, collected by the adapter for the retry
/// policy.  Field order mirrors the C struct.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct MntnsRetryCand {
    pub teardown_pending: u32,
    pub teardown_running: u32,
    pub retryable: u32,
    pub _pad: u32,
    pub attempt_pass: u64,
}

/// # Safety
/// `st` is a live, zeroed-or-initialised counter block.
#[no_mangle]
pub unsafe extern "C" fn a20_mntns_lifecycle_init(st: *mut MntnsLifecycle, cap: u32) {
    if st.is_null() {
        return;
    }
    // SAFETY: st is writable by contract.
    unsafe {
        (*st).create_inflight = 0;
        (*st).mutations_inflight = 0;
        (*st).fileops_inflight = 0;
        (*st).shutdown_active = 0;
        (*st).shutdown_count = 0;
        (*st).shutdown_cap = cap;
        (*st).teardown_pass = 0;
    }
}

/// Namespace creation admission.  Refused once shutdown is active.
///
/// # Safety
/// `st` is a live counter block.
#[no_mangle]
pub unsafe extern "C" fn a20_mntns_create_enter(st: *mut MntnsLifecycle) -> c_int {
    if st.is_null() {
        return MNTNS_EBUSY;
    }
    // SAFETY: st is live by contract.
    unsafe {
        if (*st).shutdown_active != 0 {
            return MNTNS_EBUSY;
        }
        (*st).create_inflight += 1;
        0
    }
}

/// # Safety
/// `st` is a live counter block.
#[no_mangle]
pub unsafe extern "C" fn a20_mntns_create_leave(st: *mut MntnsLifecycle) -> u32 {
    if st.is_null() {
        return 0;
    }
    // SAFETY: st is live by contract.
    unsafe {
        if (*st).create_inflight > 0 {
            (*st).create_inflight -= 1;
        }
        (*st).create_inflight
    }
}

/// # Safety
/// `st` is a live counter block.
#[no_mangle]
pub unsafe extern "C" fn a20_mntns_mutation_enter(st: *mut MntnsLifecycle) -> c_int {
    if st.is_null() {
        return MNTNS_ESHUTDOWN;
    }
    // SAFETY: st is live by contract.
    unsafe {
        if (*st).shutdown_active != 0 {
            return MNTNS_ESHUTDOWN;
        }
        (*st).mutations_inflight += 1;
        0
    }
}

/// # Safety
/// `st` is a live counter block.
#[no_mangle]
pub unsafe extern "C" fn a20_mntns_mutation_leave(st: *mut MntnsLifecycle) -> u32 {
    if st.is_null() {
        return 0;
    }
    // SAFETY: st is live by contract.
    unsafe {
        if (*st).mutations_inflight > 0 {
            (*st).mutations_inflight -= 1;
        }
        (*st).mutations_inflight
    }
}

/// # Safety
/// `st` is a live counter block.
#[no_mangle]
pub unsafe extern "C" fn a20_mntns_fileop_enter(st: *mut MntnsLifecycle) -> c_int {
    if st.is_null() {
        return MNTNS_ESHUTDOWN;
    }
    // SAFETY: st is live by contract.
    unsafe {
        if (*st).shutdown_active != 0 {
            return MNTNS_ESHUTDOWN;
        }
        (*st).fileops_inflight += 1;
        0
    }
}

/// # Safety
/// `st` is a live counter block.
#[no_mangle]
pub unsafe extern "C" fn a20_mntns_fileop_leave(st: *mut MntnsLifecycle) -> u32 {
    if st.is_null() {
        return 0;
    }
    // SAFETY: st is live by contract.
    unsafe {
        if (*st).fileops_inflight > 0 {
            (*st).fileops_inflight -= 1;
        }
        (*st).fileops_inflight
    }
}

/// # Safety
/// `st` is a live counter block.
#[no_mangle]
pub unsafe extern "C" fn a20_mntns_shutdown_active(st: *const MntnsLifecycle) -> c_int {
    if st.is_null() {
        return 0;
    }
    // SAFETY: st is live by contract.
    unsafe { ((*st).shutdown_active != 0) as c_int }
}

/// # Safety
/// `st` is a live counter block.
#[no_mangle]
pub unsafe extern "C" fn a20_mntns_shutdown_count(st: *const MntnsLifecycle) -> c_int {
    if st.is_null() {
        return 0;
    }
    // SAFETY: st is live by contract.
    unsafe {
        if (*st).shutdown_active == 0 {
            return 0;
        }
        (*st).shutdown_count as c_int
    }
}

/// Admit shutdown: refuse while anything is in flight, then let the adapter
/// pin its snapshot and only afterwards publish the shutdown flag.
///
/// # Safety
/// `st` is a live counter block and `scan` is the adapter's snapshot callback,
/// called with the adapter's registry lock held.
#[no_mangle]
pub unsafe extern "C" fn a20_mntns_shutdown_begin(
    st: *mut MntnsLifecycle,
    ctx: *mut c_void,
    scan: Option<MntnsScanFn>,
) -> c_int {
    if st.is_null() {
        return MNTNS_EBUSY;
    }
    // SAFETY: st is live by contract.
    unsafe {
        if (*st).shutdown_active != 0
            || (*st).create_inflight != 0
            || (*st).mutations_inflight != 0
            || (*st).fileops_inflight != 0
        {
            return MNTNS_EBUSY;
        }
        let scan = match scan {
            Some(f) => f,
            None => return MNTNS_EBUSY,
        };
        let mut count = 0u32;
        let r = scan(ctx, (*st).shutdown_cap, &mut count);
        if r < 0 {
            return r;
        }
        (*st).shutdown_count = count;
        // Published only after the complete pinned snapshot is ready.
        (*st).shutdown_active = 1;
        0
    }
}

/// The abort transition: clears the flag and returns the pinned count so the
/// adapter can release the snapshot.  Returns `-EINVAL` when shutdown is not
/// active, matching the adapter's early return.
///
/// # Safety
/// `st` is a live counter block.
#[no_mangle]
pub unsafe extern "C" fn a20_mntns_shutdown_abort_plan(st: *mut MntnsLifecycle) -> c_int {
    if st.is_null() {
        return MNTNS_EINVAL;
    }
    // SAFETY: st is live by contract.
    unsafe {
        if (*st).shutdown_active == 0 {
            return MNTNS_EINVAL;
        }
        let n = (*st).shutdown_count;
        (*st).shutdown_count = 0;
        (*st).shutdown_active = 0;
        n as c_int
    }
}

/// Allocate the next teardown pass.  Zero is skipped: it is the "never
/// attempted" marker a freshly created namespace carries.
///
/// # Safety
/// `st` is a live counter block.
#[no_mangle]
pub unsafe extern "C" fn a20_mntns_retry_pass(st: *mut MntnsLifecycle) -> u64 {
    if st.is_null() {
        return 0;
    }
    // SAFETY: st is live by contract.
    unsafe {
        (*st).teardown_pass = (*st).teardown_pass.wrapping_add(1);
        if (*st).teardown_pass == 0 {
            (*st).teardown_pass = (*st).teardown_pass.wrapping_add(1);
        }
        (*st).teardown_pass
    }
}

/// Choose the next namespace to tear down, or -1 when the pass has no
/// candidate.  The caller supplies the per-namespace state it collected under
/// its lock and applies the mark to the returned index.
///
/// # Safety
/// `cands` points at `n` initialised descriptors.
#[no_mangle]
pub unsafe extern "C" fn a20_mntns_retry_select(
    cands: *const MntnsRetryCand,
    n: usize,
    pass: u64,
    allow_busy_retry: c_int,
) -> c_int {
    if cands.is_null() {
        return -1;
    }
    for i in 0..n {
        // SAFETY: i < n and cands points at n descriptors.
        let c = unsafe { &*cands.add(i) };
        if c.teardown_pending != 0
            && c.teardown_running == 0
            && c.attempt_pass != pass
            && (allow_busy_retry != 0 || c.retryable != 0)
        {
            return i as c_int;
        }
    }
    -1
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::cell::Cell;

    fn fresh() -> MntnsLifecycle {
        let mut st = MntnsLifecycle {
            create_inflight: 0,
            mutations_inflight: 0,
            fileops_inflight: 0,
            shutdown_active: 0,
            shutdown_count: 0,
            shutdown_cap: 256,
            teardown_pass: 0,
        };
        unsafe { a20_mntns_lifecycle_init(&mut st, 256) };
        st
    }

    thread_local! {
        static SCAN_COUNT: Cell<u32> = const { Cell::new(2) };
        static SCAN_RC: Cell<i32> = const { Cell::new(0) };
    }

    unsafe extern "C" fn scan_stub(_ctx: *mut c_void, cap: u32, out: *mut u32) -> c_int {
        let rc = SCAN_RC.with(|c| c.get());
        let n = SCAN_COUNT.with(|c| c.get());
        if rc != 0 {
            return rc;
        }
        if n > cap {
            return MNTNS_EBUSY;
        }
        unsafe { *out = n };
        0
    }

    #[test]
    fn admission_refuses_once_shutdown_is_active() {
        let mut st = fresh();
        unsafe {
            assert_eq!(a20_mntns_mutation_enter(&mut st), 0);
            assert_eq!(a20_mntns_mutation_enter(&mut st), 0);
            assert_eq!(a20_mntns_mutation_leave(&mut st), 1);
            assert_eq!(a20_mntns_mutation_leave(&mut st), 0);
            assert_eq!(a20_mntns_fileop_enter(&mut st), 0);
            assert_eq!(a20_mntns_fileop_leave(&mut st), 0);
            assert_eq!(a20_mntns_create_enter(&mut st), 0);
            assert_eq!(a20_mntns_create_leave(&mut st), 0);
            // In flight: shutdown is refused.
            assert_eq!(a20_mntns_mutation_enter(&mut st), 0);
            assert_eq!(a20_mntns_shutdown_begin(&mut st, core::ptr::null_mut(), Some(scan_stub)), MNTNS_EBUSY);
            assert_eq!(a20_mntns_mutation_leave(&mut st), 0);
            // Quiescent: admitted, and then both kinds of work are refused.
            assert_eq!(a20_mntns_shutdown_begin(&mut st, core::ptr::null_mut(), Some(scan_stub)), 0);
            assert_eq!(a20_mntns_shutdown_active(&st), 1);
            assert_eq!(a20_mntns_shutdown_count(&st), 2);
            assert_eq!(a20_mntns_mutation_enter(&mut st), MNTNS_ESHUTDOWN);
            assert_eq!(a20_mntns_fileop_enter(&mut st), MNTNS_ESHUTDOWN);
            assert_eq!(a20_mntns_create_enter(&mut st), MNTNS_EBUSY);
            // A second shutdown is refused, and abort cannot run twice.
            assert_eq!(a20_mntns_shutdown_begin(&mut st, core::ptr::null_mut(), Some(scan_stub)), MNTNS_EBUSY);
            assert_eq!(a20_mntns_shutdown_abort_plan(&mut st), 2);
            assert_eq!(a20_mntns_shutdown_active(&st), 0);
            assert_eq!(a20_mntns_shutdown_abort_plan(&mut st), MNTNS_EINVAL);
            // Admission is open again.
            assert_eq!(a20_mntns_mutation_enter(&mut st), 0);
            assert_eq!(a20_mntns_mutation_leave(&mut st), 0);
        }
    }

    #[test]
    fn shutdown_propagates_a_scan_refusal() {
        let mut st = fresh();
        SCAN_RC.with(|c| c.set(MNTNS_EBUSY));
        unsafe {
            assert_eq!(a20_mntns_shutdown_begin(&mut st, core::ptr::null_mut(), Some(scan_stub)), MNTNS_EBUSY);
            assert_eq!(a20_mntns_shutdown_active(&st), 0);
        }
        SCAN_RC.with(|c| c.set(0));
        SCAN_COUNT.with(|c| c.set(300));
        unsafe {
            assert_eq!(a20_mntns_shutdown_begin(&mut st, core::ptr::null_mut(), Some(scan_stub)), MNTNS_EBUSY);
            assert_eq!(a20_mntns_shutdown_active(&st), 0);
        }
        SCAN_COUNT.with(|c| c.set(2));
    }

    #[test]
    fn teardown_pass_skips_zero() {
        let mut st = fresh();
        let p1 = unsafe { a20_mntns_retry_pass(&mut st) };
        assert_ne!(p1, 0);
        // Wrap to zero must skip it.
        unsafe { st.teardown_pass = u64::MAX };
        let p2 = unsafe { a20_mntns_retry_pass(&mut st) };
        assert_ne!(p2, 0);
        assert_eq!(p2, 1);
    }

    #[test]
    fn retry_selects_the_first_eligible_namespace() {
        let mk = |pending: u32, running: u32, retryable: u32, pass: u64| MntnsRetryCand {
            teardown_pending: pending,
            teardown_running: running,
            retryable,
            _pad: 0,
            attempt_pass: pass,
        };
        let cands = [
            mk(1, 0, 1, 7),   // already attempted this pass
            mk(1, 1, 1, 0),   // already running
            mk(0, 0, 1, 0),   // not pending
            mk(1, 0, 1, 0),   // eligible
            mk(1, 0, 0, 0),   // only eligible for a busy retry
        ];
        unsafe {
            assert_eq!(a20_mntns_retry_select(cands.as_ptr(), cands.len(), 7, 0), 3);
            assert_eq!(a20_mntns_retry_select(cands.as_ptr(), cands.len(), 7, 1), 3);
            assert_eq!(a20_mntns_retry_select(cands.as_ptr(), 3, 7, 1), -1);
        }
        let only_unretryable = [mk(1, 0, 0, 0)];
        unsafe {
            assert_eq!(a20_mntns_retry_select(only_unretryable.as_ptr(), 1, 7, 0), -1);
            assert_eq!(a20_mntns_retry_select(only_unretryable.as_ptr(), 1, 7, 1), 0);
        }
    }
}
