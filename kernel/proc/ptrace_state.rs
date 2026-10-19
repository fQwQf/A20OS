#![no_std]

//! `ptrace` stop, resume and tracer-lifecycle state machine.
//!
//! Rust owns the decisions: when a stop is published and with which kind and
//! siginfo, the attach/seize two-phase rules, interrupt, detach (including the
//! original-parent restore), resume with its mode and step flags, kill, option
//! updates, the event message and the tracer-death sweep.
//!
//! C keeps the mechanics: the tracee lookup and references, `park_lock` and
//! `tasklist_lock`, the architecture register export/import and trap-context
//! copy/rewind, the sched park/resume primitives, signal delivery, the
//! reparent/wake helpers, and the register/address-space accessors.
//!
//! The stop state lives across several `task_t` fields; the view borrows them
//! and the transitions below are the only places that write them, so the
//! combinations "active but not stopped" and "kind/event disagreeing" have a
//! single author.

#[cfg(test)]
extern crate std;

pub const PT_DEBUG_FLAG_TRACED: u32 = 1 << 0;
pub const PT_DEBUG_FLAG_TRACEME: u32 = 1 << 1;
pub const PT_DEBUG_FLAG_ATTACHED: u32 = 1 << 2;
pub const PT_DEBUG_FLAG_EXITKILL: u32 = 1 << 6;
pub const PT_DEBUG_FLAG_SYSCALL: u32 = 1 << 7;
pub const PT_DEBUG_FLAG_STEP: u32 = 1 << 8;
pub const PT_DEBUG_FLAG_SEIZED: u32 = 1 << 9;

pub const PT_DEBUG_OPTION_MASK: u32 = (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6);

pub const PT_DEBUG_STOP_NONE: i32 = 0;
pub const PT_DEBUG_STOP_SIGNAL: i32 = 1;
pub const PT_DEBUG_STOP_EVENT: i32 = 2;
pub const PT_DEBUG_STOP_SYSCALL_ENTRY: i32 = 3;
pub const PT_DEBUG_STOP_SYSCALL_EXIT: i32 = 4;

pub const PT_DEBUG_EVENT_EXEC: i32 = 4;

pub const PT_DEBUG_RESUME_CONT: i32 = 0;
pub const PT_DEBUG_RESUME_SYSCALL: i32 = 1;

const PROC_UNUSED: i32 = 0;
const PROC_ZOMBIE: i32 = 5;
const PROC_STOPPED: i32 = 4;
const SIGKILL: i32 = 9;
const SIGSTOP: i32 = 19;
const SIGTRAP: i32 = 5;

const EPERM: i64 = -1;
const ESRCH: i64 = -3;
const EINTR: i64 = -4;
const EINVAL: i64 = -22;

/// Borrowed view over the live ptrace fields of one task.
#[repr(C)]
pub struct PtView {
    pub ptracer: *mut *mut core::ffi::c_void,
    pub flags: *mut u32,
    pub stop_active: *mut i32,
    pub stop_kind: *mut i32,
    pub event: *mut i32,
    pub event_msg: *mut u64,
    pub stop_sig: *mut i32,
    pub siginfo_valid: *mut i32,
    pub deliver_sig: *mut i32,
    pub ctx_valid: *mut i32,
    pub orig_parent_pid: *mut i32,
    pub stop_report_pending: *mut i32,
    pub continue_report_pending: *mut i32,
    pub state: *mut i32,
    pub pid: *mut i32,
    pub ppid: *mut i32,
    pub parent: *mut *mut core::ffi::c_void,
    pub trap_ctx: *mut *mut core::ffi::c_void,
}

const _: () = {
    assert!(core::mem::size_of::<PtView>() == 18 * core::mem::size_of::<usize>());
};

