#![no_std]

//! Global task-list, children and thread-group membership.
//!
//! Rust owns the membership transitions of a task: joining and leaving the
//! global all-task list, the parent/children chain, and the thread-group
//! chain, plus the thread-group leader pin.  C keeps what the transitions are
//! made of -- the intrusive link fields, the two list ends, the locks that
//! serialize them, the reference count on a task and the lifecycle phase
//! advance that must commit atomically with list visibility.
//!
//! Every entry point here runs with `tasklist_lock` already held by the
//! adapter (except `a20_tasklist_link_newborn`, which takes it itself, as the
//! C version did), because the membership rules and the list walk share that
//! one lock.  The link fields are reached through accessors rather than a
//! view: the transitions read and write *other* tasks' links, so a borrowed
//! view of one task would not be enough and the accessors keep the pointer
//! arithmetic in one place.

#[cfg(test)]
extern crate std;

const EINVAL: i32 = -22;
const ESRCH: i32 = -3;

/// The phase event whose commit must be atomic with list visibility; the
/// value matches PROC_TASK_EVENT_PUBLISH in proc/task_lifecycle.h.
const PROC_TASK_EVENT_PUBLISH: u32 = 2;

extern "C" {
    /* Link fields, one accessor per field. */
    fn a20_tl_all_prev(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_tl_set_all_prev(t: *mut core::ffi::c_void, v: *mut core::ffi::c_void);
    fn a20_tl_all_next(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_tl_set_all_next(t: *mut core::ffi::c_void, v: *mut core::ffi::c_void);
    fn a20_tl_sibling_next(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_tl_set_sibling_next(t: *mut core::ffi::c_void, v: *mut core::ffi::c_void);
    fn a20_tl_sibling_prev_ptr(t: *mut core::ffi::c_void) -> *mut *mut core::ffi::c_void;
    fn a20_tl_set_sibling_prev_ptr(t: *mut core::ffi::c_void, v: *mut *mut core::ffi::c_void);
    fn a20_tl_tg_next(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_tl_set_tg_next(t: *mut core::ffi::c_void, v: *mut core::ffi::c_void);
    fn a20_tl_tg_prev_ptr(t: *mut core::ffi::c_void) -> *mut *mut core::ffi::c_void;
    fn a20_tl_set_tg_prev_ptr(t: *mut core::ffi::c_void, v: *mut *mut core::ffi::c_void);
    fn a20_tl_parent(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_tl_set_parent(t: *mut core::ffi::c_void, v: *mut core::ffi::c_void);
    fn a20_tl_set_ppid(t: *mut core::ffi::c_void, v: i32);
    fn a20_tl_pid(t: *mut core::ffi::c_void) -> i32;
    fn a20_tl_children(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_tl_set_children(t: *mut core::ffi::c_void, v: *mut core::ffi::c_void);
    fn a20_tl_tg_leader(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_tl_set_tg_leader(t: *mut core::ffi::c_void, v: *mut core::ffi::c_void);
    fn a20_tl_tg_leader_ref_held(t: *mut core::ffi::c_void) -> i32;
    fn a20_tl_set_tg_leader_ref_held(t: *mut core::ffi::c_void, v: i32);

    /* The two ends of the global list. */
    fn a20_tl_list_head() -> *mut core::ffi::c_void;
    fn a20_tl_set_list_head(v: *mut core::ffi::c_void);
    fn a20_tl_list_tail() -> *mut core::ffi::c_void;
    fn a20_tl_set_list_tail(v: *mut core::ffi::c_void);

    /* Locks, references, the phase advance and the diagnostics. */
    fn a20_tl_tasklist_lock() -> u64;
    fn a20_tl_tasklist_unlock(flags: u64);
    fn a20_tl_park_lock(t: *mut core::ffi::c_void) -> u64;
    fn a20_tl_park_unlock(t: *mut core::ffi::c_void, flags: u64);
    fn a20_tl_task_get(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void;
    fn a20_tl_task_put(t: *mut core::ffi::c_void);
    fn a20_tl_lifecycle_advance(t: *mut core::ffi::c_void, event: u32) -> i32;
    fn a20_tl_is_kernel_ptr(value: usize) -> i32;
    fn a20_tl_report_corrupt(t: *mut core::ffi::c_void, next: *mut core::ffi::c_void);
}

/// Append a boot-time task (the per-CPU idle tasks) to the global list.  The
/// boot idle tasks are not published through the lifecycle, so there is no
/// phase commit to take with the link.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_tasklist_link_boot_locked(t: *mut core::ffi::c_void) {
    if t.is_null() {
        return;
    }
    unsafe { link_task(t) };
}

/// Append `t` to the global all-task list.
unsafe fn link_task(t: *mut core::ffi::c_void) {
    let tail = unsafe { a20_tl_list_tail() };
    unsafe {
        a20_tl_set_all_prev(t, tail);
        a20_tl_set_all_next(t, core::ptr::null_mut());
        if !tail.is_null() {
            a20_tl_set_all_next(tail, t);
        } else {
            a20_tl_set_list_head(t);
        }
        a20_tl_set_list_tail(t);
    }
}

/// Publish a fully initialized task on the global list.  The caller holds
/// `tasklist_lock`; the phase commit and the list link are taken together
/// under the task's own park_lock, following the established list->park
/// nesting.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_tasklist_publish_locked(t: *mut core::ffi::c_void) -> i32 {
    if t.is_null() {
        return EINVAL;
    }
    let flags = unsafe { a20_tl_park_lock(t) };
    let rc = unsafe { a20_tl_lifecycle_advance(t, PROC_TASK_EVENT_PUBLISH) };
    if rc == 0 {
        unsafe { link_task(t) };
    }
    unsafe { a20_tl_park_unlock(t, flags) };
    rc
}

/// Link a freshly created (non-fork) task into the wait4 children-list model.
/// Takes and releases `tasklist_lock` itself, and defaults the thread-group
/// leader to the task when it has none.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_tasklist_link_newborn(t: *mut core::ffi::c_void) -> i32 {
    if t.is_null() {
        return EINVAL;
    }
    let flags = unsafe { a20_tl_tasklist_lock() };
    if unsafe { a20_tl_tg_leader(t) }.is_null() {
        unsafe { a20_tl_set_tg_leader(t, t) };
    }
    let rc = unsafe { a20_tasklist_publish_locked(t) };
    if rc == 0 {
        let parent = unsafe { a20_tl_parent(t) };
        unsafe { a20_tasklist_children_link_locked(parent, t) };
    }
    unsafe { a20_tl_tasklist_unlock(flags) };
    rc
}

/// Insert `child` at the head of `parent`'s children chain.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_tasklist_children_link_locked(parent: *mut core::ffi::c_void,
                                                           child: *mut core::ffi::c_void) {
    if parent.is_null() || child.is_null() {
        return;
    }
    if !unsafe { a20_tl_sibling_prev_ptr(child) }.is_null() {
        return;
    }
    let next = unsafe { a20_tl_children(parent) };
    unsafe {
        a20_tl_set_sibling_next(child, next);
        a20_tl_set_sibling_prev_ptr(child, children_slot(parent));
        if !next.is_null() {
            a20_tl_set_sibling_prev_ptr(next, sibling_slot(child));
        }
        a20_tl_set_children(parent, child);
    }
}

/// Detach `t` from whatever children chain it is on.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_tasklist_children_unlink_locked(t: *mut core::ffi::c_void) {
    if t.is_null() {
        return;
    }
    let slot = unsafe { a20_tl_sibling_prev_ptr(t) };
    if slot.is_null() {
        return;
    }
    let next = unsafe { a20_tl_sibling_next(t) };
    unsafe {
        *slot = next;
        if !next.is_null() {
            a20_tl_set_sibling_prev_ptr(next, slot);
        }
        a20_tl_set_sibling_next(t, core::ptr::null_mut());
        a20_tl_set_sibling_prev_ptr(t, core::ptr::null_mut());
    }
}

/// Move `t` under `new_parent`, keeping the chain and the reported ppid in
/// step.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_tasklist_reparent_locked(new_parent: *mut core::ffi::c_void,
                                                     t: *mut core::ffi::c_void) {
    if t.is_null() || new_parent.is_null() {
        return;
    }
    unsafe {
        a20_tasklist_children_unlink_locked(t);
        a20_tl_set_parent(t, new_parent);
        a20_tl_set_ppid(t, a20_tl_pid(new_parent));
        a20_tasklist_children_link_locked(new_parent, t);
    }
}

/// Join `t` to its thread-group chain, taking the leader pin the join needs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_tasklist_tg_link_locked(t: *mut core::ffi::c_void) -> i32 {
    let leader = unsafe { a20_tl_tg_leader(t) };
    let leader = if leader.is_null() { t } else { leader };
    if t == leader || leader == unsafe { a20_tl_tg_next(t) } {
        return 0;
    }
    if !unsafe { a20_tl_tg_prev_ptr(t) }.is_null() {
        return 0;
    }
    if unsafe { a20_tl_tg_leader_ref_held(t) } != 0 || unsafe { a20_tl_task_get(leader) }.is_null() {
        return ESRCH;
    }
    unsafe {
        a20_tl_set_tg_leader_ref_held(t, 1);
        let next = a20_tl_tg_next(leader);
        a20_tl_set_tg_next(t, next);
        a20_tl_set_tg_prev_ptr(t, tg_next_slot(leader));
        if !next.is_null() {
            a20_tl_set_tg_prev_ptr(next, tg_next_slot(t));
        }
        a20_tl_set_tg_next(leader, t);
    }
    0
}

/// Detach `t` from its thread-group chain.  The leader pin is intentionally
/// kept until final task release: other references may still inspect
/// `tg_leader` after list detachment.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_tasklist_tg_unlink_locked(t: *mut core::ffi::c_void) {
    if t.is_null() {
        return;
    }
    let slot = unsafe { a20_tl_tg_prev_ptr(t) };
    if slot.is_null() {
        return;
    }
    let next = unsafe { a20_tl_tg_next(t) };
    unsafe {
        *slot = next;
        if !next.is_null() {
            a20_tl_set_tg_prev_ptr(next, slot);
        }
        a20_tl_set_tg_next(t, core::ptr::null_mut());
        a20_tl_set_tg_prev_ptr(t, core::ptr::null_mut());
    }
}

