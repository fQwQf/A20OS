#![no_std]

//! Resource-namespace membership transactions.
//!
//! Rust owns the UTS/IPC/NET/cgroup namespace *decisions*: which kinds a flag
//! word selects, the inode sequence that gives each namespace its identity, and
//! the fork/unshare transactions including their rollback.  C keeps the
//! namespace objects themselves — allocation, refcounts, the per-namespace
//! lock, the task-slot swap, the initial-namespace pinning and the cgroup path
//! rendering — and exposes them as `a20_res_ns_*` primitives.
//!
//! The unshare transaction allocates every requested namespace before
//! publishing any of them, so a failure in the middle of a multi-domain request
//! leaves every membership intact and releases exactly the objects it created.

#[cfg(test)]
extern crate std;

use core::ffi::c_void;
use core::ptr;
use core::sync::atomic::{AtomicU32, Ordering};
#[cfg(target_has_atomic = "64")]
use core::sync::atomic::AtomicU64;

const EINVAL: i32 = 22;
const ENOMEM: i32 = 12;

/// `PROCNS_*` slots an unshare/fork can create.  Mount, pid and user
/// namespaces have their own modules and are not resource kinds.
const KIND_MAX: usize = 8;
const PROCNS_UTS: i32 = 3;
const PROCNS_IPC: i32 = 5;
const PROCNS_NET: i32 = 6;
const PROCNS_CGROUP: i32 = 7;

/// Linux clone flags, from `proc/nsclone.h`.
const CLONE_NEWCGROUP: u64 = 0x0200_0000;
const CLONE_NEWUTS: u64 = 0x0400_0000;
const CLONE_NEWIPC: u64 = 0x0800_0000;
const CLONE_NEWNET: u64 = 0x4000_0000;

/// First inode handed out for non-initial namespaces; the initial objects have
/// their own fixed inodes below this.
const FIRST_INO: u64 = 4_026_600_000;

/* riscv32/arm32 have no 64-bit atomics.  Those profiles count the offset from
 * FIRST_INO in 32 bits, which wraps only after four billion namespace
 * creations; the sequence is an opaque identity, not a capacity. */
#[cfg(target_has_atomic = "64")]
static NEXT_INO: AtomicU64 = AtomicU64::new(FIRST_INO);
#[cfg(not(target_has_atomic = "64"))]
static NEXT_INO: AtomicU32 = AtomicU32::new(0);

fn kind_flag(kind: i32) -> u64 {
    match kind {
        PROCNS_UTS => CLONE_NEWUTS,
        PROCNS_IPC => CLONE_NEWIPC,
        PROCNS_NET => CLONE_NEWNET,
        PROCNS_CGROUP => CLONE_NEWCGROUP,
        _ => 0,
    }
}

fn valid_kind(kind: i32) -> bool {
    kind > 0 && (kind as usize) < KIND_MAX && kind_flag(kind) != 0
}

fn next_ino() -> u64 {
    #[cfg(target_has_atomic = "64")]
    {
        NEXT_INO.fetch_add(1, Ordering::Relaxed)
    }
    #[cfg(not(target_has_atomic = "64"))]
    {
        FIRST_INO + NEXT_INO.fetch_add(1, Ordering::Relaxed) as u64
    }
}

unsafe extern "C" {
    /// Allocate and initialise one namespace of `kind` for `task`, stamped with
    /// `ino`.  Returns the creating reference or null.
    fn a20_res_ns_new(kind: i32, task: *mut c_void, ino: u64) -> *mut c_void;
    fn a20_res_ns_put(ns: *mut c_void);
    fn a20_res_ns_join(task: *mut c_void, ns: *mut c_void) -> i32;
    fn a20_res_ns_task_get(task: *mut c_void, kind: i32) -> *mut c_void;
    fn a20_res_ns_set_task_slot(task: *mut c_void, kind: i32, ns: *mut c_void);
    fn a20_res_ns_release_task(task: *mut c_void);
}

/// Is `kind` a resource-namespace kind this module manages?
#[unsafe(no_mangle)]
pub extern "C" fn a20_res_ns_valid_kind(kind: i32) -> i32 {
    valid_kind(kind) as i32
}