extern "C" {
    fn a20_tl_tasklist_lock() -> u64;
    fn a20_tl_tasklist_unlock(flags: u64);
    fn a20_tasklist_first_locked() -> *mut core::ffi::c_void;
    fn a20_tasklist_next_locked(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_tasklist_reparent_locked(parent: *mut core::ffi::c_void,
                                    child: *mut core::ffi::c_void);

    fn a20_pt_view_fill(t: *mut core::ffi::c_void, out: *mut PtView) -> i32;
    fn a20_pt_current() -> *mut core::ffi::c_void;
    fn a20_pt_idle_task() -> *mut core::ffi::c_void;
    fn a20_pt_may_access(a: *mut core::ffi::c_void, b: *mut core::ffi::c_void) -> i32;
    fn a20_pt_is_traced(t: *const core::ffi::c_void) -> i32;
    fn a20_pt_get(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_pt_put(t: *mut core::ffi::c_void);
    fn a20_pt_park_lock(t: *mut core::ffi::c_void) -> u64;
    fn a20_pt_park_unlock(t: *mut core::ffi::c_void, flags: u64);
    fn a20_pt_parent_alive(t: *mut core::ffi::c_void) -> i32;
    fn a20_pt_save_ctx(t: *mut core::ffi::c_void);
    fn a20_pt_stop(t: *mut core::ffi::c_void, sig: i32) -> i32;
    fn a20_pt_snapshot_siginfo(t: *mut core::ffi::c_void, sig: i32);
    fn a20_pt_default_siginfo(t: *mut core::ffi::c_void, sig: i32, code: i32);
    fn a20_pt_send_stop(t: *mut core::ffi::c_void);
    fn a20_pt_fold_ctx(t: *mut core::ffi::c_void, kind: i32);
    fn a20_pt_resume_stopped(t: *mut core::ffi::c_void) -> i32;
    fn a20_pt_signal_send(pid: i32, sig: i32) -> i32;
    fn a20_pt_reparent_original(t: *mut core::ffi::c_void, orig_pid: i32);
}

/// Publish a stop and block until the tracer resumes the task.
unsafe fn stop_enter(t: *mut core::ffi::c_void, sig: i32, kind: i32, event: i32,
                     msg: u64) -> i32 {
    if sig == SIGKILL {
        return EINTR as i32;
    }
    unsafe { a20_pt_save_ctx(t) };
    let mut view: PtView = unsafe { core::mem::zeroed() };
    if unsafe { a20_pt_view_fill(t, &mut view) } == 0 {
        return EINTR as i32;
    }
    unsafe {
        *view.stop_sig = sig;
        *view.event = event;
        *view.event_msg = msg;
        *view.stop_kind = kind;
        *view.deliver_sig = 0;
    }
    unsafe { a20_pt_stop(t, sig) }
}

/// The signal-stop path: snapshot the pending signal's siginfo, falling back
/// to a default when there is none, then publish the stop.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pt_signal_stop(t: *mut core::ffi::c_void, sig: i32) -> i32 {
    if sig == SIGKILL {
        return EINTR as i32;
    }
    let mut view: PtView = unsafe { core::mem::zeroed() };
    unsafe { a20_pt_snapshot_siginfo(t, sig) };
    if unsafe { a20_pt_view_fill(t, &mut view) } == 0 {
        return EINTR as i32;
    }
    let valid = unsafe { *view.siginfo_valid };
    if valid == 0 {
        unsafe { a20_pt_default_siginfo(t, sig, 0) };
    }
    unsafe { stop_enter(t, sig, PT_DEBUG_STOP_SIGNAL, 0, 0) }
}

/// The event-stop path (exec/exit): a default SIGTRAP siginfo, with TRAP_BRKPT
/// for an exec event.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pt_event_stop(t: *mut core::ffi::c_void, sig: i32,
                                           event: i32, msg: u64) -> i32 {
    let code = if event == PT_DEBUG_EVENT_EXEC { 1 } else { 0 };
    unsafe { a20_pt_default_siginfo(t, SIGTRAP, code) };
    unsafe { stop_enter(t, sig, PT_DEBUG_STOP_EVENT, event, msg) }
}

