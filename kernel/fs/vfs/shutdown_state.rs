//! Rust-owned phase controller for orderly system shutdown.
//!
//! The callbacks pin and gate VFS state, perform checked I/O, and tear down
//! filesystem instances. This module owns which phase may be retried: a
//! failure before teardown is abortable; once teardown starts, the system
//! remains quiesced and retries resume teardown rather than reopening writes.
use core::ffi::c_void;
use core::mem::{offset_of, size_of};
use core::sync::atomic::{AtomicU32, Ordering};

const IDLE: u32 = 0;
const RUNNING: u32 = 1;
const RETRY_SYNC: u32 = 2;
const RETRY_COMMIT: u32 = 3;
const RETRY_TEARDOWN: u32 = 4;
const COMPLETE: u32 = 5;
const EBUSY: i32 = -16;
const EINVAL: i32 = -22;

pub type PhaseOp = unsafe extern "C" fn(*mut c_void) -> i32;

#[repr(C)]
pub struct ShutdownOps {
    pub context: *mut c_void,
    /// Atomically close mutation/mount admission and pin the global namespace set.
    pub begin: Option<PhaseOp>,
    /// Drain admitted writers, flush all pinned instances, then persist clean state.
    pub sync: Option<PhaseOp>,
    /// Tear down pinned filesystem instances in dependency order. Must be idempotent.
    pub teardown: Option<PhaseOp>,
    /// Drop snapshot pins after a pre-teardown failure and reopen admission.
    pub abort: Option<PhaseOp>,
    /// Make the commit point irreversible (for example commit an MM writer freeze).
    pub commit: Option<PhaseOp>,
}

const _: () = {
    assert!(offset_of!(ShutdownOps, context) == 0);
    assert!(offset_of!(ShutdownOps, begin) == size_of::<*mut c_void>());
    assert!(offset_of!(ShutdownOps, sync) == 2 * size_of::<*mut c_void>());
    assert!(offset_of!(ShutdownOps, teardown) == 3 * size_of::<*mut c_void>());
    assert!(offset_of!(ShutdownOps, abort) == 4 * size_of::<*mut c_void>());
    assert!(offset_of!(ShutdownOps, commit) == 5 * size_of::<*mut c_void>());
    assert!(size_of::<ShutdownOps>() == 6 * size_of::<*mut c_void>());
};

static PHASE: AtomicU32 = AtomicU32::new(IDLE);

