#![no_std]

//! Orphan reparenting and pdeathsig delivery policy.
//!
//! When a task exits, Rust owns what happens to its children: electing the
//! thread-group reaper, choosing the actual reaper (the live group member, the
//! caller's reaper, or the idle task), deciding which group-dead zombies are
//! detached and destroyed, claiming `PR_SET_PDEATHSIG` signals exactly once,
//! moving each surviving child
//! under the reaper, and recording the first reaper whose zombie child needs a
//! waiter wake.
//!
//! C keeps every mechanical operation: the `tasklist_lock` and per-task
//! `park_lock`, the task references, the group-dead walk, the reparent and
//! unlink primitives, the zombie detach, the destruction and the deferred
//! signal/wake calls.
//!
//! The decision is deliberately split from the deferred calls: the C adapter
//! runs the wake, the destroy-and-put and the signal sends
//! only after the list lock has been dropped, in that order.

#[cfg(test)]
extern crate std;

pub const PROC_UNUSED: i32 = 0;
pub const PROC_ZOMBIE: i32 = 5;
const SIGCHLD: i32 = 17;
#[cfg(test)]
const SIGSTOP: i32 = 19;
const CLONE_THREAD: u64 = 0x0001_0000;
const MAX_LIST: usize = 64;

/// The deferred work one reparent pass produced.  The lists are filled in the
/// same order the C code recorded them.
#[repr(C)]
pub struct ReparentLists {
    pub to_destroy: [*mut core::ffi::c_void; MAX_LIST],
    pub pdeathsig_pids: [i32; MAX_LIST],
    pub pdeathsig_signals: [i32; MAX_LIST],
    // Reserved C/Rust ABI storage; automatic descendant killing is disabled.
    pub force_kill_pids: [i32; MAX_LIST],
}

#[repr(C)]
pub struct ReparentResult {
    pub wake_for: *mut core::ffi::c_void,
    pub destroy_count: usize,
    pub pdeathsig_count: usize,
    // Always zero. Only explicit pdeathsig requests propagate parent death.
    pub force_kill_count: usize,
}

