#![no_std]

//! Fork/task construction transaction controller.
//!
//! Rust owns stage ordering, the publish boundary, deferred-ready policy and
//! the single rollback decision. The C adapter supplies narrow resource and
//! architecture operations; it must leave partially acquired resources
//! attached to the task so the existing destructor can release them.

#[cfg(test)]
extern crate std;

// The operation table layout is shared with kernel/proc/clone_txn.rs, which
// builds the table for proc_clone(). One definition on disk, two objects.
include!("task_construction_ops.rs");

pub const TASK_BUILD_VALIDATE: u32 = 0;
pub const TASK_BUILD_ALLOCATE: u32 = 1;
pub const TASK_BUILD_BASE: u32 = 2;
pub const TASK_BUILD_SECCOMP: u32 = 3;
pub const TASK_BUILD_FS_CGROUP: u32 = 4;
pub const TASK_BUILD_FILES: u32 = 5;
pub const TASK_BUILD_SIGNALS: u32 = 6;
pub const TASK_BUILD_NAMESPACES: u32 = 7;
pub const TASK_BUILD_CAPABILITIES: u32 = 8;
pub const TASK_BUILD_MM: u32 = 9;
pub const TASK_BUILD_ARCH: u32 = 10;
pub const TASK_BUILD_PUBLISH: u32 = 11;
pub const TASK_BUILD_POST_PUBLISH: u32 = 12;
pub const TASK_BUILD_READY: u32 = 13;
pub const TASK_BUILD_VFORK_WAIT: u32 = 14;

#[inline]
fn drive_transaction<F, W, A>(deferred: bool, vfork: bool, mut run: F,
                              mut wait_vfork: W, mut abort: A) -> i32
where
    F: FnMut(u32) -> i32,
    W: FnMut(),
    A: FnMut(u8),
{
    if deferred && vfork {
        return -22;
    }
    // Mark before invoking publication because the callback may link the
    // global task slot and then fail while linking group/child metadata.
    // Rollback must handle every partial publication prefix.
    let mut publication_attempted = false;
    for stage in 0..=TASK_BUILD_POST_PUBLISH {
        if stage == TASK_BUILD_PUBLISH {
            publication_attempted = true;
        }
        let rc = run(stage);
        if rc < 0 {
            abort(publication_attempted as u8);
            return rc;
        }
    }
    if !deferred {
        let rc = run(TASK_BUILD_READY);
        if rc < 0 {
            abort(publication_attempted as u8);
            return rc;
        }
        // CLONE_VFORK is not a deferred-ready mode. The wait callback cannot
        // fail; once the child is runnable, rollback would be invalid.
        if vfork {
            wait_vfork();
        }
    }
    0
}