unsafe fn call(op: Option<PhaseOp>, context: *mut c_void) -> i32 {
    match op {
        Some(f) => f(context),
        None => EINVAL,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_vfs_shutdown_run(ops: *const ShutdownOps) -> i32 {
    if ops.is_null() {
        return EINVAL;
    }
    let ops = &*ops;
    loop {
        let old = PHASE.load(Ordering::Acquire);
        if old == COMPLETE {
            return 0;
        }
        if old == RUNNING {
            return EBUSY;
        }
        if old > COMPLETE {
            return EINVAL;
        }
        if PHASE.compare_exchange(old, RUNNING, Ordering::AcqRel, Ordering::Acquire).is_ok() {
            let mut phase = old;
            if phase == IDLE {
                let ret = call(ops.begin, ops.context);
                if ret < 0 {
                    PHASE.store(IDLE, Ordering::Release);
                    return ret;
                }
                phase = RETRY_SYNC;
            }

            if phase == RETRY_SYNC {
                let ret = call(ops.sync, ops.context);
                if ret < 0 {
                    let abort_ret = call(ops.abort, ops.context);
                    if abort_ret == 0 {
                        PHASE.store(IDLE, Ordering::Release);
                    } else {
                        /* Keep the pinned snapshot and closed admission so a
                         * later shutdown attempt can safely retry the flush. */
                        PHASE.store(RETRY_SYNC, Ordering::Release);
                    }
                    return ret;
                }
                phase = RETRY_COMMIT;
            }

            if phase == RETRY_COMMIT {
                let ret = call(ops.commit, ops.context);
                if ret < 0 {
                    /* Commit may have completed an irreversible substep before
                     * reporting an error. Never abort/reopen here; retry the
                     * idempotent commit callback while admission stays sealed. */
                    PHASE.store(RETRY_COMMIT, Ordering::Release);
                    return ret;
                }
                phase = RETRY_TEARDOWN;
            }

            if phase == RETRY_TEARDOWN {
                let ret = call(ops.teardown, ops.context);
                if ret < 0 {
                    PHASE.store(RETRY_TEARDOWN, Ordering::Release);
                    return ret;
                }
                PHASE.store(COMPLETE, Ordering::Release);
                return 0;
            }

            PHASE.store(phase, Ordering::Release);
            return EINVAL;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    // PHASE is the production singleton. Independent scenarios must not reset
    // each other's phase while the Rust test runner executes them in parallel.
    static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    #[derive(Default)]
    struct Fake {
        begin: u32,
        sync: u32,
        teardown: u32,
        abort: u32,
        sync_error: i32,
        teardown_error: i32,
        commit_error: i32,
        abort_error: i32,
        commit: u32,
        reentry_result: i32,
        reenter_ops: *mut ShutdownOps,
    }

    unsafe extern "C" fn begin(ctx: *mut c_void) -> i32 {
        let f = &mut *(ctx as *mut Fake);
        f.begin += 1;
        0
    }
    unsafe extern "C" fn sync(ctx: *mut c_void) -> i32 {
        let f = &mut *(ctx as *mut Fake);
        f.sync += 1;
        if !f.reenter_ops.is_null() {
            f.reentry_result = a20_vfs_shutdown_run(f.reenter_ops);
        }
        f.sync_error
    }
    unsafe extern "C" fn teardown(ctx: *mut c_void) -> i32 {
        let f = &mut *(ctx as *mut Fake);
        f.teardown += 1;
        f.teardown_error
    }
    unsafe extern "C" fn abort(ctx: *mut c_void) -> i32 {
        let f = &mut *(ctx as *mut Fake);
        f.abort += 1;
        f.abort_error
    }
    unsafe extern "C" fn commit(ctx: *mut c_void) -> i32 {
        let f = &mut *(ctx as *mut Fake);
        f.commit += 1;
        f.commit_error
    }

    fn ops(f: &mut Fake) -> ShutdownOps {
        ShutdownOps {
            context: f as *mut _ as *mut c_void,
            begin: Some(begin), sync: Some(sync), teardown: Some(teardown),
            abort: Some(abort), commit: Some(commit),
        }
    }

    #[test]
    fn all_phase_transitions_and_running_reentry_are_serialized() {
        let _guard = TEST_LOCK.lock().unwrap();
        unsafe {
            PHASE.store(IDLE, Ordering::Release);
            let mut f = Fake { sync_error: -5, abort_error: -16, ..Fake::default() };
            let mut callback_ops = ops(&mut f);
            f.reenter_ops = &mut callback_ops;
            assert_eq!(a20_vfs_shutdown_run(&callback_ops), -5);
            assert_eq!(f.reentry_result, EBUSY);
            assert_eq!((f.begin, f.sync, f.teardown, f.abort, f.commit), (1, 1, 0, 1, 0));

            f.sync_error = 0;
            f.commit_error = -16;
            f.reenter_ops = core::ptr::null_mut();
            f.teardown_error = -5;
            assert_eq!(a20_vfs_shutdown_run(&callback_ops), -16);
            assert_eq!((f.begin, f.sync, f.teardown, f.abort, f.commit), (1, 2, 0, 1, 1));

            f.commit_error = 0;
            assert_eq!(a20_vfs_shutdown_run(&callback_ops), -5);
            assert_eq!((f.begin, f.sync, f.teardown, f.abort, f.commit), (1, 2, 1, 1, 2));
            f.teardown_error = 0;
            assert_eq!(a20_vfs_shutdown_run(&callback_ops), 0);
            assert_eq!((f.begin, f.sync, f.teardown, f.abort, f.commit), (1, 2, 2, 1, 2));
        }
    }

    #[test]
    fn sync_failure_with_successful_abort_reopens_admission() {
        let _guard = TEST_LOCK.lock().unwrap();
        unsafe {
            PHASE.store(IDLE, Ordering::Release);
            let mut f = Fake { sync_error: -5, ..Fake::default() };
            let callback_ops = ops(&mut f);
            assert_eq!(a20_vfs_shutdown_run(&callback_ops), -5);
            assert_eq!((f.begin, f.sync, f.teardown, f.abort, f.commit), (1, 1, 0, 1, 0));
            assert_eq!(PHASE.load(Ordering::Acquire), IDLE);

            /* Admission reopened: a later attempt runs the full sequence. */
            f.sync_error = 0;
            assert_eq!(a20_vfs_shutdown_run(&callback_ops), 0);
            assert_eq!((f.begin, f.sync, f.teardown, f.abort, f.commit), (2, 2, 1, 1, 1));
            assert_eq!(PHASE.load(Ordering::Acquire), COMPLETE);
        }
    }
}
