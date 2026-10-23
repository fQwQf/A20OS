#![no_std]

//! Fork/clone transaction policy.
//!
//! Rust owns the transaction behind `proc_clone()`: the inheritance decision
//! each stage makes, the publication boundary, the CLONE_VFORK arming and its
//! rollback, and the deferred-ready handshake.  C keeps the mechanical
//! operations it already owns -- task/pid allocation, fd and MM object copies,
//! namespace forks, list linking under `tasklist_lock`, architecture frame
//! construction and usercopy -- and exposes them through the bridges declared
//! in `kernel/proc/clone_txn.h`.
//!
//! Task fields are read and written through `CloneView`, a borrowed view over
//! the live `task_t`, so no task state is duplicated here and the task layout
//! does not change.  The transaction context lives on the caller's kernel
//! stack: `proc_clone()` runs the whole transaction synchronously, so
//! concurrent clones on other CPUs have their own context.

#[cfg(test)]
extern crate std;

include!("task_construction_ops.rs");

pub const CLONE_VM: u64 = 0x0000_0100;
pub const CLONE_FILES: u64 = 0x0000_0400;
pub const CLONE_SIGHAND: u64 = 0x0000_0800;
pub const CLONE_VFORK: u64 = 0x0000_4000;
pub const CLONE_PARENT: u64 = 0x0000_8000;
pub const CLONE_THREAD: u64 = 0x0001_0000;
pub const CLONE_NEWNS: u64 = 0x0002_0000;
pub const CLONE_SETTLS: u64 = 0x0008_0000;
pub const CLONE_PARENT_SETTID: u64 = 0x0010_0000;
pub const CLONE_CHILD_CLEARTID: u64 = 0x0020_0000;
pub const CLONE_CHILD_SETTID: u64 = 0x0100_0000;

pub const NSIG: i32 = 64;
pub const CAP_SYS_RESOURCE: i32 = 24;
pub const SCHED_NORMAL: i32 = 0;
pub const SCHED_FIFO: i32 = 1;
pub const SCHED_RR: i32 = 2;
pub const PROC_UNUSED: i32 = 0;
pub const PROC_ZOMBIE: i32 = 5;

const ESRCH: i32 = -3;
const EAGAIN: i32 = -11;
const ENOMEM: i32 = -12;
const EFAULT: i32 = -14;
const EINVAL: i32 = -22;

/// Borrowed view over the live `task_t` fields the transaction touches.
/// `kernel/proc/clone_txn.h` holds the byte-compatible C mirror.
#[repr(C)]
pub struct CloneView {
    pub pid: *mut i32,
    pub tgid: *mut i32,
    pub ppid: *mut i32,
    pub parent: *mut *mut core::ffi::c_void,
    pub tg_leader: *mut *mut core::ffi::c_void,
    pub abi_mode: *mut u32,
    pub exit_signal: *mut i32,
    pub clone_flags: *mut i32,
    pub clear_child_tid: *mut *mut i32,
    pub sched_policy: *mut i32,
    pub priority: *mut i32,
    pub cfs_weight: *mut u32,
    pub exec_load_addr: *mut usize,
    pub exec_load_size: *mut usize,
    pub name: *mut u8,
    pub sched_reset_on_fork: *const i32,
    pub nproc_limit: *const u64,
}

/// One clone transaction.  Owned by `a20_proc_clone_impl`'s stack frame.
pub struct CloneTxn {
    flags: u64,
    stack: usize,
    ptid: *mut i32,
    tls: usize,
    ctid: *mut i32,
    exit_signal: i32,
    out_task: *mut *mut core::ffi::c_void,
    parent: *mut core::ffi::c_void,
    task: *mut core::ffi::c_void,
    vfork_child_ref: *mut core::ffi::c_void,
    child_pid: i32,
    parent_vfork_armed: bool,
    parent_view: CloneView,
    child_view: CloneView,
}

impl CloneTxn {
    const fn empty_view() -> CloneView {
        CloneView {
            pid: core::ptr::null_mut(),
            tgid: core::ptr::null_mut(),
            ppid: core::ptr::null_mut(),
            parent: core::ptr::null_mut(),
            tg_leader: core::ptr::null_mut(),
            abi_mode: core::ptr::null_mut(),
            exit_signal: core::ptr::null_mut(),
            clone_flags: core::ptr::null_mut(),
            clear_child_tid: core::ptr::null_mut(),
            sched_policy: core::ptr::null_mut(),
            priority: core::ptr::null_mut(),
            cfs_weight: core::ptr::null_mut(),
            exec_load_addr: core::ptr::null_mut(),
            exec_load_size: core::ptr::null_mut(),
            name: core::ptr::null_mut(),
            sched_reset_on_fork: core::ptr::null(),
            nproc_limit: core::ptr::null(),
        }
    }

    fn new(flags: u64, stack: usize, ptid: *mut i32, tls: usize, ctid: *mut i32,
           exit_signal: i32, out_task: *mut *mut core::ffi::c_void) -> Self {
        CloneTxn {
            flags,
            stack,
            ptid,
            tls,
            ctid,
            exit_signal,
            out_task,
            parent: core::ptr::null_mut(),
            task: core::ptr::null_mut(),
            vfork_child_ref: core::ptr::null_mut(),
            child_pid: 0,
            parent_vfork_armed: false,
            parent_view: Self::empty_view(),
            child_view: Self::empty_view(),
        }
    }
}