/// Execute a task-construction transaction.
///
/// # Safety
/// `ops` and every callback context must remain valid for the complete call.
/// Every function pointer in `ops` must be non-null, point to a callback with
/// the declared C ABI, and remain executable for the complete call; the C
/// adapter validates this before entering Rust.
/// Callbacks run synchronously and must obey the lock/lifetime contract in
/// `docs/rust/task-construction.md`. `abort` must tolerate failure before
/// allocation and must be idempotent for a task already detached by a failed
/// publication operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_task_construction_run(ops: *const TaskConstructionOps) -> i32 {
    if ops.is_null() {
        return -22;
    }
    let ops = unsafe { &*ops };
    drive_transaction(
        ops.deferred != 0,
        ops.vfork != 0,
        |stage| {
            let callback = match stage {
                TASK_BUILD_VALIDATE => ops.validate,
                TASK_BUILD_ALLOCATE => ops.allocate,
                TASK_BUILD_BASE => ops.base,
                TASK_BUILD_SECCOMP => ops.seccomp,
                TASK_BUILD_FS_CGROUP => ops.fs_cgroup,
                TASK_BUILD_FILES => ops.files,
                TASK_BUILD_SIGNALS => ops.signals,
                TASK_BUILD_NAMESPACES => ops.namespaces,
                TASK_BUILD_CAPABILITIES => ops.capabilities,
                TASK_BUILD_MM => ops.mm,
                TASK_BUILD_ARCH => ops.arch,
                TASK_BUILD_PUBLISH => ops.publish,
                TASK_BUILD_POST_PUBLISH => ops.post_publish,
                TASK_BUILD_READY => ops.ready,
                _ => return -22,
            };
            unsafe { callback(ops.context) }
        },
        || unsafe { (ops.vfork_wait)(ops.context) },
        |published| unsafe { (ops.abort)(ops.context, published) },
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::{Cell, RefCell};
    use std::vec::Vec;

    #[test]
    fn ordered_success_reaches_ready_once() {
        let mut seen = Vec::new();
        let rc = drive_transaction(false, false, |s| { seen.push(s); 0 }, || {}, |_| panic!("unexpected abort"));
        assert_eq!(rc, 0);
        assert_eq!(seen, (0..=TASK_BUILD_READY).collect::<Vec<_>>());
    }

    #[test]
    fn deferred_success_stops_after_post_publish() {
        let mut seen = Vec::new();
        let rc = drive_transaction(true, false, |s| { seen.push(s); 0 }, || {}, |_| panic!("unexpected abort"));
        assert_eq!(rc, 0);
        assert_eq!(seen, (0..=TASK_BUILD_POST_PUBLISH).collect::<Vec<_>>());
    }

    #[test]
    fn deferred_vfork_combination_is_rejected_before_callbacks() {
        let mut called = false;
        let rc = drive_transaction(true, true, |_| { called = true; 0 }, || {}, |_| {});
        assert_eq!(rc, -22);
        assert!(!called);
    }

    #[test]
    fn vfork_wait_runs_only_after_ready_for_nondeferred_clones() {
        let seen = RefCell::new(Vec::new());
        let rc = drive_transaction(false, true,
            |s| { seen.borrow_mut().push(s); 0 },
            || seen.borrow_mut().push(TASK_BUILD_VFORK_WAIT),
            |_| panic!("unexpected abort"));
        assert_eq!(rc, 0);
        let seen = seen.into_inner();
        assert_eq!(seen[seen.len() - 2..], [TASK_BUILD_READY, TASK_BUILD_VFORK_WAIT]);
    }

    #[test]
    fn each_prepublication_failure_rolls_back_once_without_published_flag() {
        for failed in 0..=TASK_BUILD_PUBLISH {
            let mut seen = Vec::new();
            let mut rollback = Vec::new();
            let rc = drive_transaction(false, false, |s| {
                seen.push(s);
                if s == failed { -12 } else { 0 }
            }, || {}, |published| rollback.push(published));
            assert_eq!(rc, -12);
            assert_eq!(rollback, [u8::from(failed == TASK_BUILD_PUBLISH)]);
            assert_eq!(seen.last(), Some(&failed));
        }
    }

    #[test]
    fn postpublication_failures_rollback_with_published_state() {
        for failed in [TASK_BUILD_POST_PUBLISH, TASK_BUILD_READY] {
            let mut rollback = Vec::new();
            let rc = drive_transaction(false, false, |s| if s == failed { -14 } else { 0 },
                                       || {}, |published| rollback.push(published));
            assert_eq!(rc, -14);
            assert_eq!(rollback, [1]);
        }
    }

    #[test]
    fn deferred_postpublication_failure_rolls_back_but_deferred_success_does_not_ready() {
        let mut rollback = Vec::new();
        let rc = drive_transaction(true, false, |s| if s == TASK_BUILD_POST_PUBLISH { -14 } else { 0 },
                                   || {}, |published| rollback.push(published));
        assert_eq!(rc, -14);
        assert_eq!(rollback, [1]);
    }

    #[test]
    fn publish_mutation_followed_by_error_is_rolled_back_as_published() {
        let linked = Cell::new(false);
        let mut rollback = Vec::new();
        let rc = drive_transaction(false, false, |stage| {
            if stage == TASK_BUILD_PUBLISH {
                linked.set(true);
                return -3;
            }
            0
        }, || {}, |publication_attempted| {
            assert_ne!(publication_attempted, 0);
            if linked.get() {
                linked.set(false);
            }
            rollback.push(u8::from(linked.get()));
        });
        assert_eq!(rc, -3);
        assert!(!linked.get());
        assert_eq!(rollback, [0]);
    }
}