extern "C" {
    fn a20_tl_tasklist_lock() -> u64;
    fn a20_tl_tasklist_unlock(flags: u64);
    fn a20_tasklist_reparent_locked(parent: *mut core::ffi::c_void,
                                    child: *mut core::ffi::c_void);
    fn a20_ex_pid(t: *const core::ffi::c_void) -> i32;
    fn a20_ex_tgid(t: *const core::ffi::c_void) -> i32;
    fn a20_ex_tg_leader(t: *const core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_ex_tg_next(t: *const core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_ex_exit_signal(t: *const core::ffi::c_void) -> i32;
    fn a20_ex_clone_flags(t: *const core::ffi::c_void) -> u64;
    fn a20_ex_children(t: *const core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_ex_sibling_next(t: *const core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_ex_idle_task() -> *mut core::ffi::c_void;
    fn a20_ex_state(t: *mut core::ffi::c_void) -> i32;
    fn a20_ex_park_lock(t: *mut core::ffi::c_void) -> u64;
    fn a20_ex_park_unlock(t: *mut core::ffi::c_void, flags: u64);
    fn a20_ex_pdeathsig(t: *const core::ffi::c_void) -> i32;
    fn a20_ex_set_pdeathsig(t: *mut core::ffi::c_void, sig: i32);
    fn a20_ex_tg_group_dead(t: *mut core::ffi::c_void) -> i32;
    fn a20_ex_native_thread_waitable(child: *mut core::ffi::c_void,
                                     reaper: *mut core::ffi::c_void) -> i32;
    fn a20_ex_is_current_any_cpu(t: *const core::ffi::c_void) -> i32;
    fn a20_ex_reap_detach(t: *mut core::ffi::c_void);
    fn a20_ex_note_zombie();
    fn a20_ex_get(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
}

/// `tgid` with the "0 means the pid" fallback the group rules use.
unsafe fn task_tgid(t: *const core::ffi::c_void) -> i32 {
    if t.is_null() {
        return -1;
    }
    let tgid = unsafe { a20_ex_tgid(t) };
    if tgid > 0 { tgid } else { unsafe { a20_ex_pid(t) } }
}

/// The first live, non-idle member of the dead task's thread group, starting
/// at the leader.  A live member keeps orphaned children inside the group
/// instead of handing them to the caller's reaper.
unsafe fn find_live_thread_reaper(dead: *mut core::ffi::c_void) -> *mut core::ffi::c_void {
    let dead_tgid = unsafe { task_tgid(dead) };
    if dead_tgid <= 0 {
        return core::ptr::null_mut();
    }
    let leader = unsafe { a20_ex_tg_leader(dead) };
    let leader = if leader.is_null() { dead } else { leader };
    let idle = unsafe { a20_ex_idle_task() };
    let mut t = leader;
    while !t.is_null() {
        if t != dead && t != idle {
            let st = unsafe { a20_ex_state(t) };
            if st != PROC_UNUSED && st != PROC_ZOMBIE && unsafe { task_tgid(t) } == dead_tgid {
                return t;
            }
        }
        t = unsafe { a20_ex_tg_next(t) };
    }
    core::ptr::null_mut()
}

/// The fallback reaper: the caller's candidate when it is alive, otherwise the
/// idle task (which reaps what nobody else will).
unsafe fn pick_fallback_reaper(reaper: *mut core::ffi::c_void,
                               dead: *mut core::ffi::c_void) -> *mut core::ffi::c_void {
    if reaper.is_null() || reaper == dead {
        return unsafe { a20_ex_idle_task() };
    }
    let st = unsafe { a20_ex_state(reaper) };
    if st == PROC_UNUSED || st == PROC_ZOMBIE {
        return unsafe { a20_ex_idle_task() };
    }
    reaper
}

/// Whether this zombie child must be detached and destroyed rather than
/// reparented: it belongs to a dead thread group, nobody can wait for it, it is
/// a zombie and it is not running on any CPU.
unsafe fn should_reap_group_dead(child: *mut core::ffi::c_void,
                                 thread_reaper: *mut core::ffi::c_void,
                                 actual_reaper: *mut core::ffi::c_void) -> bool {
    let idle = unsafe { a20_ex_idle_task() };
    let group_dead = if thread_reaper.is_null() {
        let dead = unsafe { a20_ex_tg_group_dead(child) };
        dead != 0
    } else {
        false
    };
    if !group_dead {
        return false;
    }
    let exit_signal = unsafe { a20_ex_exit_signal(child) };
    let threaded = unsafe { a20_ex_clone_flags(child) } & CLONE_THREAD != 0;
    let waitable = if threaded && !actual_reaper.is_null() {
        let waitable = unsafe { a20_ex_native_thread_waitable(child, actual_reaper) };
        waitable != 0
    } else {
        false
    };
    if !(actual_reaper == idle || exit_signal != SIGCHLD || (threaded && !waitable)) {
        return false;
    }
    if unsafe { a20_ex_state(child) } != PROC_ZOMBIE {
        return false;
    }
    let on_cpu = unsafe { a20_ex_is_current_any_cpu(child) };
    on_cpu == 0
}

/// Reparent every child of `dead` and fill the deferred lists.
///
/// # Safety
/// `dead` must be a live task in its exit path; `lists` and `out` point at
/// writable caller storage.  The function takes `tasklist_lock` itself and
/// leaves the deferred calls to the adapter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_ex_reparent_children(
    dead: *mut core::ffi::c_void,
    reaper: *mut core::ffi::c_void,
    lists: *mut ReparentLists,
    out: *mut ReparentResult,
) {
    let lists = unsafe { &mut *lists };
    let out = unsafe { &mut *out };
    out.wake_for = core::ptr::null_mut();
    out.destroy_count = 0;
    out.pdeathsig_count = 0;
    out.force_kill_count = 0;
    for i in 0..MAX_LIST {
        lists.to_destroy[i] = core::ptr::null_mut();
    }
    if dead.is_null() {
        return;
    }

    // Parent termination never implicitly kills surviving descendants.
    // D-Bus deliberately SIGKILLs its spawn helper after activation; the
    // service must survive that helper. Explicit PR_SET_PDEATHSIG is handled
    // below, independently of the parent's exit status.
    let flags = unsafe { a20_tl_tasklist_lock() };
    let mut thread_reaper = unsafe { find_live_thread_reaper(dead) };
    let mut actual_reaper = if thread_reaper.is_null() {
        unsafe { pick_fallback_reaper(reaper, dead) }
    } else {
        thread_reaper
    };

    let mut child = unsafe { a20_ex_children(dead) };
    while !child.is_null() {
        let next = unsafe { a20_ex_sibling_next(child) };
        if child == unsafe { a20_ex_idle_task() } {
            child = next;
            continue;
        }
        let mut cstate = unsafe { a20_ex_state(child) };
        if cstate == PROC_UNUSED {
            child = next;
            continue;
        }

        // pdeathsig is claimed exactly once, and only for a child that is not
        // already a zombie: the field is cleared under the child's park_lock.
        let pf = unsafe { a20_ex_park_lock(child) };
        let pdeathsig = if cstate != PROC_ZOMBIE { unsafe { a20_ex_pdeathsig(child) } } else { 0 };
        if pdeathsig != 0 && out.pdeathsig_count < MAX_LIST {
            let n = out.pdeathsig_count;
            lists.pdeathsig_pids[n] = unsafe { a20_ex_pid(child) };
            lists.pdeathsig_signals[n] = pdeathsig;
            unsafe { a20_ex_set_pdeathsig(child, 0) };
            out.pdeathsig_count += 1;
        }
        unsafe { a20_ex_park_unlock(child, pf) };

        if unsafe { should_reap_group_dead(child, thread_reaper, actual_reaper) } {
            if out.destroy_count < MAX_LIST {
                let owned = unsafe { a20_ex_get(child) };
                if !owned.is_null() {
                    unsafe { a20_ex_reap_detach(child) };
                    lists.to_destroy[out.destroy_count] = owned;
                    out.destroy_count += 1;
                }
            } else {
                unsafe { a20_ex_note_zombie() };
                unsafe { a20_tasklist_reparent_locked(actual_reaper, child) };
            }
            // The group may have lost its last live member.
            thread_reaper = unsafe { find_live_thread_reaper(dead) };
            actual_reaper = if thread_reaper.is_null() {
                unsafe { pick_fallback_reaper(reaper, dead) }
            } else {
                thread_reaper
            };
            if unsafe { a20_ex_state(child) } == PROC_UNUSED {
                child = next;
                continue;
            }
        }

        cstate = unsafe { a20_ex_state(child) };

        unsafe { a20_tasklist_reparent_locked(actual_reaper, child) };
        if cstate == PROC_ZOMBIE && out.wake_for.is_null() {
            out.wake_for = actual_reaper;
        }
        child = next;
    }
    unsafe { a20_tl_tasklist_unlock(flags) };
}


#[cfg(test)]
mod tests {
    use super::*;
    use std::format;
    use std::string::String;
    use std::sync::Mutex;
    use std::vec::Vec;

    static TEST_LOCK: Mutex<()> = Mutex::new(());

    #[derive(Clone, Copy)]
    struct FakeTask {
        state: i32,
        pid: i32,
        tgid: i32,
        tg_leader: *mut FakeTask,
        tg_next: *mut FakeTask,
        children: *mut FakeTask,
        sibling_next: *mut FakeTask,
        pdeathsig: i32,
        exit_signal: i32,
        clone_flags: u64,
        exit_code: i32,
        group_dead: i32,
        native_waitable: i32,
        current_any_cpu: i32,
    }

    impl FakeTask {
        const fn new() -> Self {
            FakeTask {
                state: 2, pid: 0, tgid: 0,
                tg_leader: core::ptr::null_mut(), tg_next: core::ptr::null_mut(),
                children: core::ptr::null_mut(), sibling_next: core::ptr::null_mut(),
                pdeathsig: 0, exit_signal: 17, clone_flags: 0, exit_code: 0,
                group_dead: 0, native_waitable: 0, current_any_cpu: 0,
            }
        }
    }

    static mut TASKS: [FakeTask; 80] = [FakeTask::new(); 80];
    static mut IDLE: usize = 79;
    static mut GET_OK: bool = true;
    static mut LIST_DEPTH: i32 = 0;
    static mut PARK_DEPTH: i32 = 0;
    static mut LOG: Vec<String> = Vec::new();

    fn t(i: usize) -> *mut FakeTask { unsafe { core::ptr::addr_of_mut!(TASKS[i]) } }
    fn v(p: *mut FakeTask) -> *mut core::ffi::c_void { p as *mut core::ffi::c_void }

    fn reset_state() {
        unsafe {
            for i in 0..80 {
                TASKS[i] = FakeTask::new();
                TASKS[i].pid = 100 + i as i32;
                TASKS[i].tgid = 100 + i as i32;
            }
            IDLE = 79;
            TASKS[79].pid = 0;
            TASKS[79].tgid = 0;
            GET_OK = true;
            LIST_DEPTH = 0;
            PARK_DEPTH = 0;
            LOG.clear();
        }
    }

    fn reset() -> std::sync::MutexGuard<'static, ()> {
        let guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        reset_state();
        guard
    }

    fn note(s: String) { unsafe { LOG.push(s) } }

    #[no_mangle] unsafe extern "C" fn a20_tl_tasklist_lock() -> u64 {
        unsafe { LIST_DEPTH += 1 };
        0
    }
    #[no_mangle] unsafe extern "C" fn a20_tl_tasklist_unlock(_f: u64) {
        unsafe { LIST_DEPTH -= 1 };
    }
    #[no_mangle] unsafe extern "C" fn a20_tasklist_reparent_locked(p: *mut core::ffi::c_void,
                                                                    c: *mut core::ffi::c_void) {
        note(format!("reparent {}<-{}", unsafe { (*(p as *mut FakeTask)).pid },
                     unsafe { (*(c as *mut FakeTask)).pid }));
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_exit_code(x: *const core::ffi::c_void) -> i32 {
        unsafe { (*(x as *const FakeTask)).exit_code }
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_pid(x: *const core::ffi::c_void) -> i32 {
        unsafe { (*(x as *const FakeTask)).pid }
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_tgid(x: *const core::ffi::c_void) -> i32 {
        unsafe { (*(x as *const FakeTask)).tgid }
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_tg_leader(x: *const core::ffi::c_void) -> *mut core::ffi::c_void {
        v(unsafe { (*(x as *const FakeTask)).tg_leader })
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_tg_next(x: *const core::ffi::c_void) -> *mut core::ffi::c_void {
        v(unsafe { (*(x as *const FakeTask)).tg_next })
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_exit_signal(x: *const core::ffi::c_void) -> i32 {
        unsafe { (*(x as *const FakeTask)).exit_signal }
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_clone_flags(x: *const core::ffi::c_void) -> u64 {
        unsafe { (*(x as *const FakeTask)).clone_flags }
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_children(x: *const core::ffi::c_void) -> *mut core::ffi::c_void {
        v(unsafe { (*(x as *const FakeTask)).children })
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_sibling_next(x: *const core::ffi::c_void) -> *mut core::ffi::c_void {
        v(unsafe { (*(x as *const FakeTask)).sibling_next })
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_idle_task() -> *mut core::ffi::c_void {
        v(t(unsafe { IDLE }))
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_state(x: *mut core::ffi::c_void) -> i32 {
        unsafe { (*(x as *mut FakeTask)).state }
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_park_lock(_x: *mut core::ffi::c_void) -> u64 {
        unsafe {
            PARK_DEPTH += 1;
            assert!(LIST_DEPTH > 0, "park_lock nests inside tasklist_lock");
        }
        0
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_park_unlock(_x: *mut core::ffi::c_void, _f: u64) {
        unsafe { PARK_DEPTH -= 1 };
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_pdeathsig(x: *const core::ffi::c_void) -> i32 {
        unsafe { (*(x as *const FakeTask)).pdeathsig }
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_set_pdeathsig(x: *mut core::ffi::c_void, sig: i32) {
        unsafe { (*(x as *mut FakeTask)).pdeathsig = sig };
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_tg_group_dead(x: *mut core::ffi::c_void) -> i32 {
        unsafe { (*(x as *mut FakeTask)).group_dead }
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_native_thread_waitable(_c: *mut core::ffi::c_void,
                                                                     _r: *mut core::ffi::c_void) -> i32 {
        unsafe { (*(_c as *mut FakeTask)).native_waitable }
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_is_current_any_cpu(x: *const core::ffi::c_void) -> i32 {
        unsafe { (*(x as *const FakeTask)).current_any_cpu }
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_reap_detach(x: *mut core::ffi::c_void) {
        note(format!("detach {}", unsafe { (*(x as *mut FakeTask)).pid }));
        unsafe { (*(x as *mut FakeTask)).state = PROC_UNUSED };
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_note_zombie() {
        note("note-zombie".into());
    }
    #[no_mangle] unsafe extern "C" fn a20_ex_get(x: *mut core::ffi::c_void) -> *mut core::ffi::c_void {
        if unsafe { GET_OK } { x } else { core::ptr::null_mut() }
    }

    fn run(dead: *mut FakeTask, reaper: *mut FakeTask)
           -> (ReparentLists, ReparentResult) {
        let mut lists: ReparentLists = unsafe { core::mem::zeroed() };
        let mut out: ReparentResult = unsafe { core::mem::zeroed() };
        unsafe { a20_ex_reparent_children(v(dead), v(reaper), &mut lists, &mut out) };
        (lists, out)
    }

    fn log() -> Vec<String> { unsafe { LOG.clone() } }

    #[test]
    fn a_null_dead_task_produces_no_work() {
        let _g = reset();
        let (_, out) = run(core::ptr::null_mut(), t(0));
        assert!(out.wake_for.is_null());
        assert_eq!(out.destroy_count, 0);
        assert!(log().is_empty());
    }

    #[test]
    fn children_are_reparented_and_a_zombie_selects_the_wake() {
        let _g = reset();
        unsafe {
            TASKS[0].children = t(1);
            TASKS[1].sibling_next = t(2);
            TASKS[1].pid = 101;
            TASKS[2].pid = 102;
            TASKS[2].state = PROC_ZOMBIE;
        }
        let (_, out) = run(t(0), t(5));
        assert_eq!(log(), ["reparent 105<-101", "reparent 105<-102"]);
        assert_eq!(out.wake_for, v(t(5)), "the zombie's reaper needs the wake");
    }

    #[test]
    fn the_idle_and_unused_children_are_skipped() {
        let _g = reset();
        unsafe {
            TASKS[0].children = t(1);
            TASKS[1].sibling_next = t(2);
            TASKS[1].pid = 101;
            TASKS[2].sibling_next = v(unsafe { t(unsafe { IDLE }) }) as *mut FakeTask;
            TASKS[2].pid = 102;
        }
        // chain: 1 -> 2 -> idle
        unsafe {
            TASKS[0].children = t(1);
            TASKS[1].sibling_next = t(2);
            TASKS[2].sibling_next = t(unsafe { IDLE });
        }
        let (_, out) = run(t(0), t(5));
        assert_eq!(log(), ["reparent 105<-101", "reparent 105<-102"]);
        assert_eq!(out.pdeathsig_count, 0);

        reset_state();
        unsafe {
            TASKS[0].children = t(1);
            TASKS[1].pid = 101;
            TASKS[1].state = PROC_UNUSED;
            TASKS[1].pdeathsig = 9;
        }
        let (_, out) = run(t(0), t(5));
        assert!(log().is_empty(), "an UNUSED child is neither claimed nor reparented");
        assert_eq!(out.pdeathsig_count, 0);
    }

    #[test]
    fn pdeathsig_is_claimed_once_and_only_for_non_zombies() {
        let _g = reset();
        unsafe {
            TASKS[0].children = t(1);
            TASKS[1].sibling_next = t(2);
            TASKS[1].pid = 101;
            TASKS[1].pdeathsig = 15;
            TASKS[2].pid = 102;
            TASKS[2].state = PROC_ZOMBIE;
            TASKS[2].pdeathsig = 9;
        }
        let (lists, out) = run(t(0), t(5));
        assert_eq!(out.pdeathsig_count, 1);
        assert_eq!(lists.pdeathsig_pids[0], 101);
        assert_eq!(lists.pdeathsig_signals[0], 15);
        assert_eq!(unsafe { TASKS[1].pdeathsig }, 0, "the claim clears the field");
        assert_eq!(unsafe { TASKS[2].pdeathsig }, 9, "a zombie's field is left alone");
    }

    #[test]
    fn a_live_group_member_is_the_reaper() {
        let _g = reset();
        unsafe {
            // Dead (0) leads group 100; member 1 is alive.
            TASKS[0].tgid = 100;
            TASKS[0].tg_leader = t(0);
            TASKS[0].tg_next = t(1);
            TASKS[1].tgid = 100;
            TASKS[0].children = t(2);
            TASKS[2].pid = 102;
        }
        let (_, out) = run(t(0), t(5));
        assert_eq!(log(), ["reparent 101<-102"], "the live member takes the child");
        assert_eq!(out.wake_for, core::ptr::null_mut());
    }

    #[test]
    fn a_group_dead_zombie_is_detached_and_destroyed() {
        let _g = reset();
        unsafe {
            TASKS[0].tgid = 100;
            TASKS[0].tg_leader = t(0);
            TASKS[0].children = t(2);
            TASKS[2].pid = 102;
            TASKS[2].state = PROC_ZOMBIE;
            TASKS[2].exit_signal = 9; // not SIGCHLD: never waitable by the reaper
            TASKS[2].group_dead = 1;
        }
        let (lists, out) = run(t(0), t(5));
        assert_eq!(out.destroy_count, 1);
        assert_eq!(lists.to_destroy[0], v(t(2)));
        assert_eq!(log(), ["detach 102"]);
        assert!(out.wake_for.is_null(), "a detached child is not reparented");
    }

    #[test]
    fn the_group_dead_reap_respects_the_sixteen_byte_conditions() {
        let _g = reset();
        unsafe {
            TASKS[0].children = t(1);
            TASKS[1].pid = 101;
            TASKS[1].state = PROC_ZOMBIE;
            TASKS[1].exit_signal = SIGCHLD;
            TASKS[1].group_dead = 1;
        }
        // A SIGCHLD zombie in a dead group is still reparented (waitable).
        let (_, out) = run(t(0), t(5));
        assert_eq!(out.destroy_count, 0);
        assert_eq!(log(), ["reparent 105<-101"]);

        reset_state();
        unsafe {
            TASKS[0].children = t(1);
            TASKS[1].pid = 101;
            TASKS[1].state = PROC_ZOMBIE;
            TASKS[1].exit_signal = 9;
            TASKS[1].group_dead = 1;
            TASKS[1].current_any_cpu = 1;
        }
        let (_, out) = run(t(0), t(5));
        assert_eq!(out.destroy_count, 0, "a running child is not reaped");
        assert_eq!(log(), ["reparent 105<-101"]);
    }

    #[test]
    fn a_threaded_zombie_is_reaped_when_the_native_reaper_cannot_wait() {
        let _g = reset();
        unsafe {
            TASKS[0].children = t(1);
            TASKS[1].pid = 101;
            TASKS[1].state = PROC_ZOMBIE;
            TASKS[1].exit_signal = SIGCHLD;
            TASKS[1].clone_flags = CLONE_THREAD;
            TASKS[1].group_dead = 1;
            TASKS[1].native_waitable = 0;
        }
        let (_, out) = run(t(0), t(5));
        assert_eq!(out.destroy_count, 1);
        assert_eq!(log(), ["detach 101"]);
    }

    #[test]
    fn parent_exit_reparents_without_implicitly_killing_live_children() {
        let _g = reset();
        for code in [-9, -15, -6, -SIGCHLD, -SIGSTOP, 0, 7] {
            reset_state();
            unsafe {
                TASKS[0].children = t(1);
                TASKS[1].sibling_next = t(2);
                TASKS[1].pid = 101;
                TASKS[2].pid = 102;
                TASKS[2].state = PROC_ZOMBIE;
                TASKS[0].exit_code = code;
            }
            let (_, out) = run(t(0), t(5));
            assert_eq!(out.force_kill_count, 0, "parent exit code {code}");
            assert_eq!(out.pdeathsig_count, 0);
            assert!(log().iter().any(|entry| entry == "reparent 105<-101"));
            unsafe { assert_ne!(TASKS[1].state, PROC_ZOMBIE); }
        }
    }

    #[test]
    fn the_destroy_list_overflow_falls_back_to_reparenting() {
        let _g = reset();
        unsafe {
            // 65 group-dead zombies: the 65th overflows the 64-entry array.
            for i in 1..=65 {
                TASKS[i].pid = 100 + i as i32;
                TASKS[i].state = PROC_ZOMBIE;
                TASKS[i].exit_signal = 9;
                TASKS[i].group_dead = 1;
                TASKS[i].sibling_next = if i < 65 { t(i + 1) } else { core::ptr::null_mut() };
            }
            TASKS[0].children = t(1);
        }
        // The candidate reaper must be outside the child set: every child
        // here is a zombie, and a zombie candidate falls back to idle.
        let (_, out) = run(t(0), t(78));
        assert_eq!(out.destroy_count, 64);
        let entries = log();
        assert!(entries.iter().any(|e| e == "note-zombie"));
        assert!(entries.iter().any(|e| e == "reparent 178<-165"), "{entries:?}");
    }

    #[test]
    fn the_fallback_reaper_falls_back_to_idle() {
        let _g = reset();
        unsafe {
            TASKS[0].children = t(1);
            TASKS[1].pid = 101;
            // A dead candidate reaper is replaced by idle.
            TASKS[6].state = PROC_ZOMBIE;
        }
        let (_, _) = run(t(0), t(6));
        assert_eq!(log(), ["reparent 0<-101"], "idle has pid 0");
    }
}
