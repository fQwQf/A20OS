#![no_std]

//! Ordered process-exit transaction. Rust selects the irreversible stage order;
//! C adapters retain the task, MM, lock, ABI, and notification primitives.

#[cfg(test)]
extern crate std;

pub const EXIT_BEGIN: u32 = 0;
pub const EXIT_CLEAR_TID: u32 = 1;
pub const EXIT_ROBUST_LIST: u32 = 2;
pub const EXIT_RELEASE_LOCKS: u32 = 3;
pub const EXIT_ACCOUNT: u32 = 4;
pub const EXIT_CLOSE_FILES: u32 = 5;
pub const EXIT_RELEASE_MM: u32 = 6;
pub const EXIT_DEBUG_STOP: u32 = 7;
pub const EXIT_RESOLVE_PARENT: u32 = 8;
pub const EXIT_PUBLISH_ZOMBIE: u32 = 9;
pub const EXIT_PARENT_BOOKKEEPING: u32 = 10;
pub const EXIT_TASK_SERVICES: u32 = 11;
pub const EXIT_REPARENT_CHILDREN: u32 = 12;
pub const EXIT_TRACER_DETACH: u32 = 13;
pub const EXIT_NOTIFY_PARENT: u32 = 14;
pub const EXIT_SCHEDULE: u32 = 15;

pub type StageFn = unsafe extern "C" fn(*mut core::ffi::c_void);

#[repr(C)]
pub struct TaskExitOps {
    pub context: *mut core::ffi::c_void,
    pub begin: StageFn,
    pub clear_tid: StageFn,
    pub robust_list: StageFn,
    pub release_locks: StageFn,
    pub account: StageFn,
    pub close_files: StageFn,
    pub release_mm: StageFn,
    pub debug_stop: StageFn,
    pub resolve_parent: StageFn,
    pub publish_zombie: StageFn,
    pub parent_bookkeeping: StageFn,
    pub task_services: StageFn,
    pub reparent_children: StageFn,
    pub tracer_detach: StageFn,
    pub notify_parent: StageFn,
    pub schedule: StageFn,
}

const EXIT_ORDER: [u32; 16] = [
    EXIT_BEGIN,
    EXIT_CLEAR_TID,
    EXIT_ROBUST_LIST,
    EXIT_RELEASE_LOCKS,
    EXIT_ACCOUNT,
    EXIT_CLOSE_FILES,
    EXIT_RELEASE_MM,
    EXIT_DEBUG_STOP,
    EXIT_RESOLVE_PARENT,
    EXIT_PUBLISH_ZOMBIE,
    EXIT_PARENT_BOOKKEEPING,
    EXIT_TASK_SERVICES,
    EXIT_REPARENT_CHILDREN,
    EXIT_TRACER_DETACH,
    EXIT_NOTIFY_PARENT,
    EXIT_SCHEDULE,
];

fn dispatch(ops: &TaskExitOps, stage: u32) {
    let callback = match stage {
        EXIT_BEGIN => ops.begin,
        EXIT_CLEAR_TID => ops.clear_tid,
        EXIT_ROBUST_LIST => ops.robust_list,
        EXIT_RELEASE_LOCKS => ops.release_locks,
        EXIT_ACCOUNT => ops.account,
        EXIT_CLOSE_FILES => ops.close_files,
        EXIT_RELEASE_MM => ops.release_mm,
        EXIT_DEBUG_STOP => ops.debug_stop,
        EXIT_RESOLVE_PARENT => ops.resolve_parent,
        EXIT_PUBLISH_ZOMBIE => ops.publish_zombie,
        EXIT_PARENT_BOOKKEEPING => ops.parent_bookkeeping,
        EXIT_TASK_SERVICES => ops.task_services,
        EXIT_REPARENT_CHILDREN => ops.reparent_children,
        EXIT_TRACER_DETACH => ops.tracer_detach,
        EXIT_NOTIFY_PARENT => ops.notify_parent,
        EXIT_SCHEDULE => ops.schedule,
        // The only callers are the fixed EXIT_ORDER table above. Keeping an
        // inert fallback avoids pulling an unavailable panic runtime into the
        // freestanding kernel if that table is changed incorrectly.
        _ => return,
    };
    unsafe { callback(ops.context) }
}

fn drive_exit<F: FnMut(u32)>(mut run: F) {
    for stage in EXIT_ORDER {
        run(stage);
    }
}

/// Execute the non-returning process-exit transaction.
///
/// # Safety
/// `ops` and its context must remain valid for this call. Every function
/// pointer must be non-null and a valid callback with the declared ABI. The C
/// adapter constructs a complete table before entering Rust. Callbacks run
/// synchronously and must not return to user mode. The final schedule callback
/// is expected not to return; the C caller panics if it does.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_task_exit_run(ops: *const TaskExitOps) {
    if ops.is_null() {
        return;
    }
    let ops = unsafe { &*ops };
    drive_exit(|stage| dispatch(ops, stage));
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;

    #[test]
    fn exit_releases_resources_before_zombie_and_notifies_after_reparenting() {
        let mut seen = Vec::new();
        drive_exit(|stage| seen.push(stage));
        assert_eq!(seen, EXIT_ORDER);
        assert!(seen.iter().position(|&s| s == EXIT_CLOSE_FILES).unwrap()
            < seen.iter().position(|&s| s == EXIT_PUBLISH_ZOMBIE).unwrap());
        assert!(seen.iter().position(|&s| s == EXIT_PUBLISH_ZOMBIE).unwrap()
            < seen.iter().position(|&s| s == EXIT_REPARENT_CHILDREN).unwrap());
        assert_eq!(seen.last(), Some(&EXIT_SCHEDULE));
    }

    #[test]
    fn vfork_completion_is_resolved_before_zombie_publish() {
        let resolve = EXIT_ORDER.iter().position(|&s| s == EXIT_RESOLVE_PARENT).unwrap();
        let zombie = EXIT_ORDER.iter().position(|&s| s == EXIT_PUBLISH_ZOMBIE).unwrap();
        assert!(resolve < zombie);
    }
}