/// Create the namespaces `flags` selects and install them on `task`.
///
/// # Safety
/// `task` must be null or a live task the caller may modify.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_res_ns_unshare(task: *mut c_void, flags: u64) -> i32 {
    if task.is_null() {
        return -EINVAL;
    }
    let mut prepared = [ptr::null_mut::<c_void>(); KIND_MAX];
    for kind in 1..KIND_MAX {
        let flag = kind_flag(kind as i32);
        if flag == 0 || flags & flag == 0 {
            continue;
        }
        let ns = unsafe { a20_res_ns_new(kind as i32, task, next_ino()) };
        if ns.is_null() {
            for slot in prepared {
                if !slot.is_null() {
                    unsafe { a20_res_ns_put(slot) };
                }
            }
            return -ENOMEM;
        }
        prepared[kind] = ns;
    }

    // All allocations precede publication: a failed multi-domain request left
    // every membership intact.  `join` takes its own reference, so the creating
    // reference is dropped either way.
    for kind in 1..KIND_MAX {
        let ns = prepared[kind];
        if ns.is_null() {
            continue;
        }
        let _ = unsafe { a20_res_ns_join(task, ns) };
        unsafe { a20_res_ns_put(ns) };
    }
    0
}

/// Copy the parent's memberships to `child`, then apply `flags` as unshare.
///
/// A failed unshare releases everything the child had just inherited, so it
/// never keeps a partial membership set.
///
/// # Safety
/// `child` and `parent` must be live task pointers; the child must not be
/// published yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_res_ns_fork(
    child: *mut c_void,
    parent: *mut c_void,
    flags: u64,
) -> i32 {
    if child.is_null() {
        return -EINVAL;
    }
    for kind in 1..KIND_MAX {
        if !valid_kind(kind as i32) {
            continue;
        }
        let ns = unsafe { a20_res_ns_task_get(parent, kind as i32) };
        unsafe { a20_res_ns_set_task_slot(child, kind as i32, ns) };
    }
    let rc = unsafe { a20_res_ns_unshare(child, flags) };
    if rc < 0 {
        unsafe { a20_res_ns_release_task(child) };
    }
    rc
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::thread_local;
    use std::vec::Vec;

    #[derive(Clone, Copy, PartialEq, Eq, Debug)]
    enum Event {
        New(i32, u64),
        Join(i32),
        Slot(i32, usize),
        Put(i32),
        ReleaseTask,
    }

    thread_local! {
        static EVENTS: RefCell<Vec<Event>> = const { RefCell::new(Vec::new()) };
        // Which kinds the fake allocator refuses, and which join calls fail.
        static FAIL_NEW: RefCell<Option<i32>> = const { RefCell::new(None) };
    }

    fn record(event: Event) {
        EVENTS.with(|e| e.borrow_mut().push(event));
    }

    fn events() -> Vec<Event> {
        EVENTS.with(|e| e.borrow().clone())
    }

    fn reset() {
        EVENTS.with(|e| e.borrow_mut().clear());
        FAIL_NEW.with(|f| *f.borrow_mut() = None);
    }

    // Fake namespace objects are just their kind, encoded as a pointer.
    fn fake_ns(kind: i32) -> *mut c_void {
        (kind as usize) as *mut c_void
    }

    fn ns_kind(ns: *mut c_void) -> i32 {
        ns as usize as i32
    }

    #[unsafe(no_mangle)]
    extern "C" fn a20_res_ns_new(kind: i32, _task: *mut c_void, ino: u64) -> *mut c_void {
        record(Event::New(kind, ino));
        if FAIL_NEW.with(|f| *f.borrow()) == Some(kind) {
            return ptr::null_mut();
        }
        fake_ns(kind)
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_res_ns_put(ns: *mut c_void) {
        record(Event::Put(ns_kind(ns)));
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_res_ns_join(_task: *mut c_void, ns: *mut c_void) -> i32 {
        record(Event::Join(ns_kind(ns)));
        0
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_res_ns_task_get(_task: *mut c_void, kind: i32) -> *mut c_void {
        record(Event::Slot(kind, 0));
        fake_ns(kind)
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_res_ns_set_task_slot(_task: *mut c_void, kind: i32, ns: *mut c_void) {
        record(Event::Slot(kind, ns as usize));
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_res_ns_release_task(_task: *mut c_void) {
        record(Event::ReleaseTask);
    }

    const TASK: *mut c_void = 1 as *mut c_void;

    #[test]
    fn only_the_four_resource_kinds_are_valid() {
        for kind in [PROCNS_UTS, PROCNS_IPC, PROCNS_NET, PROCNS_CGROUP] {
            assert_eq!(a20_res_ns_valid_kind(kind), 1, "kind {kind}");
        }
        for kind in [-1, 0, 1, 2, 4, 8, 99] {
            assert_eq!(a20_res_ns_valid_kind(kind), 0, "kind {kind}");
        }
    }

    #[test]
    fn unshare_allocates_all_kinds_before_publishing_any() {
        reset();
        let flags = CLONE_NEWUTS | CLONE_NEWIPC;
        assert_eq!(unsafe { a20_res_ns_unshare(TASK, flags) }, 0);
        let events = events();
        // Both allocations happen before the first join.
        let last_new = events.iter().rposition(|e| matches!(e, Event::New(..))).unwrap();
        let first_join = events.iter().position(|e| matches!(e, Event::Join(..))).unwrap();
        assert!(last_new < first_join, "publish must follow every allocation: {events:?}");
        assert_eq!(
            events.iter().filter(|e| matches!(e, Event::New(..))).count(),
            2
        );
        assert_eq!(
            events.iter().filter(|e| matches!(e, Event::Join(..))).count(),
            2
        );
        assert_eq!(events.iter().filter(|e| matches!(e, Event::Put(..))).count(), 2);
    }

    #[test]
    fn unshare_with_no_flags_allocates_nothing() {
        reset();
        assert_eq!(unsafe { a20_res_ns_unshare(TASK, 0) }, 0);
        assert!(events().is_empty(), "{:?}", events());
    }

    #[test]
    fn a_failed_allocation_releases_only_what_was_created() {
        reset();
        FAIL_NEW.with(|f| *f.borrow_mut() = Some(PROCNS_NET));
        let flags = CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWIPC;
        assert_eq!(unsafe { a20_res_ns_unshare(TASK, flags) }, -ENOMEM);
        let events = events();
        // UTS was created and must be released; nothing was published.
        assert_eq!(
            events
                .iter()
                .filter(|e| matches!(e, Event::New(PROCNS_UTS, _)))
                .count(),
            1
        );
        assert_eq!(events.iter().filter(|e| **e == Event::Put(PROCNS_UTS)).count(), 1);
        assert!(
            !events.iter().any(|e| matches!(e, Event::Join(..))),
            "a failed prepare must not publish"
        );
    }

    #[test]
    fn inode_numbers_advance_across_namespaces() {
        reset();
        let flags = CLONE_NEWUTS | CLONE_NEWIPC;
        assert_eq!(unsafe { a20_res_ns_unshare(TASK, flags) }, 0);
        let inos: Vec<u64> = events()
            .into_iter()
            .filter_map(|e| match e {
                Event::New(_, ino) => Some(ino),
                _ => None,
            })
            .collect();
        assert_eq!(inos.len(), 2);
        assert!(inos[0] >= FIRST_INO && inos[1] > inos[0], "{inos:?}");
    }

    #[test]
    fn fork_copies_every_membership_then_applies_the_flags() {
        reset();
        assert_eq!(
            unsafe { a20_res_ns_fork(TASK, TASK, CLONE_NEWNET) },
            0
        );
        let events = events();
        let slots: Vec<_> = events
            .iter()
            .filter_map(|e| match e {
                Event::Slot(kind, ns) if *ns != 0 => Some(*kind),
                _ => None,
            })
            .collect();
        assert_eq!(slots, [PROCNS_UTS, PROCNS_IPC, PROCNS_NET, PROCNS_CGROUP]);
        // Only NET was re-created by unshare.
        let created: Vec<_> = events
            .iter()
            .filter_map(|e| match e {
                Event::New(kind, _) => Some(*kind),
                _ => None,
            })
            .collect();
        assert_eq!(created, [PROCNS_NET]);
    }

    #[test]
    fn a_failed_fork_unshare_releases_the_child_memberships() {
        reset();
        FAIL_NEW.with(|f| *f.borrow_mut() = Some(PROCNS_NET));
        assert_eq!(
            unsafe { a20_res_ns_fork(TASK, TASK, CLONE_NEWNET) },
            -ENOMEM
        );
        let events = events();
        assert!(
            events.contains(&Event::ReleaseTask),
            "a failed fork must release the inherited memberships: {events:?}"
        );
    }

    #[test]
    fn a_null_child_is_rejected() {
        reset();
        assert_eq!(unsafe { a20_res_ns_fork(ptr::null_mut(), TASK, 0) }, -EINVAL);
        assert_eq!(unsafe { a20_res_ns_unshare(ptr::null_mut(), CLONE_NEWUTS) }, -EINVAL);
        assert!(events().is_empty());
    }
}
