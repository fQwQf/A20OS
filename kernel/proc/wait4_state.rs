#![no_std]

//! wait4 child-wait/reap controller (WAIT4_IMPL).
//!
//! Rust owns the child-matching rules (`pid` selector, direct-child and
//! thread-group scope, `__WNOTHREAD`, pinned `target_task` identity), the
//! reap eligibility/classification decisions (zombie / stopped / continued,
//! group-dying leader, still-on-CPU zombie, `WNOWAIT`), the stop/continue
//! report decisions taken under the child's `park_lock`, the wait status
//! encoding, and the scan -> reap / scan -> park transaction order.
//!
//! The C adapter in `kernel/proc/wait.c` retains the IRQ-save
//! `tasklist_lock`/`park_lock` primitives, the children and thread-group
//! chain walks, field sampling, task pin/detach/destroy/release, child-time
//! accounting, the park/wake protocol, and `proc_yield`.

#[cfg(test)]
extern crate std;

use core::ffi::c_void;

/* Sampled task states (proc_state_t in kernel/include/core/types.h). */
pub const WAIT_STATE_UNUSED: i32 = 0;
pub const WAIT_STATE_READY: i32 = 1;
pub const WAIT_STATE_RUNNING: i32 = 2;
pub const WAIT_STATE_BLOCKED: i32 = 3;
pub const WAIT_STATE_STOPPED: i32 = 4;
pub const WAIT_STATE_ZOMBIE: i32 = 5;

/* wait(2) option bits (kernel/proc/wait.c). */
const WNOHANG: i32 = 0x1;
const WUNTRACED: i32 = 0x2;
const WCONTINUED: i32 = 0x8;
const WNOWAIT: i32 = 0x1000000;
const __WNOTHREAD: i32 = 0x2000_0000;

const SIGTRAP: i32 = 5;

/* kernel/include/core/errno.h */
const ECHILD: i32 = 10;
const ERESTARTSYS: i32 = 512;

/* proc_wake_reason_t values used by proc_wake_reason_is_task_interrupt(). */
const WAKE_SIGNAL: i32 = 4;
const WAKE_FATAL_SIGNAL: i32 = 5;
const WAKE_TASK_EXIT: i32 = 6;

/* Stop/continue decision bits returned by a20_wait4_report_decide(). */
pub const WAIT4_REPORT_STOP: i32 = 1;
pub const WAIT4_REPORT_CONT: i32 = 2;
pub const WAIT4_REPORT_CLEAR_STOP: i32 = 4;
pub const WAIT4_REPORT_CLEAR_CONT: i32 = 8;

/// One child's sampled matching fields, filled by the C adapter under
/// `tasklist_lock` before any decision. `parent_tgid`/`parent_pid` are -1
/// when the child's parent link is NULL. `direct_child` is the sampled fact
/// `child->parent == waiter || child->ppid == waiter->pid`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Wait4ChildView {
    pub state: i32,
    pub pid: i32,
    pub pgid: i32,
    pub parent_tgid: i32,
    pub parent_pid: i32,
    pub waiter_tgid: i32,
    pub waiter_pid: i32,
    pub waiter_pgid: i32,
    pub direct_child: u8,
    pub _reserved: [u8; 3],
}

impl Wait4ChildView {
    fn empty() -> Self {
        Self {
            state: 0,
            pid: 0,
            pgid: 0,
            parent_tgid: -1,
            parent_pid: -1,
            waiter_tgid: 0,
            waiter_pid: 0,
            waiter_pgid: 0,
            direct_child: 0,
            _reserved: [0; 3],
        }
    }
}

/// The child's stop/continue facts, sampled by the C adapter under the
/// child's own `park_lock` while it calls `a20_wait4_report_decide`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Wait4ReportView {
    pub state: i32,
    pub options: i32,
    pub stop_report_pending: i32,
    pub continue_report_pending: i32,
    pub ptrace_stop_active: i32,
    pub ptrace_event: i32,
    pub exit_code: i32,
    pub _reserved: i32,
}

type ScanBeginFn = unsafe extern "C" fn(*mut c_void) -> u64;
type ScanUnlockFn = unsafe extern "C" fn(*mut c_void, u64);
type YieldFn = unsafe extern "C" fn(*mut c_void);
type GroupStartFn = unsafe extern "C" fn(*mut c_void, i32) -> *mut c_void;
type ChainNextFn = unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void;
type ChildViewFn =
    unsafe extern "C" fn(*mut c_void, *mut c_void, *mut Wait4ChildView) -> i32;
type ChildFlagFn = unsafe extern "C" fn(*mut c_void, *mut c_void) -> i32;
type ReapPinFn =
    unsafe extern "C" fn(*mut c_void, *mut c_void, i32, *mut i32) -> *mut c_void;
type ChildVoidFn = unsafe extern "C" fn(*mut c_void, *mut c_void);
type ReportTakeFn = unsafe extern "C" fn(*mut c_void, *mut c_void, i32) -> i32;
type ParkPublishFn = unsafe extern "C" fn(*mut c_void) -> i32;
type ParkCommitFn = unsafe extern "C" fn(*mut c_void) -> i32;
type ContextVoidFn = unsafe extern "C" fn(*mut c_void);