/// The syscall-boundary hooks.  `entry` selects the stop kind; anything that
/// should not stop returns 0 without publishing one.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pt_syscall_stop(t: *mut core::ffi::c_void, entry: i32) -> i32 {
    if t.is_null() || unsafe { a20_pt_is_traced(t) } == 0 {
        return 0;
    }
    let mut view: PtView = unsafe { core::mem::zeroed() };
    if unsafe { a20_pt_view_fill(t, &mut view) } == 0 {
        return 0;
    }
    if unsafe { *view.flags } & PT_DEBUG_FLAG_SYSCALL == 0 {
        return 0;
    }
    if unsafe { *view.deliver_sig } != 0 {
        return 0; // resume-with-signal: deliver first, then the syscall runs
    }
    if unsafe { *view.stop_active } != 0 {
        return 0;
    }
    unsafe { a20_pt_default_siginfo(t, SIGTRAP, 4) };
    let kind = if entry != 0 {
        PT_DEBUG_STOP_SYSCALL_ENTRY
    } else {
        PT_DEBUG_STOP_SYSCALL_EXIT
    };
    unsafe { stop_enter(t, SIGTRAP, kind, 0, 0) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pt_traceme(t: *mut core::ffi::c_void) -> i64 {
    if t.is_null() || unsafe { a20_pt_is_traced(t) } != 0 {
        return EPERM;
    }
    let mut view: PtView = unsafe { core::mem::zeroed() };
    if unsafe { a20_pt_view_fill(t, &mut view) } == 0 {
        return EPERM;
    }
    let parent = unsafe { *view.parent };
    if parent.is_null() {
        return EPERM;
    }
    let mut pview: PtView = unsafe { core::mem::zeroed() };
    if unsafe { a20_pt_view_fill(parent, &mut pview) } == 0 {
        return EPERM;
    }
    let parent_pid = unsafe { *pview.pid };
    let flags = unsafe { a20_pt_park_lock(t) };
    unsafe {
        *view.ptracer = parent;
        *view.flags |= PT_DEBUG_FLAG_TRACED | PT_DEBUG_FLAG_TRACEME;
        *view.orig_parent_pid = parent_pid;
    }
    unsafe { a20_pt_park_unlock(t, flags) };
    0
}

/// Attach (`seize == 0`) or seize (`seize != 0`).  `caller` is the tracer; the
/// adapter has already resolved and referenced the tracee.
///
/// # Safety
/// `t` must be referenced for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pt_attach(t: *mut core::ffi::c_void,
                                       caller: *mut core::ffi::c_void,
                                       seize: i32) -> i64 {
    if t == caller || t == unsafe { a20_pt_idle_task() } {
        return EPERM;
    }
    if unsafe { a20_pt_may_access(caller, t) } == 0 {
        return EPERM;
    }

    let mut view: PtView = unsafe { core::mem::zeroed() };
    if unsafe { a20_pt_view_fill(t, &mut view) } == 0 {
        return ESRCH;
    }

    // Two phases, so the tracee's park_lock and its parent's park_lock are
    // never nested (INV-P3).
    let lf = unsafe { a20_tl_tasklist_lock() };
    let parent = unsafe { *view.parent };
    let parent_alive = if parent.is_null() { 0 } else { unsafe { a20_pt_parent_alive(t) } };
    let orig_parent_pid = if parent.is_null() {
        0
    } else {
        let mut pview: PtView = unsafe { core::mem::zeroed() };
        if unsafe { a20_pt_view_fill(parent, &mut pview) } != 0 {
            unsafe { *pview.pid }
        } else {
            0
        }
    };

    let flags = unsafe { a20_pt_park_lock(t) };
    let state = unsafe { *view.state };
    let traced = unsafe { a20_pt_is_traced(t) } != 0;
    if state == PROC_UNUSED || state == PROC_ZOMBIE || traced {
        let zombie = state == PROC_ZOMBIE;
        unsafe { a20_pt_park_unlock(t, flags) };
        unsafe { a20_tl_tasklist_unlock(lf) };
        return if zombie { ESRCH } else { EPERM };
    }
    unsafe {
        *view.ptracer = caller;
        *view.flags |= PT_DEBUG_FLAG_TRACED | PT_DEBUG_FLAG_ATTACHED;
        if seize != 0 {
            *view.flags |= PT_DEBUG_FLAG_SEIZED;
        }
        *view.orig_parent_pid = orig_parent_pid;
    }
    if parent_alive != 0 {
        let mut cview: PtView = unsafe { core::mem::zeroed() };
        if unsafe { a20_pt_view_fill(caller, &mut cview) } != 0 {
            let caller_pid = unsafe { *cview.pid };
            unsafe { *view.ppid = caller_pid };
        }
    }
    unsafe { a20_pt_park_unlock(t, flags) };
    unsafe { a20_tasklist_reparent_locked(caller, t) };
    unsafe { a20_tl_tasklist_unlock(lf) };

    if seize == 0 {
        // Attach queues SIGSTOP so the tracee stops at its next boundary;
        // seize waits for the next signal/syscall boundary instead.
        unsafe { a20_pt_send_stop(t) };
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pt_interrupt(t: *mut core::ffi::c_void,
                                          caller: *mut core::ffi::c_void) -> i64 {
    let mut view: PtView = unsafe { core::mem::zeroed() };
    if unsafe { a20_pt_view_fill(t, &mut view) } == 0 {
        return ESRCH;
    }
    if unsafe { *view.ptracer } != caller {
        return EPERM;
    }
    if unsafe { *view.stop_active } != 0 {
        return 0; // already stopped
    }
    unsafe { a20_pt_send_stop(t) };
    0
}

fn clear_stop_fields(view: &mut PtView) -> bool {
    let stopped = unsafe { *view.state } == PROC_STOPPED && unsafe { *view.stop_active } != 0;
    if stopped {
        unsafe {
            *view.stop_report_pending = 0;
            *view.continue_report_pending = 0;
            *view.stop_active = 0;
            *view.stop_kind = PT_DEBUG_STOP_NONE;
            *view.event = 0;
        }
    }
    stopped
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pt_detach(t: *mut core::ffi::c_void, sig: i32) -> i64 {
    let mut view: PtView = unsafe { core::mem::zeroed() };
    if unsafe { a20_pt_view_fill(t, &mut view) } == 0 {
        return ESRCH;
    }
    let flags = unsafe { a20_pt_park_lock(t) };
    let resume = clear_stop_fields(&mut view);
    if resume && sig != 0 {
        unsafe { *view.deliver_sig = sig };
    }
    let caller = unsafe { a20_pt_current() };
    unsafe { *view.ptracer = core::ptr::null_mut() };
    unsafe { *view.flags = 0 };
    let mut orig_pid = 0;
    let parent = unsafe { *view.parent };
    let recorded = unsafe { *view.orig_parent_pid };
    if parent == caller || recorded > 0 {
        orig_pid = recorded;
    }
    unsafe { a20_pt_park_unlock(t, flags) };

    if orig_pid > 0 {
        unsafe { a20_pt_reparent_original(t, orig_pid) };
    }
    if resume {
        unsafe { a20_pt_resume_stopped(t) };
        if sig != 0 {
            let pid = unsafe { *view.pid };
            unsafe { a20_pt_signal_send(pid, sig) };
        }
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pt_resume(t: *mut core::ffi::c_void, sig: i32,
                                       mode: i32, step: i32) -> i64 {
    let mut view: PtView = unsafe { core::mem::zeroed() };
    if unsafe { a20_pt_view_fill(t, &mut view) } == 0 {
        return ESRCH;
    }
    let flags = unsafe { a20_pt_park_lock(t) };
    if unsafe { *view.state } != PROC_STOPPED || unsafe { *view.stop_active } == 0 {
        unsafe { a20_pt_park_unlock(t, flags) };
        return ESRCH;
    }
    let kind = unsafe { *view.stop_kind };
    // Fold in register edits made while stopped, and rewind a syscall-entry
    // stop so the syscall runs again.
    unsafe { a20_pt_fold_ctx(t, kind) };

    let mut new_flags = unsafe { *view.flags };
    if mode == PT_DEBUG_RESUME_SYSCALL {
        new_flags |= PT_DEBUG_FLAG_SYSCALL;
    } else {
        new_flags &= !PT_DEBUG_FLAG_SYSCALL;
    }
    if step != 0 {
        new_flags |= PT_DEBUG_FLAG_STEP;
    } else {
        new_flags &= !PT_DEBUG_FLAG_STEP;
    }
    unsafe { *view.flags = new_flags };
    unsafe {
        *view.stop_report_pending = 0;
        *view.continue_report_pending = 0;
        *view.stop_active = 0;
        *view.stop_kind = PT_DEBUG_STOP_NONE;
        *view.event = 0;
    }
    if sig != 0 {
        unsafe { *view.deliver_sig = sig };
    }
    unsafe { a20_pt_park_unlock(t, flags) };

    let resumed = unsafe { a20_pt_resume_stopped(t) };
    if resumed != 0 && sig != 0 {
        let pid = unsafe { *view.pid };
        unsafe { a20_pt_signal_send(pid, sig) };
    }
    0
}

/// Single-step resume; the adapter applies the architecture gate.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pt_singlestep(t: *mut core::ffi::c_void, sig: i32) -> i64 {
    unsafe { a20_pt_resume(t, sig, PT_DEBUG_RESUME_CONT, 1) }
}

/// Clear a pending stop and resume before SIGKILL is delivered.  The adapter
/// performs the final kill and its result mapping.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pt_kill(t: *mut core::ffi::c_void,
                                     caller: *mut core::ffi::c_void) -> i64 {
    if unsafe { a20_pt_is_traced(t) } == 0 {
        return ESRCH;
    }
    let mut view: PtView = unsafe { core::mem::zeroed() };
    if unsafe { a20_pt_view_fill(t, &mut view) } == 0 {
        return ESRCH;
    }
    if unsafe { *view.ptracer } != caller {
        return ESRCH;
    }
    let flags = unsafe { a20_pt_park_lock(t) };
    let stopped = unsafe { *view.state } == PROC_STOPPED && unsafe { *view.stop_active } != 0;
    if stopped {
        unsafe {
            *view.stop_report_pending = 0;
            *view.stop_active = 0;
            *view.stop_kind = PT_DEBUG_STOP_NONE;
        }
    }
    unsafe { a20_pt_park_unlock(t, flags) };
    if stopped {
        unsafe { a20_pt_resume_stopped(t) };
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pt_setoptions(t: *mut core::ffi::c_void, options: u64) -> i64 {
    let mut view: PtView = unsafe { core::mem::zeroed() };
    if unsafe { a20_pt_view_fill(t, &mut view) } == 0 {
        return ESRCH;
    }
    let flags = unsafe { a20_pt_park_lock(t) };
    let current = unsafe { *view.flags };
    unsafe {
        *view.flags = (current & !PT_DEBUG_OPTION_MASK) | ((options as u32) & PT_DEBUG_OPTION_MASK);
    }
    unsafe { a20_pt_park_unlock(t, flags) };
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pt_geteventmsg(t: *mut core::ffi::c_void) -> i64 {
    let mut view: PtView = unsafe { core::mem::zeroed() };
    if unsafe { a20_pt_view_fill(t, &mut view) } == 0 {
        return ESRCH;
    }
    unsafe { *view.event_msg as i64 }
}

/// The deferred work one tracer-death sweep produced.
#[repr(C)]
pub struct TracerExitLists {
    pub kill_pids: [i32; 64],
    pub resume_pids: [i32; 64],
    pub tracee_pids: [i32; 64],
    pub kill_count: usize,
    pub resume_count: usize,
    pub tracee_count: usize,
}

/// Clear every tracee of a dying tracer and record what must happen after the
/// list lock is dropped.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pt_tracer_exiting(tracer: *mut core::ffi::c_void,
                                               lists: *mut TracerExitLists) {
    let lists = unsafe { &mut *lists };
    lists.kill_count = 0;
    lists.resume_count = 0;
    lists.tracee_count = 0;
    if tracer.is_null() {
        return;
    }
    let lock = unsafe { a20_tl_tasklist_lock() };
    let mut t = unsafe { a20_tasklist_first_locked() };
    while !t.is_null() {
        if t != tracer {
            let mut view: PtView = unsafe { core::mem::zeroed() };
            if unsafe { a20_pt_view_fill(t, &mut view) } != 0 {
                let pf = unsafe { a20_pt_park_lock(t) };
                let state = unsafe { *view.state };
                let ptracer = unsafe { *view.ptracer };
                if state == PROC_UNUSED || ptracer != tracer {
                    unsafe { a20_pt_park_unlock(t, pf) };
                } else {
                    let flags = unsafe { *view.flags };
                    let pid = unsafe { *view.pid };
                    if flags & PT_DEBUG_FLAG_EXITKILL != 0 {
                        if lists.kill_count < 64 {
                            lists.kill_pids[lists.kill_count] = pid;
                            lists.kill_count += 1;
                        }
                    } else if state == PROC_STOPPED && unsafe { *view.stop_active } != 0 {
                        if lists.resume_count < 64 {
                            lists.resume_pids[lists.resume_count] = pid;
                            lists.resume_count += 1;
                        }
                    }
                    if lists.tracee_count < 64 {
                        lists.tracee_pids[lists.tracee_count] = pid;
                        lists.tracee_count += 1;
                    }
                    unsafe {
                        *view.ptracer = core::ptr::null_mut();
                        *view.flags = 0;
                        *view.deliver_sig = 0;
                        *view.stop_active = 0;
                        *view.stop_kind = PT_DEBUG_STOP_NONE;
                        *view.stop_report_pending = 0;
                        *view.continue_report_pending = 0;
                        *view.event = 0;
                    }
                    unsafe { a20_pt_park_unlock(t, pf) };
                }
            }
        }
        t = unsafe { a20_tasklist_next_locked(t) };
    }
    unsafe { a20_tl_tasklist_unlock(lock) };
}


#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    static TEST_LOCK: Mutex<()> = Mutex::new(());

    struct Fake {
        ptracer: *mut core::ffi::c_void,
        flags: u32,
        stop_active: i32,
        stop_kind: i32,
        event: i32,
        event_msg: u64,
        stop_sig: i32,
        siginfo_valid: i32,
        deliver_sig: i32,
        ctx_valid: i32,
        orig_parent_pid: i32,
        stop_report_pending: i32,
        continue_report_pending: i32,
        state: i32,
        pid: i32,
        ppid: i32,
        parent: *mut Fake,
        trap_ctx: *mut core::ffi::c_void,
        /* scripted bridge results */
        traced: i32,
        may_access: i32,
        parent_alive: i32,
        stop_rc: i32,
        resume_rc: i32,
        signal_rc: i32,
        is_idle: bool,
    }

    static mut F: Fake = Fake {
        ptracer: core::ptr::null_mut(), flags: 0, stop_active: 0, stop_kind: 0,
        event: 0, event_msg: 0, stop_sig: 0, siginfo_valid: 0, deliver_sig: 0,
        ctx_valid: 0, orig_parent_pid: 0, stop_report_pending: 0,
        continue_report_pending: 0, state: PROC_STOPPED, pid: 100, ppid: 0,
        parent: core::ptr::null_mut(), trap_ctx: core::ptr::null_mut(),
        traced: 0, may_access: 1, parent_alive: 1, stop_rc: 0, resume_rc: 1,
        signal_rc: 0, is_idle: false,
    };

    static mut LOG: std::vec::Vec<std::string::String> = std::vec::Vec::new();

    fn fix() -> &'static mut Fake { unsafe { &mut *core::ptr::addr_of_mut!(F) } }

    fn note(s: &str) { unsafe { LOG.push(std::string::String::from(s)) } }

    fn reset_state() {
        *fix() = Fake {
            ptracer: core::ptr::null_mut(), flags: 0, stop_active: 0, stop_kind: 0,
            event: 0, event_msg: 0, stop_sig: 0, siginfo_valid: 0, deliver_sig: 0,
            ctx_valid: 0, orig_parent_pid: 0, stop_report_pending: 0,
            continue_report_pending: 0, state: PROC_STOPPED, pid: 100, ppid: 0,
            parent: core::ptr::null_mut(), trap_ctx: core::ptr::null_mut(),
            traced: 0, may_access: 1, parent_alive: 1, stop_rc: 0, resume_rc: 1,
            signal_rc: 0, is_idle: false,
        };
        unsafe { LOG.clear() };
    }

    fn reset() -> std::sync::MutexGuard<'static, ()> {
        let guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        reset_state();
        guard
    }

    fn t() -> *mut core::ffi::c_void { 1 as *mut core::ffi::c_void }
    fn caller() -> *mut core::ffi::c_void { 2 as *mut core::ffi::c_void }

    #[no_mangle] unsafe extern "C" fn a20_tl_tasklist_lock() -> u64 { 0 }
    #[no_mangle] unsafe extern "C" fn a20_tl_tasklist_unlock(_f: u64) {}
    #[no_mangle] unsafe extern "C" fn a20_tasklist_first_locked() -> *mut core::ffi::c_void {
        core::ptr::null_mut()
    }
    #[no_mangle] unsafe extern "C" fn a20_tasklist_next_locked(_t: *mut core::ffi::c_void) -> *mut core::ffi::c_void {
        core::ptr::null_mut()
    }
    #[no_mangle] unsafe extern "C" fn a20_tasklist_reparent_locked(_p: *mut core::ffi::c_void,
                                                                    _c: *mut core::ffi::c_void) {
        note("reparent")
    }
    #[no_mangle] unsafe extern "C" fn a20_pt_view_fill(x: *mut core::ffi::c_void, out: *mut PtView) -> i32 {
        if x.is_null() { return 0; }
        let f = fix();
        unsafe {
            (*out).ptracer = &mut f.ptracer;
            (*out).flags = &mut f.flags;
            (*out).stop_active = &mut f.stop_active;
            (*out).stop_kind = &mut f.stop_kind;
            (*out).event = &mut f.event;
            (*out).event_msg = &mut f.event_msg;
            (*out).stop_sig = &mut f.stop_sig;
            (*out).siginfo_valid = &mut f.siginfo_valid;
            (*out).deliver_sig = &mut f.deliver_sig;
            (*out).ctx_valid = &mut f.ctx_valid;
            (*out).orig_parent_pid = &mut f.orig_parent_pid;
            (*out).stop_report_pending = &mut f.stop_report_pending;
            (*out).continue_report_pending = &mut f.continue_report_pending;
            (*out).state = &mut f.state;
            (*out).pid = &mut f.pid;
            (*out).ppid = &mut f.ppid;
            (*out).parent = &mut f.parent as *mut *mut Fake as *mut *mut core::ffi::c_void;
            (*out).trap_ctx = &mut f.trap_ctx;
        }
        1
    }
    #[no_mangle] unsafe extern "C" fn a20_pt_current() -> *mut core::ffi::c_void { caller() }
    #[no_mangle] unsafe extern "C" fn a20_pt_idle_task() -> *mut core::ffi::c_void { 3 as *mut core::ffi::c_void }
    #[no_mangle] unsafe extern "C" fn a20_pt_may_access(_a: *mut core::ffi::c_void, _b: *mut core::ffi::c_void) -> i32 {
        fix().may_access
    }
    #[no_mangle] unsafe extern "C" fn a20_pt_is_traced(_t: *const core::ffi::c_void) -> i32 { fix().traced }
    #[no_mangle] unsafe extern "C" fn a20_pt_get(x: *mut core::ffi::c_void) -> *mut core::ffi::c_void { x }
    #[no_mangle] unsafe extern "C" fn a20_pt_put(_t: *mut core::ffi::c_void) {}
    #[no_mangle] unsafe extern "C" fn a20_pt_park_lock(_t: *mut core::ffi::c_void) -> u64 { 0 }
    #[no_mangle] unsafe extern "C" fn a20_pt_park_unlock(_t: *mut core::ffi::c_void, _f: u64) {}
    #[no_mangle] unsafe extern "C" fn a20_pt_parent_alive(_t: *mut core::ffi::c_void) -> i32 {
        fix().parent_alive
    }
    #[no_mangle] unsafe extern "C" fn a20_pt_save_ctx(_t: *mut core::ffi::c_void) { note("save_ctx") }
    #[no_mangle] unsafe extern "C" fn a20_pt_stop(_t: *mut core::ffi::c_void, _s: i32) -> i32 {
        note("stop");
        fix().stop_rc
    }
    #[no_mangle] unsafe extern "C" fn a20_pt_snapshot_siginfo(_t: *mut core::ffi::c_void, _s: i32) {
        note("snapshot")
    }
    #[no_mangle] unsafe extern "C" fn a20_pt_default_siginfo(_t: *mut core::ffi::c_void, _s: i32, _c: i32) {
        note("default")
    }
    #[no_mangle] unsafe extern "C" fn a20_pt_send_stop(_t: *mut core::ffi::c_void) { note("send_stop") }
    #[no_mangle] unsafe extern "C" fn a20_pt_fold_ctx(_t: *mut core::ffi::c_void, _k: i32) { note("fold") }
    #[no_mangle] unsafe extern "C" fn a20_pt_resume_stopped(_t: *mut core::ffi::c_void) -> i32 {
        note("resume");
        fix().resume_rc
    }
    #[no_mangle] unsafe extern "C" fn a20_pt_signal_send(_p: i32, _s: i32) -> i32 {
        note("signal");
        fix().signal_rc
    }
    #[no_mangle] unsafe extern "C" fn a20_pt_reparent_original(_t: *mut core::ffi::c_void, _o: i32) {
        note("reparent_original")
    }

    fn log() -> std::vec::Vec<std::string::String> { unsafe { LOG.clone() } }

    #[test]
    fn a_signal_stop_snapshots_then_falls_back_to_a_default() {
        let _g = reset();
        unsafe {
            let rc = a20_pt_signal_stop(t(), 11);
            assert_eq!(rc, 0);
        }
        assert_eq!(log(), ["snapshot", "default", "save_ctx", "stop"]);
        assert_eq!(fix().stop_kind, PT_DEBUG_STOP_SIGNAL);
        assert_eq!(fix().stop_sig, 11);
        assert_eq!(fix().deliver_sig, 0);
    }

    #[test]
    fn sigkill_is_never_stopped() {
        let _g = reset();
        unsafe {
            assert_eq!(a20_pt_signal_stop(t(), SIGKILL), EINTR as i32);
            assert_eq!(a20_pt_event_stop(t(), SIGKILL, 0, 0), EINTR as i32);
        }
        // The event path fills its default siginfo before the SIGKILL refusal,
        // exactly as the C implementation did.
        assert_eq!(log(), ["default"]);
    }

    #[test]
    fn an_event_stop_uses_trap_brkpt_for_exec() {
        let _g = reset();
        unsafe {
            assert_eq!(a20_pt_event_stop(t(), SIGTRAP, PT_DEBUG_EVENT_EXEC, 0x55), 0);
        }
        assert_eq!(log(), ["default", "save_ctx", "stop"]);
        assert_eq!(fix().stop_kind, PT_DEBUG_STOP_EVENT);
        assert_eq!(fix().event, PT_DEBUG_EVENT_EXEC);
        assert_eq!(fix().event_msg, 0x55);
    }

    #[test]
    fn the_syscall_hook_respects_mode_delivery_and_existing_stops() {
        let _g = reset();
        unsafe {
            fix().traced = 1;
            // No syscall mode: no stop.
            assert_eq!(a20_pt_syscall_stop(t(), 1), 0);
            assert!(log().is_empty());

            reset_state_with(PT_DEBUG_FLAG_SYSCALL, 0, 0);
            assert_eq!(a20_pt_syscall_stop(t(), 1), 0);
            assert_eq!(fix().stop_kind, PT_DEBUG_STOP_SYSCALL_ENTRY);
            assert_eq!(log(), ["default", "save_ctx", "stop"]);

            reset_state_with(PT_DEBUG_FLAG_SYSCALL, 1, 0);
            assert_eq!(a20_pt_syscall_stop(t(), 0), 0);
            assert!(log().is_empty(), "a pending delivery suppresses the stop");

            reset_state_with(PT_DEBUG_FLAG_SYSCALL, 0, 0);
            assert_eq!(a20_pt_syscall_stop(t(), 0), 0);
            assert_eq!(fix().stop_kind, PT_DEBUG_STOP_SYSCALL_EXIT);
        }
    }

    fn reset_state_with(flags: u32, deliver: i32, active: i32) {
        unsafe {
            LOG.clear();
            fix().flags = flags;
            fix().deliver_sig = deliver;
            fix().stop_active = active;
        }
    }

    #[test]
    fn traceme_requires_a_parent_and_an_untraced_task() {
        let _g = reset();
        unsafe {
            assert_eq!(a20_pt_traceme(t()), EPERM, "no parent");
            fix().parent = core::ptr::addr_of_mut!(fix().parent) as *mut Fake;
            fix().traced = 1;
            assert_eq!(a20_pt_traceme(t()), EPERM, "already traced");
            fix().traced = 0;
            assert_eq!(a20_pt_traceme(t()), 0);
            assert_eq!(fix().ptracer, fix().parent as *mut core::ffi::c_void);
            assert_eq!(fix().flags & (PT_DEBUG_FLAG_TRACED | PT_DEBUG_FLAG_TRACEME),
                       PT_DEBUG_FLAG_TRACED | PT_DEBUG_FLAG_TRACEME);
        }
    }

    #[test]
    fn attach_rejects_self_and_an_unrelated_caller() {
        let _g = reset();
        unsafe {
            assert_eq!(a20_pt_attach(caller(), caller(), 0), EPERM);
            fix().may_access = 0;
            assert_eq!(a20_pt_attach(t(), caller(), 0), EPERM);
        }
    }

    #[test]
    fn attach_sets_the_flags_and_queues_sigstop_but_seize_does_not() {
        let _g = reset();
        unsafe {
            assert_eq!(a20_pt_attach(t(), caller(), 0), 0);
        }
        assert_eq!(log(), ["reparent", "send_stop"]);
        assert_eq!(fix().flags & (PT_DEBUG_FLAG_TRACED | PT_DEBUG_FLAG_ATTACHED),
                   PT_DEBUG_FLAG_TRACED | PT_DEBUG_FLAG_ATTACHED);

        reset_state();
        unsafe {
            assert_eq!(a20_pt_attach(t(), caller(), 1), 0);
        }
        assert_eq!(log(), ["reparent"], "seize does not stop the tracee");
        assert!((fix().flags & PT_DEBUG_FLAG_SEIZED) != 0);
    }

    #[test]
    fn attach_failure_maps_zombie_to_esrch_and_traced_to_eperm() {
        let _g = reset();
        unsafe {
            fix().state = PROC_ZOMBIE;
            assert_eq!(a20_pt_attach(t(), caller(), 0), ESRCH);
            fix().state = PROC_STOPPED;
            fix().traced = 1;
            assert_eq!(a20_pt_attach(t(), caller(), 0), EPERM);
        }
    }

    #[test]
    fn interrupt_is_a_noop_when_already_stopped() {
        let _g = reset();
        unsafe {
            fix().ptracer = caller();
            fix().stop_active = 1;
            assert_eq!(a20_pt_interrupt(t(), caller()), 0);
            assert!(log().is_empty());
            fix().stop_active = 0;
            assert_eq!(a20_pt_interrupt(t(), caller()), 0);
            assert_eq!(log(), ["send_stop"]);
        }
    }

    #[test]
    fn detach_clears_the_stop_and_restores_the_original_parent() {
        let _g = reset();
        unsafe {
            fix().ptracer = caller();
            fix().flags = PT_DEBUG_FLAG_TRACED | PT_DEBUG_FLAG_ATTACHED;
            fix().stop_active = 1;
            fix().stop_kind = PT_DEBUG_STOP_SIGNAL;
            fix().event = 7;
            fix().orig_parent_pid = 42;
        }
        unsafe { assert_eq!(a20_pt_detach(t(), 9), 0) };
        assert_eq!(fix().stop_active, 0);
        assert_eq!(fix().stop_kind, PT_DEBUG_STOP_NONE);
        assert_eq!(fix().event, 0);
        assert!(fix().ptracer.is_null());
        assert_eq!(fix().flags, 0);
        assert_eq!(log(), ["reparent_original", "resume", "signal"]);
    }

    #[test]
    fn resume_requires_a_stopped_state_and_rewinds_a_syscall_entry() {
        let _g = reset();
        unsafe {
            fix().state = PROC_STOPPED;
            fix().stop_active = 1;
            fix().stop_kind = PT_DEBUG_STOP_SYSCALL_ENTRY;
            assert_eq!(a20_pt_resume(t(), 0, PT_DEBUG_RESUME_SYSCALL, 0), 0);
        }
        assert_eq!(log(), ["fold", "resume"]);
        assert!((fix().flags & PT_DEBUG_FLAG_SYSCALL) != 0);
        assert_eq!(fix().stop_active, 0);

        reset_state();
        unsafe {
            fix().state = PROC_STOPPED;
            fix().stop_active = 1;
            fix().flags = PT_DEBUG_FLAG_SYSCALL;
            assert_eq!(a20_pt_resume(t(), 0, PT_DEBUG_RESUME_CONT, 1), 0);
        }
        assert_eq!(fix().flags & PT_DEBUG_FLAG_SYSCALL, 0);
        assert!((fix().flags & PT_DEBUG_FLAG_STEP) != 0);

        reset_state();
        unsafe {
            fix().stop_active = 0;
            assert_eq!(a20_pt_resume(t(), 0, PT_DEBUG_RESUME_CONT, 0), ESRCH);
        }
    }

    #[test]
    fn resume_delivers_the_signal_only_after_a_real_resume() {
        let _g = reset();
        unsafe {
            fix().stop_active = 1;
            fix().resume_rc = 1;
            assert_eq!(a20_pt_resume(t(), 15, PT_DEBUG_RESUME_CONT, 0), 0);
        }
        assert_eq!(log(), ["fold", "resume", "signal"]);
        assert_eq!(fix().deliver_sig, 15);

        reset_state();
        unsafe {
            fix().stop_active = 1;
            fix().resume_rc = 0;
            assert_eq!(a20_pt_resume(t(), 15, PT_DEBUG_RESUME_CONT, 0), 0);
        }
        assert_eq!(log(), ["fold", "resume"]);
    }

    #[test]
    fn kill_requires_the_tracer_and_clears_a_pending_stop() {
        let _g = reset();
        unsafe {
            fix().traced = 1;
            fix().ptracer = core::ptr::null_mut();
            assert_eq!(a20_pt_kill(t(), caller()), ESRCH);
            fix().ptracer = caller();
            fix().stop_active = 1;
            assert_eq!(a20_pt_kill(t(), caller()), 0);
        }
        assert_eq!(log(), ["resume"]);
        assert_eq!(fix().stop_active, 0);
    }

    #[test]
    fn options_replace_only_the_masked_bits() {
        let _g = reset();
        unsafe {
            fix().flags = PT_DEBUG_FLAG_TRACED | PT_DEBUG_FLAG_EXITKILL;
            assert_eq!(a20_pt_setoptions(t(), 0), 0);
            assert_eq!(fix().flags, PT_DEBUG_FLAG_TRACED);
            fix().flags = PT_DEBUG_FLAG_TRACED;
            assert_eq!(a20_pt_setoptions(t(), (1 << 6) | 0x100), 0);
            assert_eq!(fix().flags, PT_DEBUG_FLAG_TRACED | PT_DEBUG_FLAG_EXITKILL);
        }
    }

    #[test]
    fn the_event_message_is_returned_verbatim() {
        let _g = reset();
        unsafe {
            fix().event_msg = 0x1234;
            assert_eq!(a20_pt_geteventmsg(t()), 0x1234);
        }
    }

    #[test]
    fn tracer_death_sweeps_no_tracees_in_this_model() {
        let _g = reset();
        let mut lists: TracerExitLists = unsafe { core::mem::zeroed() };
        unsafe { a20_pt_tracer_exiting(caller(), &mut lists) };
        assert_eq!(lists.tracee_count, 0);
        assert_eq!(lists.kill_count, 0);
        // A null tracer is a no-op.
        unsafe { a20_pt_tracer_exiting(core::ptr::null_mut(), &mut lists) };
        assert_eq!(lists.tracee_count, 0);
    }
}