/// Drop the thread-group leader pin, if this task holds one.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_tasklist_tg_leader_ref_release(t: *mut core::ffi::c_void) {
    if t.is_null() || unsafe { a20_tl_tg_leader_ref_held(t) } == 0 {
        return;
    }
    let leader = unsafe { a20_tl_tg_leader(t) };
    unsafe {
        a20_tl_set_tg_leader_ref_held(t, 0);
        a20_tl_set_tg_leader(t, core::ptr::null_mut());
        a20_tl_task_put(leader);
    }
}

/// Remove `t` from the children chain, the thread-group chain and the global
/// list.  The caller holds `tasklist_lock`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_tasklist_unlink_locked(t: *mut core::ffi::c_void) {
    if t.is_null() {
        return;
    }
    unsafe { a20_tasklist_children_unlink_locked(t) };
    unsafe { a20_tasklist_tg_unlink_locked(t) };
    let head = unsafe { a20_tl_list_head() };
    let tail = unsafe { a20_tl_list_tail() };
    let prev = unsafe { a20_tl_all_prev(t) };
    let next = unsafe { a20_tl_all_next(t) };
    unsafe {
        if !prev.is_null() {
            a20_tl_set_all_next(prev, next);
        } else if head == t {
            a20_tl_set_list_head(next);
        }
        if !next.is_null() {
            a20_tl_set_all_prev(next, prev);
        } else if tail == t {
            a20_tl_set_list_tail(prev);
        }
        a20_tl_set_all_next(t, core::ptr::null_mut());
        a20_tl_set_all_prev(t, core::ptr::null_mut());
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_tasklist_first_locked() -> *mut core::ffi::c_void {
    unsafe { a20_tl_list_head() }
}

/// Walk to the next task, refusing a link that is not a kernel pointer or
/// that is misaligned, which is how a corrupted chain is kept from being
/// followed into user memory.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_tasklist_next_locked(t: *mut core::ffi::c_void) -> *mut core::ffi::c_void {
    if t.is_null() {
        return core::ptr::null_mut();
    }
    let next = unsafe { a20_tl_all_next(t) };
    if next.is_null() {
        return core::ptr::null_mut();
    }
    let misaligned = (next as usize) & (core::mem::size_of::<*const core::ffi::c_void>() - 1);
    if misaligned != 0 || unsafe { a20_tl_is_kernel_ptr(next as usize) } == 0 {
        unsafe { a20_tl_report_corrupt(t, next) };
        return core::ptr::null_mut();
    }
    next
}