/// Mechanically ordered C primitives. Every callback runs synchronously.
/// `group_start`/`member_next`/`child_first`/`child_next`/`child_view`/
/// `group_dead`/`is_current_any_cpu`/`reap_pin`/`reap_detach`/`report_take`
/// require `tasklist_lock` held from `scan_begin`. `park_publish` runs with
/// `tasklist_lock` still held and returns the sampled signal flag;
/// `scan_unlock` is ordered after it by the Rust driver.
///
/// `reap_pin(child, want_pin, &code)` runs under the child's `park_lock` and
/// always samples the exit code. It takes a task reference (`proc_get`) only
/// when `want_pin` is nonzero and returns that pinned task (NULL when the
/// reference cannot be taken); with `want_pin == 0` it returns the child as an
/// unpinned alias that must not be passed to `reap_release`.
#[repr(C)]
pub struct Wait4Ops {
    pub context: *mut c_void,
    pub scan_begin: ScanBeginFn,
    pub scan_unlock: ScanUnlockFn,
    pub yield_now: YieldFn,
    pub group_start: GroupStartFn,
    pub member_next: ChainNextFn,
    pub child_first: ChainNextFn,
    pub child_next: ChainNextFn,
    pub child_view: ChildViewFn,
    pub group_dead: ChildFlagFn,
    pub is_current_any_cpu: ChildFlagFn,
    pub reap_pin: ReapPinFn,
    pub reap_detach: ChildVoidFn,
    pub reap_release: ChildVoidFn,
    pub report_take: ReportTakeFn,
    pub park_publish: ParkPublishFn,
    pub park_commit: ParkCommitFn,
    pub park_finish: ContextVoidFn,
    pub park_clear: ContextVoidFn,
}

/// Effective thread-group id of a sampled task: `tgid > 0 ? tgid : pid`, with
/// -1 for a missing parent (`parent_tgid`/`parent_pid` are both -1 then).
#[inline]
fn task_tgid(tgid: i32, pid: i32) -> i32 {
    if tgid > 0 {
        tgid
    } else if pid > 0 {
        pid
    } else {
        -1
    }
}

/// Child-selection rule of wait4.
#[inline]
fn child_matches(
    view: &Wait4ChildView,
    pid: i32,
    options: i32,
    target_match: bool,
) -> bool {
    if !target_match {
        return false;
    }
    if options & __WNOTHREAD != 0 {
        if view.direct_child == 0 {
            return false;
        }
    } else if view.direct_child == 0 {
        let parent_group = task_tgid(view.parent_tgid, view.parent_pid);
        let waiter_group = task_tgid(view.waiter_tgid, view.waiter_pid);
        if parent_group != waiter_group {
            return false;
        }
    }
    if pid > 0 && view.pid != pid {
        return false;
    }
    if pid == 0 && view.pgid != view.waiter_pgid {
        return false;
    }
    if pid < -1 && view.pgid != -pid {
        return false;
    }
    true
}

/// wait(2) exit-status encoding: normal exit (>= 0) or terminating signal.
#[inline]
fn exit_status(code: i32) -> i32 {
    if code >= 0 {
        (code & 0xFF) << 8
    } else {
        (-code) & 0xFF
    }
}

#[inline]
fn is_task_interrupt(reason: i32) -> bool {
    reason == WAKE_SIGNAL || reason == WAKE_FATAL_SIGNAL || reason == WAKE_TASK_EXIT
}

/// Decide and encode one stop/continue report while the child's `park_lock`
/// is held. Returns `WAIT4_REPORT_STOP`/`WAIT4_REPORT_CONT` bits for the
/// reported event plus `WAIT4_REPORT_CLEAR_*` bits for flags the adapter must
/// clear (never under `WNOWAIT`). Writes `*status` when non-null, exactly like
/// the C reference.
///
/// # Safety
/// `view` must point to a valid `Wait4ReportView`; `status` is null or a
/// writable `int`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_wait4_report_decide(
    view: *const Wait4ReportView,
    status: *mut i32,
) -> i32 {
    if view.is_null() {
        return 0;
    }
    // SAFETY: the C adapter passes a live aligned view built for this call.
    let view = unsafe { &*view };
    let options = view.options;
    let take_stop = view.state == WAIT_STATE_STOPPED
        && view.stop_report_pending != 0
        && (options & WUNTRACED != 0 || view.ptrace_stop_active != 0);
    let take_cont = !take_stop
        && options & WCONTINUED != 0
        && view.continue_report_pending != 0;
    if take_stop {
        if !status.is_null() {
            let value = if view.ptrace_event != 0 {
                (view.ptrace_event << 16) | (SIGTRAP << 8) | 0x7F
            } else {
                (view.exit_code << 8) | 0x7F
            };
            // SAFETY: documented non-null writable int from the waiter.
            unsafe { status.write(value) };
        }
    } else if take_cont && !status.is_null() {
        // SAFETY: same contract as the stop status write.
        unsafe { status.write(0xFFFF) };
    }
    let mut action = 0;
    if take_stop {
        action |= WAIT4_REPORT_STOP;
        if options & WNOWAIT == 0 {
            action |= WAIT4_REPORT_CLEAR_STOP;
        }
    }
    if take_cont {
        action |= WAIT4_REPORT_CONT;
        if options & WNOWAIT == 0 {
            action |= WAIT4_REPORT_CLEAR_CONT;
        }
    }
    action
}