pub type ReleaseFn = unsafe extern "C" fn(*mut core::ffi::c_void);

#[repr(C)]
pub struct TaskReleaseOps {
    pub context: *mut core::ffi::c_void,
    pub cgroup: ReleaseFn,
    pub udriver: ReleaseFn,
    pub process_locks: ReleaseFn,
    pub files: ReleaseFn,
    pub mm: ReleaseFn,
    pub arch: ReleaseFn,
    pub keyring: ReleaseFn,
    pub landlock: ReleaseFn,
    pub envelope: ReleaseFn,
    pub thread_signal_info: ReleaseFn,
    pub signal_state: ReleaseFn,
    pub scratch: ReleaseFn,
    pub handle_table: ReleaseFn,
    pub nommu_vfork: ReleaseFn,
    pub kernel_stack: ReleaseFn,
    pub thread_leader: ReleaseFn,
    pub task_object: ReleaseFn,
    pub pid: ReleaseFn,
    pub free_task: ReleaseFn,
}

const RELEASE_ORDER: [usize; 19] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18];

fn drive_release<F: FnMut(usize)>(mut run: F) {
    for stage in RELEASE_ORDER {
        run(stage);
    }
}

/// Atomically drops one task reference, returning the previous count. The
/// C-only branch retains its equivalent compare/exchange loop.
///
/// # Safety
/// `value` must be a valid, aligned task refcount shared only through atomic
/// accesses for the duration of this operation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_task_ref_dec(value: *mut i32) -> i32 {
    if value.is_null() {
        return -1;
    }
    let refs = unsafe { &*(value.cast::<core::sync::atomic::AtomicI32>()) };
    let mut old = refs.load(core::sync::atomic::Ordering::Relaxed);
    loop {
        if old <= 0 {
            return old;
        }
        match refs.compare_exchange_weak(
            old,
            old - 1,
            core::sync::atomic::Ordering::AcqRel,
            core::sync::atomic::Ordering::Relaxed,
        ) {
            Ok(_) => return old,
            Err(actual) => old = actual,
        }
    }
}

/// Run final task-resource teardown in dependency order.
///
/// # Safety
/// `ops`, its context, and every callback (all of which must be non-null)
/// must remain valid throughout the synchronous call. The C adapter constructs
/// the complete table before entry. In particular, `free_task` runs last; no
/// callback may access the task after freeing it, and the final callback must
/// not access the task or callback table after releasing them.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_task_release_run(ops: *const TaskReleaseOps) {
    if ops.is_null() {
        return;
    }
    let ops = unsafe { &*ops };
    drive_release(|stage| {
        let callback = match stage {
            0 => ops.cgroup,
            1 => ops.udriver,
            2 => ops.process_locks,
            3 => ops.files,
            4 => ops.mm,
            5 => ops.arch,
            6 => ops.keyring,
            7 => ops.landlock,
            8 => ops.envelope,
            9 => ops.thread_signal_info,
            10 => ops.signal_state,
            11 => ops.scratch,
            12 => ops.handle_table,
            13 => ops.nommu_vfork,
            14 => ops.kernel_stack,
            15 => ops.thread_leader,
            16 => ops.task_object,
            17 => ops.pid,
            18 => ops.free_task,
            _ => return,
        };
        unsafe { callback(ops.context) }
    });
}

#[cfg(test)]
mod release_tests {
    use super::*;
    use std::sync::atomic::{AtomicI32, Ordering};
    use std::vec::Vec;

    #[test]
    fn reference_drop_preserves_nonfinal_and_final_boundary() {
        let refs = AtomicI32::new(2);
        assert_eq!(unsafe { a20_task_ref_dec(refs.as_ptr() as *mut i32) }, 2);
        assert_eq!(refs.load(Ordering::Relaxed), 1);
        assert_eq!(unsafe { a20_task_ref_dec(refs.as_ptr() as *mut i32) }, 1);
        assert_eq!(refs.load(Ordering::Relaxed), 0);
        assert_eq!(unsafe { a20_task_ref_dec(refs.as_ptr() as *mut i32) }, 0);
        assert_eq!(refs.load(Ordering::Relaxed), 0);
    }

    #[test]
    fn concurrent_final_reference_is_claimed_exactly_once() {
        use std::sync::Arc;
        use std::thread;

        const THREADS: usize = 8;
        const DROPS_PER_THREAD: usize = 128;
        let refs = Arc::new(AtomicI32::new((THREADS * DROPS_PER_THREAD) as i32));
        let final_drops = Arc::new(AtomicI32::new(0));
        let mut workers = Vec::new();
        for _ in 0..THREADS {
            let refs = Arc::clone(&refs);
            let final_drops = Arc::clone(&final_drops);
            workers.push(thread::spawn(move || {
                let ptr = refs.as_ptr() as *mut i32;
                for _ in 0..DROPS_PER_THREAD {
                    if unsafe { a20_task_ref_dec(ptr) } == 1 {
                        final_drops.fetch_add(1, Ordering::Relaxed);
                    }
                }
            }));
        }
        for worker in workers {
            worker.join().unwrap();
        }
        assert_eq!(refs.load(Ordering::Relaxed), 0);
        assert_eq!(final_drops.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn final_release_frees_only_after_all_references() {
        let mut seen = Vec::new();
        drive_release(|stage| seen.push(stage));
        assert_eq!(seen, RELEASE_ORDER);
        assert_eq!(seen[seen.len() - 1], 18);
    }
}