/* The address of the parent's children head, and of two tasks' next-links.
 * These are the intrusive-list slot addresses the chains are rewired through. */
unsafe fn children_slot(parent: *mut core::ffi::c_void) -> *mut *mut core::ffi::c_void {
    unsafe { a20_tl_children_slot(parent) }
}

unsafe fn sibling_slot(child: *mut core::ffi::c_void) -> *mut *mut core::ffi::c_void {
    unsafe { a20_tl_sibling_next_slot(child) }
}

unsafe fn tg_next_slot(t: *mut core::ffi::c_void) -> *mut *mut core::ffi::c_void {
    unsafe { a20_tl_tg_next_slot(t) }
}

extern "C" {
    fn a20_tl_children_slot(t: *mut core::ffi::c_void) -> *mut *mut core::ffi::c_void;
    fn a20_tl_sibling_next_slot(t: *mut core::ffi::c_void) -> *mut *mut core::ffi::c_void;
    fn a20_tl_tg_next_slot(t: *mut core::ffi::c_void) -> *mut *mut core::ffi::c_void;
}


#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    static TEST_LOCK: Mutex<()> = Mutex::new(());

    /// A task with just the fields the transitions touch.
    #[derive(Clone, Copy)]
    struct FakeTask {
        pid: i32,
        ppid: i32,
        parent: *mut FakeTask,
        children: *mut FakeTask,
        sibling_next: *mut FakeTask,
        sibling_prev_ptr: *mut *mut FakeTask,
        all_prev: *mut FakeTask,
        all_next: *mut FakeTask,
        tg_leader: *mut FakeTask,
        tg_next: *mut FakeTask,
        tg_prev_ptr: *mut *mut FakeTask,
        tg_leader_ref_held: i32,
    }

    impl FakeTask {
        const fn new() -> Self {
            FakeTask {
                pid: 0,
                ppid: 0,
                parent: core::ptr::null_mut(),
                children: core::ptr::null_mut(),
                sibling_next: core::ptr::null_mut(),
                sibling_prev_ptr: core::ptr::null_mut(),
                all_prev: core::ptr::null_mut(),
                all_next: core::ptr::null_mut(),
                tg_leader: core::ptr::null_mut(),
                tg_next: core::ptr::null_mut(),
                tg_prev_ptr: core::ptr::null_mut(),
                tg_leader_ref_held: 0,
            }
        }
    }

    static mut TASKS: [FakeTask; 6] = [FakeTask::new(); 6];
    static mut HEAD: *mut FakeTask = core::ptr::null_mut();
    static mut TAIL: *mut FakeTask = core::ptr::null_mut();
    static mut ADVANCE_RC: i32 = 0;
    static mut ADVANCE_EVENTS: [u32; 4] = [0; 4];
    static mut ADVANCE_COUNT: usize = 0;
    static mut PARK_DEPTH: i32 = 0;
    static mut LIST_HOLD: i32 = 0;
    static mut PUTS: [i32; 4] = [0; 4];
    static mut PUT_COUNT: usize = 0;
    static mut GET_OK: bool = true;
    static mut CORRUPT_REPORTS: usize = 0;
    static mut KERNEL_PTRS: bool = true;

    fn t(index: usize) -> *mut FakeTask {
        unsafe { core::ptr::addr_of_mut!(TASKS[index]) }
    }

    fn v(p: *mut FakeTask) -> *mut core::ffi::c_void {
        p as *mut core::ffi::c_void
    }

    fn reset() -> std::sync::MutexGuard<'static, ()> {
        let guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        unsafe {
            core::ptr::write_bytes(core::ptr::addr_of_mut!(TASKS), 0, 1);
            for i in 0..6 {
                TASKS[i] = FakeTask::new();
                TASKS[i].pid = 100 + i as i32;
            }
            HEAD = core::ptr::null_mut();
            TAIL = core::ptr::null_mut();
            ADVANCE_RC = 0;
            ADVANCE_EVENTS = [0; 4];
            ADVANCE_COUNT = 0;
            PARK_DEPTH = 0;
            LIST_HOLD = 0;
            PUTS = [0; 4];
            PUT_COUNT = 0;
            GET_OK = true;
            CORRUPT_REPORTS = 0;
            KERNEL_PTRS = true;
        }
        guard
    }

    /// Publish under tasklist_lock, the contract the C caller honours.
    unsafe fn publish(p: *mut core::ffi::c_void) -> i32 {
        let flags = unsafe { a20_tl_tasklist_lock() };
        let rc = unsafe { a20_tasklist_publish_locked(p) };
        unsafe { a20_tl_tasklist_unlock(flags) };
        rc
    }

    fn list_len() -> usize {
        let mut n = 0;
        let mut cur = unsafe { HEAD };
        while !cur.is_null() {
            n += 1;
            cur = unsafe { (*cur).all_next };
        }
        n
    }

    // ---- accessors -------------------------------------------------------

    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_all_prev(p: *const core::ffi::c_void) -> *mut core::ffi::c_void {
        v(unsafe { (*(p as *const FakeTask)).all_prev })
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_set_all_prev(p: *mut core::ffi::c_void, x: *mut core::ffi::c_void) {
        unsafe { (*(p as *mut FakeTask)).all_prev = x as *mut FakeTask };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_all_next(p: *const core::ffi::c_void) -> *mut core::ffi::c_void {
        v(unsafe { (*(p as *const FakeTask)).all_next })
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_set_all_next(p: *mut core::ffi::c_void, x: *mut core::ffi::c_void) {
        unsafe { (*(p as *mut FakeTask)).all_next = x as *mut FakeTask };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_sibling_next(p: *const core::ffi::c_void) -> *mut core::ffi::c_void {
        v(unsafe { (*(p as *const FakeTask)).sibling_next })
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_set_sibling_next(p: *mut core::ffi::c_void, x: *mut core::ffi::c_void) {
        unsafe { (*(p as *mut FakeTask)).sibling_next = x as *mut FakeTask };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_sibling_prev_ptr(p: *const core::ffi::c_void) -> *mut *mut core::ffi::c_void {
        unsafe { (*(p as *const FakeTask)).sibling_prev_ptr as *mut *mut core::ffi::c_void }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_set_sibling_prev_ptr(p: *mut core::ffi::c_void, x: *mut *mut core::ffi::c_void) {
        unsafe { (*(p as *mut FakeTask)).sibling_prev_ptr = x as *mut *mut FakeTask };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_children_slot(p: *mut core::ffi::c_void) -> *mut *mut core::ffi::c_void {
        unsafe { core::ptr::addr_of_mut!((*(p as *mut FakeTask)).children) as *mut *mut core::ffi::c_void }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_sibling_next_slot(p: *mut core::ffi::c_void) -> *mut *mut core::ffi::c_void {
        unsafe { core::ptr::addr_of_mut!((*(p as *mut FakeTask)).sibling_next) as *mut *mut core::ffi::c_void }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_tg_next_slot(p: *mut core::ffi::c_void) -> *mut *mut core::ffi::c_void {
        unsafe { core::ptr::addr_of_mut!((*(p as *mut FakeTask)).tg_next) as *mut *mut core::ffi::c_void }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_tg_next(p: *const core::ffi::c_void) -> *mut core::ffi::c_void {
        v(unsafe { (*(p as *const FakeTask)).tg_next })
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_set_tg_next(p: *mut core::ffi::c_void, x: *mut core::ffi::c_void) {
        unsafe { (*(p as *mut FakeTask)).tg_next = x as *mut FakeTask };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_tg_prev_ptr(p: *const core::ffi::c_void) -> *mut *mut core::ffi::c_void {
        unsafe { (*(p as *const FakeTask)).tg_prev_ptr as *mut *mut core::ffi::c_void }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_set_tg_prev_ptr(p: *mut core::ffi::c_void, x: *mut *mut core::ffi::c_void) {
        unsafe { (*(p as *mut FakeTask)).tg_prev_ptr = x as *mut *mut FakeTask };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_parent(p: *const core::ffi::c_void) -> *mut core::ffi::c_void {
        v(unsafe { (*(p as *const FakeTask)).parent })
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_set_parent(p: *mut core::ffi::c_void, x: *mut core::ffi::c_void) {
        unsafe { (*(p as *mut FakeTask)).parent = x as *mut FakeTask };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_set_ppid(p: *mut core::ffi::c_void, x: i32) {
        unsafe { (*(p as *mut FakeTask)).ppid = x };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_pid(p: *const core::ffi::c_void) -> i32 {
        unsafe { (*(p as *const FakeTask)).pid }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_children(p: *const core::ffi::c_void) -> *mut core::ffi::c_void {
        v(unsafe { (*(p as *const FakeTask)).children })
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_set_children(p: *mut core::ffi::c_void, x: *mut core::ffi::c_void) {
        unsafe { (*(p as *mut FakeTask)).children = x as *mut FakeTask };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_tg_leader(p: *const core::ffi::c_void) -> *mut core::ffi::c_void {
        v(unsafe { (*(p as *const FakeTask)).tg_leader })
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_set_tg_leader(p: *mut core::ffi::c_void, x: *mut core::ffi::c_void) {
        unsafe { (*(p as *mut FakeTask)).tg_leader = x as *mut FakeTask };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_tg_leader_ref_held(p: *const core::ffi::c_void) -> i32 {
        unsafe { (*(p as *const FakeTask)).tg_leader_ref_held }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_set_tg_leader_ref_held(p: *mut core::ffi::c_void, x: i32) {
        unsafe { (*(p as *mut FakeTask)).tg_leader_ref_held = x };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_list_head() -> *mut core::ffi::c_void {
        v(unsafe { HEAD })
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_set_list_head(x: *mut core::ffi::c_void) {
        unsafe { HEAD = x as *mut FakeTask };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_list_tail() -> *mut core::ffi::c_void {
        v(unsafe { TAIL })
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_set_list_tail(x: *mut core::ffi::c_void) {
        unsafe { TAIL = x as *mut FakeTask };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_tasklist_lock() -> u64 {
        unsafe { LIST_HOLD += 1 };
        0
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_tasklist_unlock(_flags: u64) {
        unsafe { LIST_HOLD -= 1 };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_park_lock(_t: *mut core::ffi::c_void) -> u64 {
        unsafe {
            PARK_DEPTH += 1;
            assert!(LIST_HOLD > 0, "park_lock nests inside tasklist_lock");
        }
        0
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_park_unlock(_t: *mut core::ffi::c_void, _flags: u64) {
        unsafe { PARK_DEPTH -= 1 };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_task_get(x: *mut core::ffi::c_void) -> *mut core::ffi::c_void {
        if unsafe { GET_OK } { x } else { core::ptr::null_mut() }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_task_put(x: *mut core::ffi::c_void) {
        unsafe {
            if PUT_COUNT < PUTS.len() {
                PUTS[PUT_COUNT] = (*(x as *const FakeTask)).pid;
                PUT_COUNT += 1;
            }
        }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_lifecycle_advance(_t: *mut core::ffi::c_void, event: u32) -> i32 {
        unsafe {
            if ADVANCE_COUNT < ADVANCE_EVENTS.len() {
                ADVANCE_EVENTS[ADVANCE_COUNT] = event;
            }
            ADVANCE_COUNT += 1;
            assert_eq!(PARK_DEPTH, 1, "the phase commit holds park_lock");
            ADVANCE_RC
        }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_is_kernel_ptr(_value: usize) -> i32 {
        if unsafe { KERNEL_PTRS } { 1 } else { 0 }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_tl_report_corrupt(_t: *const core::ffi::c_void,
                                               _next: *const core::ffi::c_void) {
        unsafe { CORRUPT_REPORTS += 1 };
    }

    // ---- tests -----------------------------------------------------------

    #[test]
    fn link_task_appends_and_keeps_both_ends() {
        let _guard = reset();
        unsafe {
            assert_eq!(publish(v(t(0))), 0);
            assert_eq!(publish(v(t(1))), 0);
            assert_eq!(publish(v(t(2))), 0);
        }
        assert_eq!(list_len(), 3);
        unsafe {
            assert_eq!(*(core::ptr::addr_of!(HEAD)), t(0));
            assert_eq!(*(core::ptr::addr_of!(TAIL)), t(2));
            assert_eq!((*t(0)).all_next, t(1));
            assert_eq!((*t(1)).all_prev, t(0));
            assert_eq!((*t(2)).all_next, core::ptr::null_mut());
        }
        assert_eq!(unsafe { ADVANCE_EVENTS[0] }, PROC_TASK_EVENT_PUBLISH);
    }

    #[test]
    fn publish_rejects_a_null_task() {
        let _guard = reset();
        assert_eq!(unsafe { publish(core::ptr::null_mut()) }, EINVAL);
        assert_eq!(list_len(), 0);
    }

    #[test]
    fn publish_links_only_after_the_phase_commit() {
        let _guard = reset();
        unsafe { ADVANCE_RC = -EINVAL };
        assert_eq!(unsafe { publish(v(t(0))) }, -EINVAL);
        assert_eq!(list_len(), 0, "a refused phase leaves the task unpublished");
    }

    #[test]
    fn link_newborn_defaults_the_leader_and_joins_the_parent_chain() {
        let _guard = reset();
        unsafe {
            (*t(0)).pid = 100;
            (*t(1)).parent = t(0);
        }
        assert_eq!(unsafe { a20_tasklist_link_newborn(v(t(1))) }, 0);
        assert_eq!(list_len(), 1);
        unsafe {
            assert_eq!((*t(1)).tg_leader, t(1), "a newborn leads its own group");
            assert_eq!((*t(0)).children, t(1));
            assert_eq!((*t(1)).sibling_prev_ptr, core::ptr::addr_of_mut!((*t(0)).children));
        }
        assert_eq!(unsafe { LIST_HOLD }, 0, "link_newborn takes its own lock");
    }

    #[test]
    fn children_link_ignores_a_child_already_on_a_chain() {
        let _guard = reset();
        unsafe {
            (*t(1)).parent = t(0);
            a20_tasklist_children_link_locked(v(t(0)), v(t(1)));
            a20_tasklist_children_link_locked(v(t(0)), v(t(1)));
            assert_eq!((*t(0)).children, t(1));
            assert_eq!((*t(1)).sibling_next, core::ptr::null_mut());
            a20_tasklist_children_link_locked(core::ptr::null_mut(), v(t(1)));
            a20_tasklist_children_link_locked(v(t(0)), core::ptr::null_mut());
        }
    }

    #[test]
    fn children_chain_keeps_the_previous_slot_addresses_correct() {
        let _guard = reset();
        unsafe {
            a20_tasklist_children_link_locked(v(t(0)), v(t(1)));
            a20_tasklist_children_link_locked(v(t(0)), v(t(2)));
            assert_eq!((*t(0)).children, t(2));
            assert_eq!((*t(2)).sibling_next, t(1));
            assert_eq!((*t(1)).sibling_prev_ptr, core::ptr::addr_of_mut!((*t(2)).sibling_next));
            a20_tasklist_children_unlink_locked(v(t(1)));
            assert_eq!((*t(2)).sibling_next, core::ptr::null_mut());
            a20_tasklist_children_unlink_locked(v(t(2)));
            assert_eq!((*t(0)).children, core::ptr::null_mut());
            a20_tasklist_children_unlink_locked(v(t(2)));
        }
    }

    #[test]
    fn reparent_moves_the_child_and_reports_the_new_ppid() {
        let _guard = reset();
        unsafe {
            (*t(4)).pid = 104;
            a20_tasklist_children_link_locked(v(t(0)), v(t(1)));
            a20_tasklist_reparent_locked(v(t(4)), v(t(1)));
            assert_eq!((*t(1)).parent, t(4));
            assert_eq!((*t(1)).ppid, 104);
            assert_eq!((*t(0)).children, core::ptr::null_mut());
            assert_eq!((*t(4)).children, t(1));
        }
    }

    #[test]
    fn tg_link_pins_the_leader_once() {
        let _guard = reset();
        unsafe {
            (*t(0)).tg_leader = t(0);
            (*t(1)).tg_leader = t(0);
            assert_eq!(a20_tasklist_tg_link_locked(v(t(0))), 0, "a leader does not link to itself");
            assert_eq!(a20_tasklist_tg_link_locked(v(t(1))), 0);
            assert_eq!(a20_tasklist_tg_link_locked(v(t(1))), 0, "an already linked task is left alone");
            assert_eq!((*t(1)).tg_leader_ref_held, 1);
            assert_eq!((*t(0)).tg_next, t(1));
            assert_eq!((*t(1)).tg_prev_ptr, core::ptr::addr_of_mut!((*t(0)).tg_next));
        }
    }

    #[test]
    fn tg_link_reports_a_failed_leader_pin() {
        let _guard = reset();
        unsafe {
            (*t(1)).tg_leader = t(0);
            GET_OK = false;
            assert_eq!(a20_tasklist_tg_link_locked(v(t(1))), ESRCH);
            assert_eq!((*t(1)).tg_leader_ref_held, 0);
            assert_eq!((*t(0)).tg_next, core::ptr::null_mut());
        }
    }

    #[test]
    fn tg_unlink_keeps_the_pin_until_release() {
        let _guard = reset();
        unsafe {
            (*t(1)).tg_leader = t(0);
            a20_tasklist_tg_link_locked(v(t(1)));
            a20_tasklist_tg_unlink_locked(v(t(1)));
            assert_eq!((*t(0)).tg_next, core::ptr::null_mut());
            assert_eq!((*t(1)).tg_next, core::ptr::null_mut());
            assert_eq!((*t(1)).tg_prev_ptr, core::ptr::null_mut());
            assert_eq!((*t(1)).tg_leader_ref_held, 1, "the pin outlives list detachment");
            a20_tasklist_tg_leader_ref_release(v(t(1)));
            assert_eq!((*t(1)).tg_leader_ref_held, 0);
            assert_eq!((*t(1)).tg_leader, core::ptr::null_mut());
            assert_eq!(unsafe { PUT_COUNT }, 1);
            assert_eq!(unsafe { PUTS[0] }, 100, "the leader loses exactly one reference");
            a20_tasklist_tg_leader_ref_release(v(t(1)));
            assert_eq!(unsafe { PUT_COUNT }, 1, "release is idempotent");
        }
    }

    #[test]
    fn unlink_removes_all_three_memberships_and_the_list_ends() {
        let _guard = reset();
        unsafe {
            a20_tasklist_children_link_locked(v(t(0)), v(t(1)));
            (*t(1)).parent = t(0);
            (*t(1)).tg_leader = t(0);
            a20_tasklist_tg_link_locked(v(t(1)));
            publish(v(t(0)));
            publish(v(t(1)));
            publish(v(t(2)));
            a20_tasklist_unlink_locked(v(t(1)));
            assert_eq!(list_len(), 2);
            assert_eq!((*t(0)).all_next, t(2));
            assert_eq!((*t(2)).all_prev, t(0));
            assert_eq!((*t(0)).children, core::ptr::null_mut());
            assert_eq!((*t(0)).tg_next, core::ptr::null_mut());

            a20_tasklist_unlink_locked(v(t(0)));
            assert_eq!(list_len(), 1);
            assert_eq!(*(core::ptr::addr_of!(HEAD)), t(2));
            a20_tasklist_unlink_locked(v(t(2)));
            assert_eq!(list_len(), 0);
            assert_eq!(*(core::ptr::addr_of!(HEAD)), core::ptr::null_mut());
            assert_eq!(*(core::ptr::addr_of!(TAIL)), core::ptr::null_mut());
            a20_tasklist_unlink_locked(core::ptr::null_mut());
        }
    }

    #[test]
    fn first_and_next_walk_the_list() {
        let _guard = reset();
        unsafe {
            publish(v(t(3)));
            publish(v(t(4)));
            let mut cur = a20_tasklist_first_locked();
            assert_eq!(cur, v(t(3)));
            cur = a20_tasklist_next_locked(cur);
            assert_eq!(cur, v(t(4)));
            cur = a20_tasklist_next_locked(cur);
            assert!(cur.is_null());
            assert!(a20_tasklist_next_locked(core::ptr::null_mut()).is_null());
        }
    }

    #[test]
    fn next_refuses_a_link_that_is_not_a_kernel_pointer() {
        let _guard = reset();
        unsafe {
            publish(v(t(0)));
            publish(v(t(1)));
            KERNEL_PTRS = false;
            assert!(a20_tasklist_next_locked(v(t(0))).is_null());
            assert_eq!(CORRUPT_REPORTS, 1);
        }
    }

    #[test]
    fn next_refuses_a_misaligned_link() {
        let _guard = reset();
        unsafe {
            publish(v(t(0)));
            (*t(0)).all_next = 0x1235 as *mut FakeTask; // odd address
            assert!(a20_tasklist_next_locked(v(t(0))).is_null());
            assert_eq!(CORRUPT_REPORTS, 1);
        }
    }
}