/// Run one wait4 request to completion (or to the next park generation).
///
/// # Safety
/// `ops` must point to a complete, live `Wait4Ops` table whose context and
/// callbacks obey the header contract and `kernel/proc/wait.c`'s lock rules;
/// it must stay valid for the whole call. `status` is null or a writable
/// `int` owned by the calling thread. `target_task` is null or the pinned
/// task pointer whose identity the call must match. The adapter must not
/// re-enter this function for the same context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_wait4_run(
    ops: *const Wait4Ops,
    pid: i32,
    status: *mut i32,
    options: i32,
    target_task: *mut c_void,
) -> i32 {
    if ops.is_null() {
        return -ECHILD;
    }
    // SAFETY: documented table contract; read-only for this call.
    let ops = unsafe { &*ops };
    let ctx = ops.context;
    loop {
        // SAFETY: every callback below satisfies the header contract.
        let flags = unsafe { (ops.scan_begin)(ctx) };
        let mut found = false;
        let whole_group = i32::from(options & __WNOTHREAD == 0);
        let mut member = unsafe { (ops.group_start)(ctx, whole_group) };
        while !member.is_null() {
            let mut child = unsafe { (ops.child_first)(ctx, member) };
            while !child.is_null() {
                let next = unsafe { (ops.child_next)(ctx, child) };
                let mut view = Wait4ChildView::empty();
                let sampled =
                    unsafe { (ops.child_view)(ctx, child, &mut view) } == 0;
                let target_match = target_task.is_null() || child == target_task;
                if sampled
                    && view.state != WAIT_STATE_UNUSED
                    && child_matches(&view, pid, options, target_match)
                {
                    found = true;
                    if view.state == WAIT_STATE_ZOMBIE
                        && unsafe { (ops.group_dead)(ctx, child) } == 0
                    {
                        /* Zombie leader with live member threads: waitable
                         * but not yet reportable. */
                        child = next;
                        continue;
                    }
                    if view.state == WAIT_STATE_ZOMBIE {
                        if unsafe { (ops.is_current_any_cpu)(ctx, child) } != 0 {
                            /* The switch-out completion will notify us under
                             * the scan lock. Park below (or return WNOHANG)
                             * rather than polling a stack still in use. */
                            child = next;
                            continue;
                        }
                        /* WNOWAIT only reports: match the C reference and
                         * sample the exit code without taking a reference. */
                        let want_pin = i32::from(options & WNOWAIT == 0);
                        let mut exit_code: i32 = 0;
                        let pin = unsafe {
                            (ops.reap_pin)(ctx, child, want_pin, &mut exit_code)
                        };
                        if !status.is_null() {
                            // SAFETY: waiter-owned writable int.
                            unsafe { status.write(exit_status(exit_code)) };
                        }
                        if options & WNOWAIT == 0 {
                            if pin.is_null() {
                                unsafe { (ops.scan_unlock)(ctx, flags) };
                                return -ECHILD;
                            }
                            unsafe { (ops.reap_detach)(ctx, child) };
                            unsafe { (ops.scan_unlock)(ctx, flags) };
                            unsafe { (ops.reap_release)(ctx, pin) };
                            return view.pid;
                        }
                        /* WNOWAIT: reported, left reapable. */
                        unsafe { (ops.scan_unlock)(ctx, flags) };
                        return view.pid;
                    }
                    if view.state == WAIT_STATE_STOPPED || options & WCONTINUED != 0 {
                        let action =
                            unsafe { (ops.report_take)(ctx, child, view.state) };
                        if action & (WAIT4_REPORT_STOP | WAIT4_REPORT_CONT) != 0 {
                            unsafe { (ops.scan_unlock)(ctx, flags) };
                            return view.pid;
                        }
                    }
                }
                child = next;
            }
            member = unsafe { (ops.member_next)(ctx, member) };
        }

        if !found {
            unsafe { (ops.scan_unlock)(ctx, flags) };
            return -ECHILD;
        }
        if options & WNOHANG != 0 {
            unsafe { (ops.scan_unlock)(ctx, flags) };
            return 0;
        }

        /* Publish wait registration and the park token before releasing the
         * scan lock, closing the scan-to-park lost-wakeup window. */
        let signal_seen = unsafe { (ops.park_publish)(ctx) };
        unsafe { (ops.scan_unlock)(ctx, flags) };
        let reason = unsafe { (ops.park_commit)(ctx) };
        unsafe { (ops.park_finish)(ctx) };
        unsafe { (ops.park_clear)(ctx) };
        if is_task_interrupt(reason) || signal_seen != 0 {
            return -ERESTARTSYS;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::boxed::Box;
    use std::vec::Vec;

    const EVENT: i32 = 1;

    #[derive(Default)]
    struct MockChild {
        state: i32,
        pid: i32,
        pgid: i32,
        parent_tgid: i32,
        parent_pid: i32,
        direct_child: u8,
        group_dead: bool,
        current_any_cpu: u32,
        exit_code: i32,
        stop_pending: i32,
        cont_pending: i32,
        ptrace_active: i32,
        ptrace_event: i32,
        detached: bool,
        released: bool,
        next: Option<usize>,
    }

    #[derive(Default)]
    struct MockMember {
        first_child: Option<usize>,
    }

    #[derive(Default)]
    struct MockWorld {
        waiter_tgid: i32,
        waiter_pid: i32,
        waiter_pgid: i32,
        self_member: usize,
        members: Vec<MockMember>,
        children: Vec<MockChild>,
        events: Vec<&'static str>,
        lock_depth: i32,
        yields: u32,
        park_signal: i32,
        park_reason: i32,
        park_commit_hook: Option<unsafe fn(*mut MockWorld)>,
        status_ptr: *mut i32,
        options: i32,
        pin_fails: bool,
    }

    impl MockWorld {
        fn new() -> Box<MockWorld> {
            let mut world = Box::new(MockWorld {
                self_member: 0,
                status_ptr: core::ptr::null_mut(),
                ..MockWorld::default()
            });
            world.members.push(MockMember::default());
            world
        }

        fn add_member(&mut self) -> usize {
            self.members.push(MockMember::default());
            self.members.len() - 1
        }

        fn add_child(&mut self, child: MockChild) -> usize {
            self.children.push(child);
            self.children.len() - 1
        }
    }

    fn member_ptr(world: &mut MockWorld, index: usize) -> *mut c_void {
        (&mut world.members[index]) as *mut MockMember as *mut c_void
    }

    fn child_ptr(world: &mut MockWorld, index: usize) -> *mut c_void {
        (&mut world.children[index]) as *mut MockChild as *mut c_void
    }

    fn member_index(world: &MockWorld, member: *mut c_void) -> usize {
        (member as usize - world.members.as_ptr() as usize)
            / core::mem::size_of::<MockMember>()
    }

    fn child_index(world: &MockWorld, child: *mut c_void) -> usize {
        (child as usize - world.children.as_ptr() as usize)
            / core::mem::size_of::<MockChild>()
    }

    unsafe extern "C" fn mock_scan_begin(ctx: *mut c_void) -> u64 {
        // SAFETY: the driver passes the world pointer back unchanged.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        world.events.push("lock");
        world.lock_depth += 1;
        0
    }

    unsafe extern "C" fn mock_scan_unlock(ctx: *mut c_void, _flags: u64) {
        // SAFETY: same world pointer as scan_begin.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        world.events.push("unlock");
        world.lock_depth -= 1;
    }

    unsafe extern "C" fn mock_yield(ctx: *mut c_void) {
        // SAFETY: same world pointer.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        world.events.push("yield");
        world.yields += 1;
    }

    unsafe extern "C" fn mock_group_start(ctx: *mut c_void, whole_group: i32) -> *mut c_void {
        // SAFETY: same world pointer.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        let start = if whole_group != 0 {
            world.events.push("group_start:whole");
            0
        } else {
            world.events.push("group_start:self");
            world.self_member
        };
        member_ptr(world, start)
    }

    unsafe extern "C" fn mock_member_next(ctx: *mut c_void, member: *mut c_void) -> *mut c_void {
        // SAFETY: same world pointer; the member chain is fixed at run start.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        let index = member_index(world, member);
        if index + 1 < world.members.len() {
            member_ptr(world, index + 1)
        } else {
            core::ptr::null_mut()
        }
    }

    unsafe extern "C" fn mock_child_first(ctx: *mut c_void, member: *mut c_void) -> *mut c_void {
        // SAFETY: same world pointer.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        let index = member_index(world, member);
        world.events.push("child_first");
        match world.members[index].first_child {
            Some(child) => child_ptr(world, child),
            None => core::ptr::null_mut(),
        }
    }

    unsafe extern "C" fn mock_child_next(ctx: *mut c_void, child: *mut c_void) -> *mut c_void {
        // SAFETY: same world pointer; the sibling chain is fixed at run start.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        let index = child_index(world, child);
        world.events.push("child_next");
        match world.children[index].next {
            Some(next) => child_ptr(world, next),
            None => core::ptr::null_mut(),
        }
    }

    unsafe extern "C" fn mock_child_view(
        ctx: *mut c_void,
        child: *mut c_void,
        out: *mut Wait4ChildView,
    ) -> i32 {
        // SAFETY: same world pointer; driver-provided writable view.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        let index = child_index(world, child);
        world.events.push("view");
        let c = &world.children[index];
        let view = Wait4ChildView {
            state: c.state,
            pid: c.pid,
            pgid: c.pgid,
            parent_tgid: c.parent_tgid,
            parent_pid: c.parent_pid,
            waiter_tgid: world.waiter_tgid,
            waiter_pid: world.waiter_pid,
            waiter_pgid: world.waiter_pgid,
            direct_child: c.direct_child,
            _reserved: [0; 3],
        };
        unsafe { out.write(view) };
        0
    }

    unsafe extern "C" fn mock_group_dead(ctx: *mut c_void, child: *mut c_void) -> i32 {
        // SAFETY: same world pointer.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        let index = child_index(world, child);
        world.events.push("group_dead");
        i32::from(world.children[index].group_dead)
    }

    unsafe extern "C" fn mock_is_current(ctx: *mut c_void, child: *mut c_void) -> i32 {
        // SAFETY: same world pointer.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        let index = child_index(world, child);
        world.events.push("current");
        let c = &mut world.children[index];
        if c.current_any_cpu > 0 {
            c.current_any_cpu -= 1;
            1
        } else {
            0
        }
    }

    unsafe extern "C" fn mock_reap_pin(
        ctx: *mut c_void,
        child: *mut c_void,
        want_pin: i32,
        exit_code_out: *mut i32,
    ) -> *mut c_void {
        // SAFETY: same world pointer; driver-provided writable code slot.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        let index = child_index(world, child);
        world
            .events
            .push(if want_pin != 0 { "pin" } else { "sample" });
        let exit_code = world.children[index].exit_code;
        unsafe { exit_code_out.write(exit_code) };
        if want_pin != 0 && world.pin_fails {
            core::ptr::null_mut()
        } else {
            child
        }
    }

    unsafe extern "C" fn mock_reap_detach(ctx: *mut c_void, child: *mut c_void) {
        // SAFETY: same world pointer.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        let index = child_index(world, child);
        world.events.push("detach");
        world.children[index].detached = true;
    }

    unsafe extern "C" fn mock_reap_release(ctx: *mut c_void, pin: *mut c_void) {
        // SAFETY: same world pointer.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        let index = child_index(world, pin);
        world.events.push("release");
        world.children[index].released = true;
    }

    unsafe extern "C" fn mock_report_take(
        ctx: *mut c_void,
        child: *mut c_void,
        state: i32,
    ) -> i32 {
        // SAFETY: same world pointer; single-threaded scripted world.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        let index = child_index(world, child);
        world.events.push("report");
        let options = world.options;
        let status_ptr = world.status_ptr;
        let c = &mut world.children[index];
        let view = Wait4ReportView {
            state,
            options,
            stop_report_pending: c.stop_pending,
            continue_report_pending: c.cont_pending,
            ptrace_stop_active: c.ptrace_active,
            ptrace_event: c.ptrace_event,
            exit_code: c.exit_code,
            _reserved: 0,
        };
        let action = unsafe { a20_wait4_report_decide(&view, status_ptr) };
        if action & WAIT4_REPORT_CLEAR_STOP != 0 {
            c.stop_pending = 0;
        }
        if action & WAIT4_REPORT_CLEAR_CONT != 0 {
            c.cont_pending = 0;
        }
        action
    }

    unsafe extern "C" fn mock_park_publish(ctx: *mut c_void) -> i32 {
        // SAFETY: same world pointer.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        world.events.push("park_publish");
        world.park_signal
    }

    unsafe extern "C" fn mock_park_commit(ctx: *mut c_void) -> i32 {
        // SAFETY: same world pointer; the hook gets a fresh exclusive borrow.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        world.events.push("park_commit");
        let hook = world.park_commit_hook;
        let reason = world.park_reason;
        if let Some(hook) = hook {
            unsafe { hook(ctx as *mut MockWorld) };
        }
        reason
    }

    unsafe extern "C" fn mock_park_finish(ctx: *mut c_void) {
        // SAFETY: same world pointer.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        world.events.push("park_finish");
    }

    unsafe extern "C" fn mock_park_clear(ctx: *mut c_void) {
        // SAFETY: same world pointer.
        let world = unsafe { &mut *(ctx as *mut MockWorld) };
        world.events.push("park_clear");
    }

    fn make_ops(world: *mut MockWorld) -> Wait4Ops {
        Wait4Ops {
            context: world as *mut c_void,
            scan_begin: mock_scan_begin,
            scan_unlock: mock_scan_unlock,
            yield_now: mock_yield,
            group_start: mock_group_start,
            member_next: mock_member_next,
            child_first: mock_child_first,
            child_next: mock_child_next,
            child_view: mock_child_view,
            group_dead: mock_group_dead,
            is_current_any_cpu: mock_is_current,
            reap_pin: mock_reap_pin,
            reap_detach: mock_reap_detach,
            reap_release: mock_reap_release,
            report_take: mock_report_take,
            park_publish: mock_park_publish,
            park_commit: mock_park_commit,
            park_finish: mock_park_finish,
            park_clear: mock_park_clear,
        }
    }

    struct RunResult {
        rc: i32,
        status: i32,
    }

    fn run(world: &mut MockWorld, pid: i32, options: i32, target: usize) -> RunResult {
        let mut status: i32 = -1;
        world.status_ptr = &mut status;
        world.options = options;
        let target_ptr = if target == usize::MAX {
            core::ptr::null_mut()
        } else {
            child_ptr(world, target)
        };
        let ops = make_ops(world as *mut MockWorld);
        let rc = unsafe {
            a20_wait4_run(&ops, pid, &mut status, options, target_ptr)
        };
        let lock_depth = world.lock_depth;
        assert_eq!(lock_depth, 0, "scan lock not released (depth {lock_depth})");
        RunResult { rc, status }
    }

    fn zombie(pid: i32) -> MockChild {
        MockChild {
            state: WAIT_STATE_ZOMBIE,
            pid,
            pgid: 50,
            parent_tgid: 100,
            parent_pid: 100,
            direct_child: 1,
            group_dead: true,
            exit_code: 7,
            ..MockChild::default()
        }
    }

    fn running(pid: i32) -> MockChild {
        MockChild {
            state: WAIT_STATE_RUNNING,
            pid,
            pgid: 50,
            parent_tgid: 100,
            parent_pid: 100,
            direct_child: 1,
            ..MockChild::default()
        }
    }

    fn base_world() -> Box<MockWorld> {
        let mut world = MockWorld::new();
        world.waiter_tgid = 100;
        world.waiter_pid = 100;
        world.waiter_pgid = 50;
        world
    }

    #[test]
    fn reap_zombie_runs_pin_detach_release_transaction_in_order() {
        let mut world = base_world();
        let c0 = world.add_child(zombie(11));
        world.members[0].first_child = Some(c0);
        let result = run(&mut world, -1, 0, usize::MAX);
        assert_eq!(result.rc, 11);
        assert_eq!(result.status, 0x700);
        assert_eq!(
            world.events,
            [
                "lock",
                "group_start:whole",
                "child_first",
                "child_next",
                "view",
                "group_dead",
                "current",
                "pin",
                "detach",
                "unlock",
                "release",
            ]
        );
        assert!(world.children[c0].detached);
        assert!(world.children[c0].released);
        assert_eq!(world.yields, 0);
    }

    #[test]
    fn no_children_returns_echild() {
        let mut world = base_world();
        let result = run(&mut world, -1, 0, usize::MAX);
        assert_eq!(result.rc, -ECHILD);
        assert!(!world.events.contains(&"pin"));
        assert_eq!(world.yields, 0);
    }

    #[test]
    fn wnohang_running_child_returns_zero_without_reaping() {
        let mut world = base_world();
        let c0 = world.add_child(running(11));
        world.members[0].first_child = Some(c0);
        let result = run(&mut world, -1, WNOHANG, usize::MAX);
        assert_eq!(result.rc, 0);
        assert!(!world.children[c0].detached);
        assert!(!world.children[c0].released);
    }

    #[test]
    fn group_dying_zombie_leader_blocks_nohang_report_but_not_echild() {
        let mut world = base_world();
        let c0 = world.add_child(zombie(11));
        world.children[c0].group_dead = false;
        world.members[0].first_child = Some(c0);
        let result = run(&mut world, -1, WNOHANG, usize::MAX);
        assert_eq!(result.rc, 0, "found-but-group-dying must not be ECHILD");
        assert!(!world.events.contains(&"pin"));
        /* Same tree becomes reapable once the group is dead. */
        world.children[c0].group_dead = true;
        let result = run(&mut world, -1, WNOHANG, usize::MAX);
        assert_eq!(result.rc, 11);
        let _ = world.events.clear();
    }

    #[test]
    fn zombie_on_cpu_parks_and_rescans_before_reaping() {
        let mut world = base_world();
        let c0 = world.add_child(zombie(11));
        world.children[c0].current_any_cpu = 1;
        world.members[0].first_child = Some(c0);
        let result = run(&mut world, -1, 0, usize::MAX);
        assert_eq!(result.rc, 11);
        assert_eq!(world.yields, 0);
        let commit = world.events.iter().position(|e| *e == "park_commit").unwrap();
        let pin = world.events.iter().position(|e| *e == "pin").unwrap();
        assert!(commit < pin);
    }

    #[test]
    fn wnowait_reports_status_and_leaves_child_reapable() {
        let mut world = base_world();
        let c0 = world.add_child(zombie(11));
        world.children[c0].exit_code = 3;
        world.members[0].first_child = Some(c0);
        let result = run(&mut world, -1, WNOWAIT, usize::MAX);
        assert_eq!(result.rc, 11);
        assert_eq!(result.status, 0x300);
        assert!(!world.children[c0].detached);
        assert!(!world.children[c0].released);
        assert!(!world.events.contains(&"pin"));
        assert!(!world.events.contains(&"detach"));
        assert!(!world.events.contains(&"release"));
        assert!(world.events.contains(&"sample"));
        /* The report-only path must succeed even when a pinned reference
         * could not be taken: it never asks for one. */
        world.pin_fails = true;
        world.events.clear();
        let result = run(&mut world, -1, WNOWAIT, usize::MAX);
        assert_eq!(result.rc, 11);
        assert_eq!(result.status, 0x300);
        assert!(!world.children[c0].released);
    }

    #[test]
    fn pid_selectors_match_by_pid_pgid_and_group() {
        for (pid_selector, expect) in [(11, 11), (0, 11), (-50, 11)] {
            let mut world = base_world();
            let c0 = world.add_child(zombie(11));
            world.members[0].first_child = Some(c0);
            let result = run(&mut world, pid_selector, 0, usize::MAX);
            assert_eq!(result.rc, expect, "selector {pid_selector}");
            assert!(world.children[c0].released);
        }
        let mut world = base_world();
        let c0 = world.add_child(zombie(11));
        world.children[c0].pgid = 51;
        world.members[0].first_child = Some(c0);
        assert_eq!(run(&mut world, -50, 0, usize::MAX).rc, -ECHILD);
        assert_eq!(run(&mut world, 0, 0, usize::MAX).rc, -ECHILD);
        assert_eq!(run(&mut world, 12, 0, usize::MAX).rc, -ECHILD);
    }

    #[test]
    fn wnothing_scope_limits_scan_to_the_waiter_member() {
        let mut world = base_world();
        let c0 = world.add_child(zombie(11));
        world.members[0].first_child = Some(c0);
        world.self_member = world.add_member();
        let result = run(&mut world, -1, __WNOTHREAD, usize::MAX);
        assert_eq!(result.rc, -ECHILD);
        /* The same child is in scope for the whole-thread-group walk. */
        let result = run(&mut world, -1, 0, usize::MAX);
        assert_eq!(result.rc, 11);
    }

    #[test]
    fn indirect_child_matches_only_through_parent_group() {
        let mut world = base_world();
        let c0 = world.add_child(zombie(11));
        world.children[c0].direct_child = 0;
        world.members[0].first_child = Some(c0);
        assert_eq!(run(&mut world, -1, 0, usize::MAX).rc, 11);
        assert!(world.children[c0].released);

        let mut world = base_world();
        let c0 = world.add_child(zombie(11));
        world.children[c0].direct_child = 0;
        world.children[c0].parent_tgid = 7;
        world.children[c0].parent_pid = 7;
        world.members[0].first_child = Some(c0);
        assert_eq!(run(&mut world, -1, 0, usize::MAX).rc, -ECHILD);
    }

    #[test]
    fn target_identity_selects_the_pinned_child_only() {
        let mut world = base_world();
        let c0 = world.add_child(zombie(11));
        let c1 = world.add_child(zombie(12));
        world.children[c0].exit_code = 7;
        world.children[c1].exit_code = 8;
        world.children[c0].next = Some(c1);
        world.members[0].first_child = Some(c0);
        let result = run(&mut world, -1, 0, c1);
        assert_eq!(result.rc, 12);
        assert_eq!(result.status, 0x800);
        assert!(!world.children[c0].released);
        assert!(world.children[c1].released);
    }

    #[test]
    fn reap_pin_failure_returns_echild_without_detaching() {
        let mut world = base_world();
        let c0 = world.add_child(zombie(11));
        world.members[0].first_child = Some(c0);
        world.pin_fails = true;
        let result = run(&mut world, -1, 0, usize::MAX);
        assert_eq!(result.rc, -ECHILD);
        assert!(!world.children[c0].detached);
        assert!(!world.children[c0].released);
    }

    #[test]
    fn stop_report_requires_wuntraced_or_ptrace_and_honors_wnowait() {
        let mut world = base_world();
        let c0 = world.add_child(MockChild {
            state: WAIT_STATE_STOPPED,
            pid: 11,
            pgid: 50,
            parent_tgid: 100,
            parent_pid: 100,
            direct_child: 1,
            exit_code: 19,
            stop_pending: 1,
            ..MockChild::default()
        });
        world.members[0].first_child = Some(c0);
        let result = run(&mut world, -1, WUNTRACED, usize::MAX);
        assert_eq!(result.rc, 11);
        assert_eq!(result.status, 0x137F);
        assert_eq!(world.children[c0].stop_pending, 0);
        assert!(!world.children[c0].released, "stop report must not reap");

        /* WNOWAIT reports the stop again but leaves it pending. */
        let mut world = base_world();
        let c0 = world.add_child(MockChild {
            state: WAIT_STATE_STOPPED,
            pid: 11,
            pgid: 50,
            parent_tgid: 100,
            parent_pid: 100,
            direct_child: 1,
            exit_code: 19,
            stop_pending: 1,
            ..MockChild::default()
        });
        world.members[0].first_child = Some(c0);
        let result = run(&mut world, -1, WUNTRACED | WNOWAIT, usize::MAX);
        assert_eq!(result.rc, 11);
        assert_eq!(result.status, 0x137F);
        assert_eq!(
            world.children[c0].stop_pending, 1,
            "WNOWAIT must not clear the pending stop"
        );
        let result = run(&mut world, -1, WUNTRACED, usize::MAX);
        assert_eq!(result.rc, 11);
        assert_eq!(result.status, 0x137F);
        assert_eq!(world.children[c0].stop_pending, 0);
    }

    #[test]
    fn stop_report_without_pending_is_not_reported() {
        let mut world = base_world();
        let c0 = world.add_child(MockChild {
            state: WAIT_STATE_STOPPED,
            pid: 11,
            pgid: 50,
            parent_tgid: 100,
            parent_pid: 100,
            direct_child: 1,
            exit_code: 19,
            ..MockChild::default()
        });
        world.members[0].first_child = Some(c0);
        let result = run(&mut world, -1, WUNTRACED | WNOHANG, usize::MAX);
        assert_eq!(result.rc, 0);
        assert!(!world.children[c0].released);
    }

    #[test]
    fn ptrace_stop_reports_without_wuntraced_with_event_status() {
        let mut world = base_world();
        let c0 = world.add_child(MockChild {
            state: WAIT_STATE_STOPPED,
            pid: 11,
            pgid: 50,
            parent_tgid: 100,
            parent_pid: 100,
            direct_child: 1,
            exit_code: 19,
            stop_pending: 1,
            ptrace_active: 1,
            ptrace_event: 7,
            ..MockChild::default()
        });
        world.members[0].first_child = Some(c0);
        let result = run(&mut world, -1, 0, usize::MAX);
        assert_eq!(result.rc, 11);
        assert_eq!(result.status, (7 << 16) | (SIGTRAP << 8) | 0x7F);
        assert_eq!(world.children[c0].stop_pending, 0);
    }

    #[test]
    fn continue_report_needs_wcontinued_and_yields_0xffff() {
        let mut world = base_world();
        let c0 = world.add_child(MockChild {
            state: WAIT_STATE_READY,
            pid: 11,
            pgid: 50,
            parent_tgid: 100,
            parent_pid: 100,
            direct_child: 1,
            cont_pending: 1,
            ..MockChild::default()
        });
        world.members[0].first_child = Some(c0);
        let result = run(&mut world, -1, WCONTINUED, usize::MAX);
        assert_eq!(result.rc, 11);
        assert_eq!(result.status, 0xFFFF);
        assert_eq!(world.children[c0].cont_pending, 0);

        let result = run(&mut world, -1, WCONTINUED | WNOHANG, usize::MAX);
        assert_eq!(result.rc, 0, "no pending continue left to report");
        assert!(!world.children[c0].released);
    }

    #[test]
    fn stop_report_takes_precedence_over_continue() {
        let mut world = base_world();
        let c0 = world.add_child(MockChild {
            state: WAIT_STATE_STOPPED,
            pid: 11,
            pgid: 50,
            parent_tgid: 100,
            parent_pid: 100,
            direct_child: 1,
            exit_code: 19,
            stop_pending: 1,
            cont_pending: 1,
            ..MockChild::default()
        });
        world.members[0].first_child = Some(c0);
        let result = run(&mut world, -1, WUNTRACED | WCONTINUED, usize::MAX);
        assert_eq!(result.rc, 11);
        assert_eq!(result.status, 0x137F);
        assert_eq!(world.children[c0].stop_pending, 0);
        assert_eq!(
            world.children[c0].cont_pending, 1,
            "continue report stays pending while the stop wins"
        );
        assert!(!world.children[c0].released);
    }

    #[test]
    fn signal_before_park_returns_erestartsys_through_publish_order() {
        let mut world = base_world();
        let c0 = world.add_child(running(11));
        world.members[0].first_child = Some(c0);
        world.park_signal = 1;
        world.park_reason = EVENT;
        let result = run(&mut world, -1, 0, usize::MAX);
        assert_eq!(result.rc, -ERESTARTSYS);
        let publish = world.events.iter().position(|e| *e == "park_publish").unwrap();
        let unlock = world.events.iter().position(|e| *e == "unlock").unwrap();
        let commit = world.events.iter().position(|e| *e == "park_commit").unwrap();
        let clear = world.events.iter().position(|e| *e == "park_clear").unwrap();
        assert!(publish < unlock, "registration is published before unlocking");
        assert!(unlock < commit, "tasklist_lock is released before commit");
        assert!(commit < clear);
    }

    #[test]
    fn task_interrupt_wake_reason_returns_erestartsys() {
        let mut world = base_world();
        let c0 = world.add_child(running(11));
        world.members[0].first_child = Some(c0);
        world.park_reason = WAKE_SIGNAL;
        let result = run(&mut world, -1, 0, usize::MAX);
        assert_eq!(result.rc, -ERESTARTSYS);
    }

    unsafe fn exit_child_on_commit(world: *mut MockWorld) {
        // SAFETY: the hook runs from the mock commit with the world live.
        unsafe {
            let world = &mut *world;
            world.children[0].state = WAIT_STATE_ZOMBIE;
            world.children[0].group_dead = true;
        }
    }

    #[test]
    fn park_event_rescans_and_reaps_the_child_that_exited() {
        let mut world = base_world();
        let c0 = world.add_child(running(11));
        world.members[0].first_child = Some(c0);
        world.park_reason = EVENT;
        world.park_commit_hook = Some(exit_child_on_commit);
        let result = run(&mut world, -1, 0, usize::MAX);
        assert_eq!(result.rc, 11);
        assert!(world.children[c0].released);
        assert!(world.events.iter().filter(|e| **e == "park_commit").count() == 1);
    }

    #[test]
    fn exit_and_stop_status_encoding_matches_wait_formula() {
        assert_eq!(exit_status(0), 0);
        assert_eq!(exit_status(7), 0x700);
        assert_eq!(exit_status(255), 0xFF00);
        assert_eq!(exit_status(300), 0x2C00);
        assert_eq!(exit_status(-9), 9);
        assert_eq!(exit_status(-137), 137);
        let view = Wait4ReportView {
            state: WAIT_STATE_STOPPED,
            options: WUNTRACED,
            stop_report_pending: 1,
            continue_report_pending: 0,
            ptrace_stop_active: 0,
            ptrace_event: 0,
            exit_code: 19,
            _reserved: 0,
        };
        let mut status = 0;
        let action = unsafe { a20_wait4_report_decide(&view, &mut status) };
        assert_eq!(action, WAIT4_REPORT_STOP | WAIT4_REPORT_CLEAR_STOP);
        assert_eq!(status, 0x137F);
        let mut status = 0;
        let cont = Wait4ReportView {
            state: WAIT_STATE_RUNNING,
            options: WCONTINUED,
            stop_report_pending: 0,
            continue_report_pending: 1,
            ptrace_stop_active: 0,
            ptrace_event: 0,
            exit_code: 0,
            _reserved: 0,
        };
        let action = unsafe { a20_wait4_report_decide(&cont, &mut status) };
        assert_eq!(action, WAIT4_REPORT_CONT | WAIT4_REPORT_CLEAR_CONT);
        assert_eq!(status, 0xFFFF);
    }
}