extern "C" {
    fn a20_clone_view_fill(t: *mut core::ffi::c_void, out: *mut CloneView);
    fn a20_clone_task_pid(t: *const core::ffi::c_void) -> i32;
    fn a20_clone_task_uid(t: *const core::ffi::c_void) -> i32;
    fn a20_clone_flags_supported(flags: u64) -> i32;
    fn a20_clone_has_cap(t: *const core::ffi::c_void, cap: i32) -> i32;
    fn a20_clone_weight_for_nice(nice: i32) -> u32;
    fn a20_clone_notify_begin(parent_pid: i32, flags: u64);
    fn a20_clone_notify_ready(t: *mut core::ffi::c_void, child_pid: i32, flags: u64);
    fn a20_clone_inherit_stdio(t: *mut core::ffi::c_void, parent: *mut core::ffi::c_void);
    fn a20_clone_inherit_capabilities(t: *mut core::ffi::c_void,
                                      parent: *mut core::ffi::c_void, flags: u64);
    fn a20_clone_init_mm(t: *mut core::ffi::c_void, parent: *mut core::ffi::c_void,
                         flags: u64, child_pid: i32) -> i32;
    fn a20_clone_init_arch(t: *mut core::ffi::c_void, parent: *mut core::ffi::c_void,
                           flags: u64, stack: usize, tls: usize) -> i32;
    fn a20_clone_tasklist_lock() -> u64;
    fn a20_clone_tasklist_unlock(flags: u64);
    fn a20_clone_vfork_snapshot_create(parent: *mut core::ffi::c_void) -> i32;
    fn a20_clone_vfork_arm(parent: *mut core::ffi::c_void);
    fn a20_clone_vfork_disarm(parent: *mut core::ffi::c_void);
    fn a20_clone_vfork_wait(child: *mut core::ffi::c_void);

    fn proc_current() -> *mut core::ffi::c_void;
    fn proc_alloc_task_slot() -> *mut core::ffi::c_void;
    fn proc_pid_alloc() -> i32;
    fn proc_task_init_base(t: *mut core::ffi::c_void, parent: *const core::ffi::c_void);
    fn proc_task_init_seccomp(t: *mut core::ffi::c_void, parent: *const core::ffi::c_void);
    fn proc_task_init_fs_cgroup(t: *mut core::ffi::c_void, parent: *const core::ffi::c_void);
    fn proc_task_init_signals(t: *mut core::ffi::c_void, parent: *const core::ffi::c_void,
                              flags: u64, checked: i32) -> i32;
    fn proc_set_name(t: *mut core::ffi::c_void, name: *const u8);
    fn proc_publish_task_slot_locked(t: *mut core::ffi::c_void) -> i32;
    fn proc_tg_link_locked(t: *mut core::ffi::c_void) -> i32;
    fn proc_children_link_locked(parent: *mut core::ffi::c_void, child: *mut core::ffi::c_void);
    fn proc_pid_register(t: *mut core::ffi::c_void);
    fn proc_make_ready(t: *mut core::ffi::c_void);
    fn proc_first_task_locked() -> *mut core::ffi::c_void;
    fn proc_next_task_locked(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn proc_task_state_get(t: *mut core::ffi::c_void) -> i32;
    fn proc_get(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn proc_put(t: *mut core::ffi::c_void);
    fn proc_destroy_task(t: *mut core::ffi::c_void);
    fn fdtable_share(dst: *mut core::ffi::c_void, src: *const core::ffi::c_void);
    fn fdtable_copy_checked(dst: *mut core::ffi::c_void, src: *const core::ffi::c_void) -> i32;
    fn mntns_fork(child: *mut core::ffi::c_void, parent: *const core::ffi::c_void,
                  clone_flags: u64) -> i32;
    fn pidns_fork(child: *mut core::ffi::c_void, parent: *const core::ffi::c_void,
                  clone_flags: u64) -> i32;
    fn userns_fork(child: *mut core::ffi::c_void, parent: *const core::ffi::c_void,
                   clone_flags: u64) -> i32;
    fn resource_ns_fork(child: *mut core::ffi::c_void, parent: *const core::ffi::c_void,
                        flags: u64) -> i32;
    fn copy_to_user(dst: *mut core::ffi::c_void, src: *const core::ffi::c_void,
                    n: usize) -> isize;
    fn proc_copy_to_task_user(task: *mut core::ffi::c_void, dst: *mut core::ffi::c_void,
                              src: *const core::ffi::c_void, n: usize) -> i32;

    fn a20_task_construction_run(ops: *const TaskConstructionOps) -> i32;
}

/// RLIMIT_NPROC is scoped to the caller's real uid rather than per-process, so
/// a fork bomb from one account is capped no matter how many processes it
/// already has.  The walk itself holds `tasklist_lock`; each task's state is
/// sampled through its own park_lock, one task at a time.
unsafe fn count_tasks_for_uid(uid: i32) -> u32 {
    let mut n: u32 = 0;
    let flags = unsafe { a20_clone_tasklist_lock() };
    let mut t = unsafe { proc_first_task_locked() };
    while !t.is_null() {
        let st = unsafe { proc_task_state_get(t) };
        if st != PROC_UNUSED && st != PROC_ZOMBIE && unsafe { a20_clone_task_uid(t) } == uid {
            n += 1;
        }
        t = unsafe { proc_next_task_locked(t) };
    }
    unsafe { a20_clone_tasklist_unlock(flags) };
    n
}

unsafe extern "C" fn stage_validate(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    tx.parent = unsafe { proc_current() };
    if tx.parent.is_null() {
        return ESRCH;
    }
    let parent_pid = unsafe { a20_clone_task_pid(tx.parent) };
    unsafe { a20_clone_notify_begin(parent_pid, tx.flags) };
    let rc = unsafe { a20_clone_flags_supported(tx.flags) };
    if rc < 0 {
        return rc;
    }
    unsafe { a20_clone_view_fill(tx.parent, &mut tx.parent_view) };
    let limit = unsafe { *tx.parent_view.nproc_limit };
    if limit != 0 && unsafe { a20_clone_has_cap(tx.parent, CAP_SYS_RESOURCE) } == 0 {
        let uid = unsafe { a20_clone_task_uid(tx.parent) };
        if u64::from(unsafe { count_tasks_for_uid(uid) }) >= limit {
            return EAGAIN;
        }
    }
    if tx.exit_signal < 0 || tx.exit_signal >= NSIG {
        return EINVAL;
    }
    0
}

unsafe extern "C" fn stage_allocate(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    tx.task = unsafe { proc_alloc_task_slot() };
    if tx.task.is_null() {
        return EAGAIN;
    }
    unsafe { a20_clone_view_fill(tx.task, &mut tx.child_view) };
    let pid = unsafe { proc_pid_alloc() };
    unsafe { *tx.child_view.pid = pid };
    if pid < 0 {
        return EAGAIN;
    }
    tx.child_pid = pid;
    0
}

unsafe extern "C" fn stage_base(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    let t = tx.task;
    let parent = tx.parent;
    unsafe { proc_task_init_base(t, parent) };
    unsafe { a20_clone_view_fill(t, &mut tx.child_view) };
    let child = &tx.child_view;
    let pv = &tx.parent_view;
    unsafe { *child.abi_mode = if parent.is_null() { 0 } else { *pv.abi_mode } };
    unsafe { a20_clone_inherit_stdio(t, parent) };
    if !parent.is_null() && unsafe { *pv.sched_reset_on_fork } != 0 {
        let parent_policy = unsafe { *pv.sched_policy };
        if parent_policy == SCHED_FIFO || parent_policy == SCHED_RR {
            unsafe { *child.sched_policy = SCHED_NORMAL };
            unsafe { *child.priority = 0 };
            unsafe { *child.cfs_weight = a20_clone_weight_for_nice(0) };
        } else if unsafe { *child.priority } < 0 {
            unsafe { *child.priority = 0 };
            unsafe { *child.cfs_weight = a20_clone_weight_for_nice(0) };
        }
    }
    let child_signal = if tx.flags & CLONE_THREAD != 0 { 0 } else { tx.exit_signal };
    unsafe { *child.exit_signal = child_signal };
    if tx.flags & CLONE_PARENT != 0 && !parent.is_null() {
        let grandparent = unsafe { *pv.parent };
        if !grandparent.is_null() {
            unsafe { *child.parent = grandparent };
            unsafe { *child.ppid = a20_clone_task_pid(grandparent) };
        }
    }
    if tx.flags & CLONE_THREAD != 0 {
        let tgid = if parent.is_null() { unsafe { *child.pid } } else { unsafe { *pv.tgid } };
        unsafe { *child.tgid = tgid };
    }
    unsafe { *child.clone_flags = tx.flags as i32 };
    if tx.flags & CLONE_CHILD_CLEARTID != 0 {
        unsafe { *child.clear_child_tid = tx.ctid };
    }
    unsafe { proc_set_name(t, pv.name) };
    unsafe { *child.exec_load_addr = *pv.exec_load_addr };
    unsafe { *child.exec_load_size = *pv.exec_load_size };
    0
}

unsafe extern "C" fn stage_seccomp(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    unsafe { proc_task_init_seccomp(tx.task, tx.parent) };
    0
}

unsafe extern "C" fn stage_fs_cgroup(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    unsafe { proc_task_init_fs_cgroup(tx.task, tx.parent) };
    0
}

unsafe extern "C" fn stage_files(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    if tx.parent.is_null() {
        return 0;
    }
    if tx.flags & (CLONE_FILES | CLONE_THREAD) != 0 {
        unsafe { fdtable_share(tx.task, tx.parent) };
        return 0;
    }
    unsafe { fdtable_copy_checked(tx.task, tx.parent) }
}

unsafe extern "C" fn stage_signals(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    unsafe { proc_task_init_signals(tx.task, tx.parent, tx.flags, 1) }
}

unsafe extern "C" fn stage_namespaces(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    let t = tx.task;
    let parent = tx.parent;
    let flags = tx.flags;
    if unsafe { mntns_fork(t, parent, flags) } < 0 {
        return ENOMEM;
    }
    let rc = unsafe { pidns_fork(t, parent, flags) };
    if rc < 0 {
        return rc;
    }
    let rc = unsafe { userns_fork(t, parent, flags) };
    if rc < 0 {
        return rc;
    }
    unsafe { resource_ns_fork(t, parent, flags) }
}

unsafe extern "C" fn stage_capabilities(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    unsafe { a20_clone_inherit_capabilities(tx.task, tx.parent, tx.flags) };
    0
}

unsafe extern "C" fn stage_mm(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    unsafe { a20_clone_init_mm(tx.task, tx.parent, tx.flags, tx.child_pid) }
}

unsafe extern "C" fn stage_arch(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    unsafe { a20_clone_init_arch(tx.task, tx.parent, tx.flags, tx.stack, tx.tls) }
}

/// The publish boundary.  `proc_publish_task_slot_locked` links the global task
/// slot first, so a later failure while linking the group or the child chain
/// leaves a partial publication prefix that rollback must handle.
unsafe extern "C" fn stage_publish(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    let t = tx.task;
    let parent = tx.parent;
    let child = &tx.child_view;
    let flags = tx.flags;
    let mut rc: i32;
    let list_flags = unsafe { a20_clone_tasklist_lock() };
    rc = unsafe { proc_publish_task_slot_locked(t) };
    if rc == 0 {
        let leader = if flags & CLONE_THREAD != 0 {
            if !parent.is_null() {
                let parent_leader = unsafe { *tx.parent_view.tg_leader };
                if parent_leader.is_null() { parent } else { parent_leader }
            } else {
                t
            }
        } else {
            t
        };
        unsafe { *child.tg_leader = leader };
        if leader != t {
            rc = unsafe { proc_tg_link_locked(t) };
        }
    }
    if rc == 0 {
        let effective_parent = unsafe { *child.parent };
        unsafe { proc_children_link_locked(effective_parent, t) };
    }
    unsafe { a20_clone_tasklist_unlock(list_flags) };
    if rc < 0 {
        return rc;
    }
    unsafe { proc_pid_register(t) };
    0
}

unsafe extern "C" fn stage_post_publish(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    let t = tx.task;
    let tid: i32 = unsafe { *tx.child_view.pid };
    if tx.flags & CLONE_PARENT_SETTID != 0 && !tx.ptid.is_null() {
        let src = &tid as *const i32 as *const core::ffi::c_void;
        if unsafe { copy_to_user(tx.ptid as *mut core::ffi::c_void, src, 4) } < 0 {
            return EFAULT;
        }
    }
    if tx.flags & CLONE_CHILD_SETTID != 0 && !tx.ctid.is_null() {
        let src = &tid as *const i32 as *const core::ffi::c_void;
        let rc = unsafe { proc_copy_to_task_user(t, tx.ctid as *mut core::ffi::c_void, src, 4) };
        if rc < 0 {
            return EFAULT;
        }
    }
    if tx.flags & CLONE_VFORK != 0 {
        let rc = unsafe { a20_clone_vfork_snapshot_create(tx.parent) };
        if rc < 0 {
            return rc;
        }
        unsafe { a20_clone_vfork_arm(tx.parent) };
        tx.parent_vfork_armed = true;
        tx.vfork_child_ref = unsafe { proc_get(t) };
        if tx.vfork_child_ref.is_null() {
            return ESRCH;
        }
    }
    if !tx.out_task.is_null() {
        unsafe { *tx.out_task = t };
    }
    0
}

unsafe extern "C" fn stage_ready(ctx: *mut core::ffi::c_void) -> i32 {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    unsafe { proc_make_ready(tx.task) };
    unsafe { a20_clone_notify_ready(tx.task, tx.child_pid, tx.flags) };
    0
}

unsafe extern "C" fn stage_vfork_wait(ctx: *mut core::ffi::c_void) {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    if tx.flags & CLONE_VFORK == 0 {
        return;
    }
    unsafe { a20_clone_vfork_wait(tx.vfork_child_ref) };
    tx.vfork_child_ref = core::ptr::null_mut();
}

unsafe extern "C" fn stage_abort(ctx: *mut core::ffi::c_void, _publication_attempted: u8) {
    let tx = unsafe { &mut *(ctx as *mut CloneTxn) };
    if tx.task.is_null() {
        return;
    }
    if tx.parent_vfork_armed {
        unsafe { a20_clone_vfork_disarm(tx.parent) };
        tx.parent_vfork_armed = false;
    }
    if !tx.vfork_child_ref.is_null() {
        unsafe { proc_put(tx.vfork_child_ref) };
        tx.vfork_child_ref = core::ptr::null_mut();
    }
    unsafe { proc_destroy_task(tx.task) };
    tx.task = core::ptr::null_mut();
}

/// Build the runner's operation table for one transaction.  The table layout
/// comes from `task_construction_ops.rs`, the same file the runner includes.
fn build_ops(tx: &mut CloneTxn) -> TaskConstructionOps {
    TaskConstructionOps {
        context: tx as *mut CloneTxn as *mut core::ffi::c_void,
        validate: stage_validate,
        allocate: stage_allocate,
        base: stage_base,
        seccomp: stage_seccomp,
        fs_cgroup: stage_fs_cgroup,
        files: stage_files,
        signals: stage_signals,
        namespaces: stage_namespaces,
        capabilities: stage_capabilities,
        mm: stage_mm,
        arch: stage_arch,
        publish: stage_publish,
        post_publish: stage_post_publish,
        ready: stage_ready,
        vfork_wait: stage_vfork_wait,
        abort: stage_abort,
        deferred: u8::from(!tx.out_task.is_null()),
        vfork: u8::from(tx.flags & CLONE_VFORK != 0),
    }
}

/// Run one fork/clone transaction and return the child pid or a negative errno.
///
/// # Safety
/// The caller must be a task context with no transaction-local state on the
/// stack, and every pointer argument must be valid for the duration of the
/// call, exactly as the previous C `proc_clone()` required.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_proc_clone_impl(flags: u64, stack: usize, ptid: *mut i32,
                                             tls: usize, ctid: *mut i32, exit_signal: i32,
                                             out_task: *mut *mut core::ffi::c_void) -> i32 {
    let mut tx = CloneTxn::new(flags, stack, ptid, tls, ctid, exit_signal, out_task);
    let ops = build_ops(&mut tx);
    let rc = unsafe { a20_task_construction_run(&ops) };
    if rc < 0 { rc } else { tx.child_pid }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    /// The fixture is global because the C bridges are plain C symbols; one
    /// test at a time touches it.
    static TEST_LOCK: Mutex<()> = Mutex::new(());

    #[derive(Clone, Copy)]
    struct FakeTask {
        pid: i32,
        tgid: i32,
        ppid: i32,
        parent: *mut FakeTask,
        tg_leader: *mut FakeTask,
        abi_mode: u32,
        exit_signal: i32,
        clone_flags: i32,
        clear_child_tid: *mut i32,
        sched_policy: i32,
        priority: i32,
        cfs_weight: u32,
        exec_load_addr: usize,
        exec_load_size: usize,
        name: [u8; 64],
        sched_reset_on_fork: i32,
        nproc_limit: u64,
        uid: i32,
    }

    impl FakeTask {
        const fn new() -> Self {
            FakeTask {
                pid: 0,
                tgid: 0,
                ppid: 0,
                parent: core::ptr::null_mut(),
                tg_leader: core::ptr::null_mut(),
                abi_mode: 0,
                exit_signal: 0,
                clone_flags: 0,
                clear_child_tid: core::ptr::null_mut(),
                sched_policy: 0,
                priority: 0,
                cfs_weight: 0,
                exec_load_addr: 0,
                exec_load_size: 0,
                name: [0u8; 64],
                sched_reset_on_fork: 0,
                nproc_limit: 0,
                uid: 0,
            }
        }
    }

    #[derive(Clone, Copy)]
    struct Fixture {
        parent: FakeTask,
        child: FakeTask,
        grandparent: FakeTask,
        /// Tasks the RLIMIT_NPROC walk reports, as (state, uid).
        walk: [(i32, i32); 4],
        walk_len: usize,
        walk_index: usize,
        current_is_parent: bool,
        pid_alloc: i32,
        task_alloc_ok: bool,
        flags_supported: i32,
        has_cap: i32,
        weight: u32,
        publish_rc: i32,
        tg_link_rc: i32,
        mntns_rc: i32,
        pidns_rc: i32,
        userns_rc: i32,
        resource_ns_rc: i32,
        fd_copy_rc: i32,
        copy_to_user_rc: isize,
        copy_to_task_rc: i32,
        snapshot_rc: i32,
        get_ok: bool,
        runner_rc: i32,
        copy_user_dst: i32,
        copy_child_dst: i32,
        log: [&'static str; 40],
        log_len: usize,
    }

    impl Fixture {
        const fn new() -> Self {
            Fixture {
                parent: FakeTask::new(),
                child: FakeTask::new(),
                grandparent: FakeTask::new(),
                walk: [(0, 0); 4],
                walk_len: 0,
                walk_index: 0,
                current_is_parent: true,
                pid_alloc: 4242,
                task_alloc_ok: true,
                flags_supported: 0,
                has_cap: 0,
                weight: 1000,
                publish_rc: 0,
                tg_link_rc: 0,
                mntns_rc: 0,
                pidns_rc: 0,
                userns_rc: 0,
                resource_ns_rc: 0,
                fd_copy_rc: 0,
                copy_to_user_rc: 0,
                copy_to_task_rc: 0,
                snapshot_rc: 0,
                get_ok: true,
                runner_rc: -22,
                copy_user_dst: 0,
                copy_child_dst: 0,
                log: [""; 40],
                log_len: 0,
            }
        }

        fn note(&mut self, what: &'static str) {
            if self.log_len < self.log.len() {
                self.log[self.log_len] = what;
                self.log_len += 1;
            }
        }

        fn calls(&self) -> &[&'static str] {
            &self.log[..self.log_len]
        }
    }

    static mut FIX: Fixture = Fixture::new();

    fn fix() -> &'static mut Fixture {
        unsafe { &mut *core::ptr::addr_of_mut!(FIX) }
    }

    /// Reset the fixture and return the lock guard for one test.
    fn reset() -> std::sync::MutexGuard<'static, ()> {
        let guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let f = fix();
        let runner_rc = f.runner_rc;
        *f = Fixture::new();
        f.runner_rc = runner_rc;
        f.parent.pid = 100;
        f.parent.tgid = 100;
        f.parent.uid = 7;
        f.parent.nproc_limit = 0;
        f.parent.abi_mode = 0;
        f.parent.sched_policy = SCHED_NORMAL;
        f.parent.priority = 0;
        f.grandparent.pid = 90;
        f.grandparent.tgid = 90;
        f.child.pid = -1;
        f.child.tgid = -1;
        let self_leader = core::ptr::addr_of_mut!(f.parent);
        let grandparent = core::ptr::addr_of_mut!(f.grandparent);
        let parent = core::ptr::addr_of_mut!(f.parent);
        f.parent.tg_leader = self_leader;
        f.parent.parent = grandparent;
        f.grandparent.tg_leader = grandparent;
        f.child.parent = parent;
        f.child.tg_leader = core::ptr::addr_of_mut!(f.child);
        f.current_is_parent = true;
        guard
    }

    fn parent_ptr() -> *mut core::ffi::c_void {
        core::ptr::addr_of_mut!(fix().parent) as *mut core::ffi::c_void
    }

    fn child_ptr() -> *mut core::ffi::c_void {
        core::ptr::addr_of_mut!(fix().child) as *mut core::ffi::c_void
    }

    fn grandparent_ptr() -> *mut core::ffi::c_void {
        grandparent_task_ptr() as *mut core::ffi::c_void
    }

    fn grandparent_task_ptr() -> *mut FakeTask {
        core::ptr::addr_of_mut!(fix().grandparent)
    }

    fn run_stage(stage: StageFn, tx: &mut CloneTxn) -> i32 {
        unsafe { stage(tx as *mut CloneTxn as *mut core::ffi::c_void) }
    }

    /// Build a transaction already past validate+allocate, with the child view
    /// bound to the fixture's child task.  The call log is cleared so a test
    /// sees only the stage it exercises.
    fn allocated(flags: u64, exit_signal: i32) -> CloneTxn {
        let mut tx = CloneTxn::new(flags, 0x8000, core::ptr::null_mut(), 0,
                                   core::ptr::null_mut(), exit_signal, core::ptr::null_mut());
        tx.parent = parent_ptr();
        let rc = run_stage(stage_validate, &mut tx);
        assert_eq!(rc, 0);
        let rc = run_stage(stage_allocate, &mut tx);
        assert_eq!(rc, 0);
        fix().log_len = 0;
        tx
    }

    // ---- C bridges -------------------------------------------------------

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_view_fill(t: *const core::ffi::c_void, out: *mut CloneView) {
        let src = t as *mut FakeTask;
        unsafe {
            (*out).pid = core::ptr::addr_of_mut!((*src).pid);
            (*out).tgid = core::ptr::addr_of_mut!((*src).tgid);
            (*out).ppid = core::ptr::addr_of_mut!((*src).ppid);
            (*out).parent = core::ptr::addr_of_mut!((*src).parent) as *mut *mut core::ffi::c_void;
            (*out).tg_leader =
                core::ptr::addr_of_mut!((*src).tg_leader) as *mut *mut core::ffi::c_void;
            (*out).abi_mode = core::ptr::addr_of_mut!((*src).abi_mode);
            (*out).exit_signal = core::ptr::addr_of_mut!((*src).exit_signal);
            (*out).clone_flags = core::ptr::addr_of_mut!((*src).clone_flags);
            (*out).clear_child_tid = core::ptr::addr_of_mut!((*src).clear_child_tid);
            (*out).sched_policy = core::ptr::addr_of_mut!((*src).sched_policy);
            (*out).priority = core::ptr::addr_of_mut!((*src).priority);
            (*out).cfs_weight = core::ptr::addr_of_mut!((*src).cfs_weight);
            (*out).exec_load_addr = core::ptr::addr_of_mut!((*src).exec_load_addr);
            (*out).exec_load_size = core::ptr::addr_of_mut!((*src).exec_load_size);
            (*out).name = core::ptr::addr_of_mut!((*src).name) as *mut u8;
            (*out).sched_reset_on_fork = core::ptr::addr_of!((*src).sched_reset_on_fork);
            (*out).nproc_limit = core::ptr::addr_of!((*src).nproc_limit);
        }
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_task_pid(t: *const core::ffi::c_void) -> i32 {
        unsafe { (*(t as *const FakeTask)).pid }
    }

    /// Real fixture tasks report their own uid; walk entries are the small
    /// index pointers `proc_first_task_locked` hands out.
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_task_uid(t: *const core::ffi::c_void) -> i32 {
        let f = fix();
        let p = t as *const FakeTask;
        if p == core::ptr::addr_of!(f.parent)
            || p == core::ptr::addr_of!(f.child)
            || p == core::ptr::addr_of!(f.grandparent)
        {
            unsafe { (*p).uid }
        } else {
            f.walk[(t as usize) - 1].1
        }
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_flags_supported(_flags: u64) -> i32 {
        fix().flags_supported
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_has_cap(_t: *const core::ffi::c_void, _cap: i32) -> i32 {
        fix().has_cap
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_weight_for_nice(_nice: i32) -> u32 {
        fix().weight
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_notify_begin(_parent_pid: i32, _flags: u64) {
        fix().note("notify_begin");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_notify_ready(_t: *mut core::ffi::c_void, _child_pid: i32,
                                               _flags: u64) {
        fix().note("notify_ready");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_inherit_stdio(_t: *mut core::ffi::c_void,
                                                _parent: *mut core::ffi::c_void) {
        fix().note("inherit_stdio");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_inherit_capabilities(_t: *mut core::ffi::c_void,
                                                       _parent: *mut core::ffi::c_void,
                                                       _flags: u64) {
        fix().note("capabilities");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_init_mm(_t: *mut core::ffi::c_void,
                                           _parent: *mut core::ffi::c_void, _flags: u64,
                                           _child_pid: i32) -> i32 {
        fix().note("mm");
        0
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_init_arch(_t: *mut core::ffi::c_void,
                                             _parent: *mut core::ffi::c_void, _flags: u64,
                                             _stack: usize, _tls: usize) -> i32 {
        fix().note("arch");
        0
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_tasklist_lock() -> u64 {
        fix().note("tasklist_lock");
        0
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_tasklist_unlock(_flags: u64) {
        fix().note("tasklist_unlock");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_vfork_snapshot_create(_parent: *mut core::ffi::c_void) -> i32 {
        fix().note("vfork_snapshot");
        fix().snapshot_rc
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_vfork_arm(_parent: *mut core::ffi::c_void) {
        fix().note("vfork_arm");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_vfork_disarm(_parent: *mut core::ffi::c_void) {
        fix().note("vfork_disarm");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_clone_vfork_wait(_child: *mut core::ffi::c_void) {
        fix().note("vfork_wait");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_current() -> *mut core::ffi::c_void {
        if fix().current_is_parent { parent_ptr() } else { core::ptr::null_mut() }
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_alloc_task_slot() -> *mut core::ffi::c_void {
        if fix().task_alloc_ok { child_ptr() } else { core::ptr::null_mut() }
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_pid_alloc() -> i32 {
        fix().pid_alloc
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_task_init_base(t: *mut core::ffi::c_void,
                                             parent: *const core::ffi::c_void) {
        let child = t as *mut FakeTask;
        let par = parent as *const FakeTask;
        unsafe {
            (*child).pid = fix().pid_alloc;
            (*child).tgid = (*child).pid;
            (*child).ppid = (*par).pid;
            (*child).parent = core::ptr::addr_of_mut!(fix().parent);
            (*child).abi_mode = (*par).abi_mode;
            (*child).sched_policy = (*par).sched_policy;
            (*child).priority = (*par).priority;
            (*child).cfs_weight = (*par).cfs_weight;
            (*child).exec_load_addr = (*par).exec_load_addr;
            (*child).exec_load_size = (*par).exec_load_size;
        }
        fix().note("init_base");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_task_init_seccomp(_t: *mut core::ffi::c_void,
                                                _parent: *const core::ffi::c_void) {
        fix().note("seccomp");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_task_init_fs_cgroup(_t: *mut core::ffi::c_void,
                                                  _parent: *const core::ffi::c_void) {
        fix().note("fs_cgroup");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_task_init_signals(_t: *mut core::ffi::c_void,
                                                _parent: *const core::ffi::c_void,
                                                flags: u64,
                                                _checked: i32) -> i32 {
        assert_eq!(flags as i32, fix().child.clone_flags);
        fix().note("signals");
        0
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_set_name(_t: *mut core::ffi::c_void, name: *const u8) {
        let f = fix();
        let parent_name = f.parent.name.as_ptr();
        assert_eq!(name, parent_name);
        f.note("set_name");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_publish_task_slot_locked(_t: *mut core::ffi::c_void) -> i32 {
        fix().note("publish_slot");
        fix().publish_rc
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_tg_link_locked(_t: *mut core::ffi::c_void) -> i32 {
        fix().note("tg_link");
        fix().tg_link_rc
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_children_link_locked(parent: *mut core::ffi::c_void,
                                                   child: *mut core::ffi::c_void) {
        let f = fix();
        f.note("children_link");
        f.note(if parent == grandparent_ptr() { "child_of_grandparent" } else { "child_of_parent" });
        assert_eq!(child, child_ptr());
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_pid_register(_t: *mut core::ffi::c_void) {
        fix().note("pid_register");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_make_ready(_t: *mut core::ffi::c_void) {
        fix().note("make_ready");
    }

    /// Walk entries are handed out as 1-based index pointers so index 0 is not
    /// mistaken for a null end-of-list.
    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_first_task_locked() -> *mut core::ffi::c_void {
        let f = fix();
        f.walk_index = 0;
        if f.walk_len == 0 { core::ptr::null_mut() } else { 1usize as *mut core::ffi::c_void }
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_next_task_locked(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void {
        let f = fix();
        let index = t as usize;
        f.walk_index = index;
        if index >= f.walk_len {
            core::ptr::null_mut()
        } else {
            (index + 1) as *mut core::ffi::c_void
        }
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_task_state_get(t: *mut core::ffi::c_void) -> i32 {
        fix().walk[(t as usize) - 1].0
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_get(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void {
        fix().note("get");
        if fix().get_ok { t } else { core::ptr::null_mut() }
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_put(t: *mut core::ffi::c_void) {
        let f = fix();
        f.note("put");
        assert_eq!(t, child_ptr());
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_destroy_task(t: *mut core::ffi::c_void) {
        let f = fix();
        f.note("destroy");
        assert_eq!(t, child_ptr());
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn fdtable_share(_dst: *mut core::ffi::c_void,
                                       _src: *const core::ffi::c_void) {
        fix().note("fd_share");
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn fdtable_copy_checked(_dst: *mut core::ffi::c_void,
                                              _src: *const core::ffi::c_void) -> i32 {
        fix().note("fd_copy");
        fix().fd_copy_rc
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn mntns_fork(_child: *mut core::ffi::c_void,
                                    _parent: *const core::ffi::c_void, _flags: u64) -> i32 {
        fix().mntns_rc
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn pidns_fork(_child: *mut core::ffi::c_void,
                                    _parent: *const core::ffi::c_void, _flags: u64) -> i32 {
        fix().pidns_rc
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn userns_fork(_child: *mut core::ffi::c_void,
                                     _parent: *const core::ffi::c_void, _flags: u64) -> i32 {
        fix().userns_rc
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn resource_ns_fork(_child: *mut core::ffi::c_void,
                                          _parent: *const core::ffi::c_void, _flags: u64) -> i32 {
        fix().resource_ns_rc
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn copy_to_user(dst: *mut core::ffi::c_void,
                                      src: *const core::ffi::c_void, n: usize) -> isize {
        let f = fix();
        f.note("copy_to_user");
        assert_eq!(n, 4);
        assert_eq!(unsafe { *(src as *const i32) }, f.pid_alloc);
        assert_eq!(dst, core::ptr::addr_of_mut!(f.copy_user_dst) as *mut core::ffi::c_void);
        f.copy_to_user_rc
    }

    #[unsafe(no_mangle)]
    unsafe extern "C" fn proc_copy_to_task_user(_task: *mut core::ffi::c_void,
                                                dst: *mut core::ffi::c_void,
                                                src: *const core::ffi::c_void,
                                                n: usize) -> i32 {
        let f = fix();
        f.note("copy_to_child");
        assert_eq!(n, 4);
        assert_eq!(unsafe { *(src as *const i32) }, f.pid_alloc);
        assert_eq!(dst, core::ptr::addr_of_mut!(f.copy_child_dst) as *mut core::ffi::c_void);
        f.copy_to_task_rc
    }

    /// The runner itself is covered by test_task_construction_host.py; here it
    /// is only a link placeholder that records the entry point was reached.
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_task_construction_run(_ops: *const TaskConstructionOps) -> i32 {
        fix().note("runner");
        fix().runner_rc
    }

    // ---- validate --------------------------------------------------------

    #[test]
    fn validate_requires_a_current_task() {
        let _guard = reset();
        fix().current_is_parent = false;
        let mut tx = CloneTxn::new(0, 0, core::ptr::null_mut(), 0, core::ptr::null_mut(),
                                   SIGCHLD_DEFAULT, core::ptr::null_mut());
        assert_eq!(run_stage(stage_validate, &mut tx), ESRCH);
        assert!(fix().calls().is_empty());
    }

    #[test]
    fn validate_reports_the_architectural_flag_rule() {
        let _guard = reset();
        fix().flags_supported = EINVAL;
        let mut tx = CloneTxn::new(0, 0, core::ptr::null_mut(), 0, core::ptr::null_mut(),
                                   SIGCHLD_DEFAULT, core::ptr::null_mut());
        assert_eq!(run_stage(stage_validate, &mut tx), EINVAL);
        assert_eq!(fix().calls(), ["notify_begin"]);
    }

    #[test]
    fn validate_rejects_out_of_range_exit_signal() {
        let _guard = reset();
        for signal in [-1, NSIG, NSIG + 5] {
            let mut tx = CloneTxn::new(0, 0, core::ptr::null_mut(), 0, core::ptr::null_mut(),
                                       signal, core::ptr::null_mut());
            assert_eq!(run_stage(stage_validate, &mut tx), EINVAL, "signal {signal}");
        }
        let mut tx = CloneTxn::new(0, 0, core::ptr::null_mut(), 0, core::ptr::null_mut(),
                                   NSIG - 1, core::ptr::null_mut());
        assert_eq!(run_stage(stage_validate, &mut tx), 0);
    }

    #[test]
    fn validate_enforces_nproc_for_the_callers_uid() {
        let _guard = reset();
        let f = fix();
        f.parent.nproc_limit = 3;
        f.has_cap = 0;
        f.walk_len = 3;
        f.walk = [(2, 7), (2, 7), (2, 7), (0, 0)];
        let mut tx = CloneTxn::new(0, 0, core::ptr::null_mut(), 0, core::ptr::null_mut(),
                                   SIGCHLD_DEFAULT, core::ptr::null_mut());
        assert_eq!(run_stage(stage_validate, &mut tx), EAGAIN);
        // A cap holder bypasses the limit.
        fix().has_cap = 1;
        assert_eq!(run_stage(stage_validate, &mut tx), 0);
    }

    #[test]
    fn validate_nproc_counts_only_live_matching_uids() {
        let _guard = reset();
        let f = fix();
        f.parent.nproc_limit = 2;
        f.walk_len = 4;
        f.walk = [
            (PROC_UNUSED, 7),
            (PROC_ZOMBIE, 7),
            (2, 7),
            (2, 99),
        ];
        let mut tx = CloneTxn::new(0, 0, core::ptr::null_mut(), 0, core::ptr::null_mut(),
                                   SIGCHLD_DEFAULT, core::ptr::null_mut());
        assert_eq!(run_stage(stage_validate, &mut tx), 0);
        assert_eq!(f.walk_index, 4, "the walk visited every task");
    }

    #[test]
    fn validate_ignores_an_unlimited_nproc() {
        let _guard = reset();
        fix().parent.nproc_limit = 0;
        fix().walk_len = 4;
        fix().walk = [(2, 7), (2, 7), (2, 7), (2, 7)];
        let mut tx = CloneTxn::new(0, 0, core::ptr::null_mut(), 0, core::ptr::null_mut(),
                                   SIGCHLD_DEFAULT, core::ptr::null_mut());
        assert_eq!(run_stage(stage_validate, &mut tx), 0);
        assert_eq!(fix().walk_index, 0, "the walk is not entered at all");
    }

    // ---- allocate --------------------------------------------------------

    #[test]
    fn allocate_takes_a_slot_then_an_id() {
        let _guard = reset();
        let tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(tx.child_pid, 4242);
        assert_eq!(unsafe { *tx.child_view.pid }, 4242);
    }

    #[test]
    fn allocate_fails_when_the_slot_pool_is_empty() {
        let _guard = reset();
        fix().task_alloc_ok = false;
        let mut tx = CloneTxn::new(0, 0, core::ptr::null_mut(), 0, core::ptr::null_mut(),
                                   SIGCHLD_DEFAULT, core::ptr::null_mut());
        assert_eq!(run_stage(stage_validate, &mut tx), 0);
        assert_eq!(run_stage(stage_allocate, &mut tx), EAGAIN);
        assert!(tx.task.is_null());
    }

    #[test]
    fn allocate_fails_when_the_id_space_is_exhausted() {
        let _guard = reset();
        fix().pid_alloc = EAGAIN;
        let mut tx = CloneTxn::new(0, 0, core::ptr::null_mut(), 0, core::ptr::null_mut(),
                                   SIGCHLD_DEFAULT, core::ptr::null_mut());
        assert_eq!(run_stage(stage_validate, &mut tx), 0);
        assert_eq!(run_stage(stage_allocate, &mut tx), EAGAIN);
    }

    // ---- base ------------------------------------------------------------

    #[test]
    fn base_masks_the_exit_signal_for_threads() {
        let _guard = reset();
        let mut tx = allocated(CLONE_THREAD, SIGCHLD_DEFAULT);
        fix().parent.tgid = 55;
        assert_eq!(run_stage(stage_base, &mut tx), 0);
        assert_eq!(unsafe { *tx.child_view.exit_signal }, 0);
        assert_eq!(unsafe { *tx.child_view.tgid }, 55);
    }

    #[test]
    fn base_keeps_the_requested_signal_and_group_for_processes() {
        let _guard = reset();
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_base, &mut tx), 0);
        assert_eq!(unsafe { *tx.child_view.exit_signal }, SIGCHLD_DEFAULT);
        assert_eq!(unsafe { *tx.child_view.tgid }, 4242);
    }

    #[test]
    fn base_redirects_the_parent_for_clone_parent() {
        let _guard = reset();
        let mut tx = allocated(CLONE_PARENT, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_base, &mut tx), 0);
        assert_eq!(unsafe { *tx.child_view.parent }, grandparent_ptr());
        assert_eq!(unsafe { *tx.child_view.ppid }, 90);
    }

    #[test]
    fn base_keeps_the_calling_parent_without_clone_parent() {
        let _guard = reset();
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_base, &mut tx), 0);
        assert_eq!(unsafe { *tx.child_view.parent }, parent_ptr());
        assert_eq!(unsafe { *tx.child_view.ppid }, 100);
    }

    #[test]
    fn base_resets_rt_policy_for_a_reset_on_fork_parent() {
        let _guard = reset();
        let f = fix();
        f.parent.sched_reset_on_fork = 1;
        f.parent.sched_policy = SCHED_RR;
        f.parent.priority = 30;
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_base, &mut tx), 0);
        let child = &tx.child_view;
        assert_eq!(unsafe { *child.sched_policy }, SCHED_NORMAL);
        assert_eq!(unsafe { *child.priority }, 0);
        assert_eq!(unsafe { *child.cfs_weight }, 1000);
    }

    #[test]
    fn base_resets_a_negative_nice_for_a_reset_on_fork_parent() {
        let _guard = reset();
        let f = fix();
        f.parent.sched_reset_on_fork = 1;
        f.parent.sched_policy = SCHED_NORMAL;
        f.parent.priority = -7;
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_base, &mut tx), 0);
        assert_eq!(unsafe { *tx.child_view.priority }, 0);
        assert_eq!(unsafe { *tx.child_view.cfs_weight }, 1000);
    }

    #[test]
    fn base_keeps_policy_without_reset_on_fork() {
        let _guard = reset();
        let f = fix();
        f.parent.sched_policy = SCHED_FIFO;
        f.parent.priority = 30;
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_base, &mut tx), 0);
        assert_eq!(unsafe { *tx.child_view.sched_policy }, SCHED_FIFO);
        assert_eq!(unsafe { *tx.child_view.priority }, 30);
    }

    #[test]
    fn base_records_the_clear_child_tid_and_flags() {
        let _guard = reset();
        let mut ctid: i32 = 0;
        let mut tx = CloneTxn::new(CLONE_CHILD_CLEARTID, 0x8000, core::ptr::null_mut(), 0,
                                   &mut ctid as *mut i32, SIGCHLD_DEFAULT,
                                   core::ptr::null_mut());
        tx.parent = parent_ptr();
        assert_eq!(run_stage(stage_validate, &mut tx), 0);
        assert_eq!(run_stage(stage_allocate, &mut tx), 0);
        assert_eq!(run_stage(stage_base, &mut tx), 0);
        assert_eq!(unsafe { *tx.child_view.clear_child_tid }, &mut ctid as *mut i32);
        assert_eq!(unsafe { *tx.child_view.clone_flags }, CLONE_CHILD_CLEARTID as i32);
    }

    #[test]
    fn base_copies_the_image_bounds_and_the_name() {
        let _guard = reset();
        let f = fix();
        f.parent.exec_load_addr = 0x1234_0000;
        f.parent.exec_load_size = 0x9000;
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_base, &mut tx), 0);
        assert_eq!(unsafe { *tx.child_view.exec_load_addr }, 0x1234_0000);
        assert_eq!(unsafe { *tx.child_view.exec_load_size }, 0x9000);
        assert!(fix().calls().contains(&"set_name"));
    }

    #[test]
    fn base_inherits_the_abi_mode_and_the_stdio_adoption() {
        let _guard = reset();
        fix().parent.abi_mode = 1;
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_base, &mut tx), 0);
        assert_eq!(unsafe { *tx.child_view.abi_mode }, 1);
        assert!(fix().calls().contains(&"inherit_stdio"));
    }

    // ---- files and namespaces -------------------------------------------

    #[test]
    fn files_shares_the_table_for_threads_and_clone_files() {
        let _guard = reset();
        for flags in [CLONE_THREAD, CLONE_FILES] {
            let mut tx = allocated(flags, SIGCHLD_DEFAULT);
            assert_eq!(run_stage(stage_files, &mut tx), 0);
            assert_eq!(fix().calls(), ["fd_share"], "flags {flags:#x}");
        }
    }

    #[test]
    fn files_copies_the_table_for_a_plain_fork() {
        let _guard = reset();
        fix().fd_copy_rc = ENOMEM;
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_files, &mut tx), ENOMEM);
        assert_eq!(fix().calls(), ["fd_copy"]);
    }

    #[test]
    fn namespaces_map_a_mount_failure_to_enomem() {
        let _guard = reset();
        fix().mntns_rc = EINVAL;
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_namespaces, &mut tx), ENOMEM);
    }

    #[test]
    fn namespaces_propagate_the_later_failures_unchanged() {
        let _guard = reset();
        fix().pidns_rc = ESRCH;
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_namespaces, &mut tx), ESRCH);

        let f = fix();
        f.pidns_rc = 0;
        f.userns_rc = EFAULT;
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_namespaces, &mut tx), EFAULT);

        let f = fix();
        f.userns_rc = 0;
        f.resource_ns_rc = EAGAIN;
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_namespaces, &mut tx), EAGAIN);
    }

    // ---- publish ---------------------------------------------------------

    #[test]
    fn publish_makes_a_thread_follow_its_parent_group() {
        let _guard = reset();
        fix().parent.tg_leader = grandparent_task_ptr();
        let mut tx = allocated(CLONE_THREAD, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_publish, &mut tx), 0);
        assert_eq!(unsafe { *tx.child_view.tg_leader }, grandparent_ptr());
        assert_eq!(fix().calls(),
                   ["tasklist_lock", "publish_slot", "tg_link", "children_link",
                    "child_of_parent", "tasklist_unlock", "pid_register"]);
    }

    #[test]
    fn publish_uses_the_parent_as_leader_when_it_has_none() {
        let _guard = reset();
        fix().parent.tg_leader = core::ptr::null_mut();
        let mut tx = allocated(CLONE_THREAD, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_publish, &mut tx), 0);
        assert_eq!(unsafe { *tx.child_view.tg_leader }, parent_ptr());
        assert!(fix().calls().contains(&"tg_link"));
    }

    #[test]
    fn publish_leads_its_own_group_for_a_process() {
        let _guard = reset();
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_publish, &mut tx), 0);
        assert_eq!(unsafe { *tx.child_view.tg_leader }, child_ptr());
        assert!(!fix().calls().contains(&"tg_link"));
    }

    #[test]
    fn publish_links_the_child_under_its_effective_parent() {
        let _guard = reset();
        let mut tx = allocated(CLONE_PARENT, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_base, &mut tx), 0);
        assert_eq!(run_stage(stage_publish, &mut tx), 0);
        assert!(fix().calls().contains(&"child_of_grandparent"));
    }

    #[test]
    fn publish_stops_before_the_group_link_when_the_slot_fails() {
        let _guard = reset();
        fix().publish_rc = EAGAIN;
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_publish, &mut tx), EAGAIN);
        let calls = fix().calls();
        assert!(calls.contains(&"publish_slot"));
        assert!(!calls.contains(&"children_link"));
        assert!(!calls.contains(&"pid_register"));
        assert!(calls.contains(&"tasklist_unlock"), "the lock is always released");
    }

    #[test]
    fn publish_stops_before_the_child_link_when_the_group_link_fails() {
        let _guard = reset();
        fix().tg_link_rc = ESRCH;
        let mut tx = allocated(CLONE_THREAD, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_publish, &mut tx), ESRCH);
        let calls = fix().calls();
        assert!(calls.contains(&"tg_link"));
        assert!(!calls.contains(&"children_link"));
        assert!(!calls.contains(&"pid_register"));
    }

    // ---- post publish ----------------------------------------------------

    #[test]
    fn post_publish_writes_both_tid_slots() {
        let _guard = reset();
        let ptid = core::ptr::addr_of_mut!(fix().copy_user_dst);
        let ctid = core::ptr::addr_of_mut!(fix().copy_child_dst);
        let mut tx = allocated(CLONE_PARENT_SETTID | CLONE_CHILD_SETTID, SIGCHLD_DEFAULT);
        tx.ptid = ptid;
        tx.ctid = ctid;
        assert_eq!(run_stage(stage_post_publish, &mut tx), 0);
        assert_eq!(fix().calls(), ["copy_to_user", "copy_to_child"]);
    }

    #[test]
    fn post_publish_skips_absent_tid_slots() {
        let _guard = reset();
        let mut tx = allocated(CLONE_PARENT_SETTID | CLONE_CHILD_SETTID, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_post_publish, &mut tx), 0);
        assert!(fix().calls().is_empty());
    }

    #[test]
    fn post_publish_reports_a_parent_slot_fault() {
        let _guard = reset();
        fix().copy_to_user_rc = -1;
        let ptid = core::ptr::addr_of_mut!(fix().copy_user_dst);
        let mut tx = allocated(CLONE_PARENT_SETTID, SIGCHLD_DEFAULT);
        tx.ptid = ptid;
        assert_eq!(run_stage(stage_post_publish, &mut tx), EFAULT);
    }

    #[test]
    fn post_publish_reports_a_child_slot_fault() {
        let _guard = reset();
        fix().copy_to_task_rc = EFAULT;
        let ctid = core::ptr::addr_of_mut!(fix().copy_child_dst);
        let mut tx = allocated(CLONE_CHILD_SETTID, SIGCHLD_DEFAULT);
        tx.ctid = ctid;
        assert_eq!(run_stage(stage_post_publish, &mut tx), EFAULT);
    }

    #[test]
    fn post_publish_arms_vfork_and_takes_a_child_reference() {
        let _guard = reset();
        let mut out: *mut core::ffi::c_void = core::ptr::null_mut();
        let mut tx = allocated(CLONE_VFORK, SIGCHLD_DEFAULT);
        tx.out_task = &mut out;
        assert_eq!(run_stage(stage_post_publish, &mut tx), 0);
        assert!(tx.parent_vfork_armed);
        assert_eq!(tx.vfork_child_ref, child_ptr());
        assert_eq!(out, child_ptr());
        assert_eq!(fix().calls(),
                   ["vfork_snapshot", "vfork_arm", "get"]);
    }

    #[test]
    fn post_publish_reports_a_missing_vfork_reference() {
        let _guard = reset();
        fix().get_ok = false;
        let mut tx = allocated(CLONE_VFORK, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_post_publish, &mut tx), ESRCH);
        assert!(tx.parent_vfork_armed);
        assert!(tx.vfork_child_ref.is_null());
    }

    #[test]
    fn post_publish_reports_a_failed_snapshot_before_arming() {
        let _guard = reset();
        fix().snapshot_rc = ENOMEM;
        let mut tx = allocated(CLONE_VFORK, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_post_publish, &mut tx), ENOMEM);
        assert!(!tx.parent_vfork_armed);
        assert_eq!(fix().calls(), ["vfork_snapshot"]);
    }

    #[test]
    fn post_publish_only_publishes_out_task_when_asked() {
        let _guard = reset();
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_post_publish, &mut tx), 0);
        assert!(tx.out_task.is_null());
    }

    // ---- ready, vfork wait, abort ---------------------------------------

    #[test]
    fn ready_makes_the_child_runnable_then_notifies() {
        let _guard = reset();
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_ready, &mut tx), 0);
        assert_eq!(fix().calls(), ["make_ready", "notify_ready"]);
    }

    #[test]
    fn vfork_wait_is_a_noop_without_clone_vfork() {
        let _guard = reset();
        let mut tx = allocated(0, SIGCHLD_DEFAULT);
        unsafe { stage_vfork_wait(&mut tx as *mut CloneTxn as *mut core::ffi::c_void) };
        assert!(fix().calls().is_empty());
    }

    #[test]
    fn vfork_wait_blocks_on_the_child_and_releases_it() {
        let _guard = reset();
        let mut tx = allocated(CLONE_VFORK, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_post_publish, &mut tx), 0);
        fix().log_len = 0;
        unsafe { stage_vfork_wait(&mut tx as *mut CloneTxn as *mut core::ffi::c_void) };
        assert_eq!(fix().calls(), ["vfork_wait"]);
        assert!(tx.vfork_child_ref.is_null());
    }

    #[test]
    fn abort_rolls_back_an_armed_vfork_then_destroys_the_child() {
        let _guard = reset();
        let mut tx = allocated(CLONE_VFORK, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_post_publish, &mut tx), 0);
        fix().log_len = 0;
        unsafe { stage_abort(&mut tx as *mut CloneTxn as *mut core::ffi::c_void, 1) };
        assert_eq!(fix().calls(), ["vfork_disarm", "put", "destroy"]);
        assert!(tx.task.is_null());
        assert!(tx.vfork_child_ref.is_null());
        assert!(!tx.parent_vfork_armed);
    }

    #[test]
    fn abort_before_allocation_is_a_noop() {
        let _guard = reset();
        let mut tx = CloneTxn::new(0, 0, core::ptr::null_mut(), 0, core::ptr::null_mut(),
                                   SIGCHLD_DEFAULT, core::ptr::null_mut());
        unsafe { stage_abort(&mut tx as *mut CloneTxn as *mut core::ffi::c_void, 0) };
        assert!(fix().calls().is_empty());
    }

    #[test]
    fn abort_is_idempotent_after_a_successful_rollback() {
        let _guard = reset();
        let mut tx = allocated(CLONE_VFORK, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_post_publish, &mut tx), 0);
        unsafe { stage_abort(&mut tx as *mut CloneTxn as *mut core::ffi::c_void, 1) };
        let after_first = fix().log_len;
        unsafe { stage_abort(&mut tx as *mut CloneTxn as *mut core::ffi::c_void, 1) };
        assert_eq!(fix().log_len, after_first, "the second abort releases nothing twice");
    }

    #[test]
    fn abort_disarms_an_armed_vfork_without_a_reference() {
        let _guard = reset();
        fix().get_ok = false;
        let mut tx = allocated(CLONE_VFORK, SIGCHLD_DEFAULT);
        assert_eq!(run_stage(stage_post_publish, &mut tx), ESRCH);
        fix().log_len = 0;
        unsafe { stage_abort(&mut tx as *mut CloneTxn as *mut core::ffi::c_void, 1) };
        assert_eq!(fix().calls(), ["vfork_disarm", "destroy"]);
    }

    // ---- entry point and table ------------------------------------------

    #[test]
    fn entry_point_builds_the_table_and_calls_the_runner() {
        let _guard = reset();
        let rc = unsafe {
            a20_proc_clone_impl(0, 0, core::ptr::null_mut(), 0, core::ptr::null_mut(),
                                SIGCHLD_DEFAULT, core::ptr::null_mut())
        };
        assert_eq!(rc, -22, "the placeholder runner's error is propagated");
        assert_eq!(fix().calls(), ["runner"]);
    }

    #[test]
    fn ops_table_carries_every_stage_exactly_once() {
        let mut tx = CloneTxn::new(0, 0, core::ptr::null_mut(), 0, core::ptr::null_mut(),
                                   SIGCHLD_DEFAULT, core::ptr::null_mut());
        let ops = build_ops(&mut tx);
        assert_eq!(ops.context, &mut tx as *mut CloneTxn as *mut core::ffi::c_void);
        assert_eq!(ops.validate as usize, stage_validate as *const () as usize);
        assert_eq!(ops.allocate as usize, stage_allocate as *const () as usize);
        assert_eq!(ops.base as usize, stage_base as *const () as usize);
        assert_eq!(ops.seccomp as usize, stage_seccomp as *const () as usize);
        assert_eq!(ops.fs_cgroup as usize, stage_fs_cgroup as *const () as usize);
        assert_eq!(ops.files as usize, stage_files as *const () as usize);
        assert_eq!(ops.signals as usize, stage_signals as *const () as usize);
        assert_eq!(ops.namespaces as usize, stage_namespaces as *const () as usize);
        assert_eq!(ops.capabilities as usize, stage_capabilities as *const () as usize);
        assert_eq!(ops.mm as usize, stage_mm as *const () as usize);
        assert_eq!(ops.arch as usize, stage_arch as *const () as usize);
        assert_eq!(ops.publish as usize, stage_publish as *const () as usize);
        assert_eq!(ops.post_publish as usize, stage_post_publish as *const () as usize);
        assert_eq!(ops.ready as usize, stage_ready as *const () as usize);
        assert_eq!(ops.vfork_wait as usize, stage_vfork_wait as *const () as usize);
        assert_eq!(ops.abort as usize, stage_abort as *const () as usize);
        assert_eq!(ops.deferred, 0);
        assert_eq!(ops.vfork, 0);
    }

    #[test]
    fn ops_table_derives_deferred_and_vfork_from_the_request() {
        let mut out: *mut core::ffi::c_void = core::ptr::null_mut();
        let mut tx = CloneTxn::new(CLONE_VFORK, 0, core::ptr::null_mut(), 0,
                                   core::ptr::null_mut(), SIGCHLD_DEFAULT, &mut out);
        let ops = build_ops(&mut tx);
        assert_eq!(ops.deferred, 1);
        assert_eq!(ops.vfork, 1);
    }

    const SIGCHLD_DEFAULT: i32 = 17;
}
