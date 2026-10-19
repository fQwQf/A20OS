#![no_std]

//! PID-namespace id spaces and membership transactions.
//!
//! Rust owns the per-namespace id allocator and every transaction that changes
//! a task's PID-namespace membership: acquiring one id per level with unwind on
//! failure, releasing them, the `unshare`/`fork`/`join` placement decisions and
//! the visibility predicate that decides whether a task's id is meaningful in a
//! namespace.
//!
//! C keeps the namespace objects (allocation, refcounts, the registry and child
//! lists, initial-namespace pinning), the per-namespace lock primitive, and the
//! task fields.  Those are exposed as `a20_pidns_*` accessors, so Rust reads and
//! writes the live objects under the caller's locks without a mirror of either
//! structure.

#[cfg(test)]
extern crate std;

use core::ffi::c_void;
use core::ptr;

const EAGAIN: i32 = 11;
const ENOMEM: i32 = 12;
const ESRCH: i32 = 3;
const EINVAL: i32 = 22;
const EUSERSRCH: i32 = 85;

const PID_MAX_LEVELS: usize = 4;
const PIDNS_CHILD_MAX: i32 = 32768;

const CLONE_THREAD: u64 = 0x0001_0000;
const CLONE_NEWPID: u64 = 0x2000_0000;

unsafe extern "C" {
    // Namespace object accessors.
    fn a20_pidns_level(ns: *const c_void) -> i32;
    fn a20_pidns_parent(ns: *const c_void) -> *mut c_void;
    fn a20_pidns_lock(ns: *mut c_void) -> u64;
    fn a20_pidns_unlock(ns: *mut c_void, flags: u64);
    fn a20_pidns_bitmap(ns: *mut c_void, words: *mut u32) -> *mut u64;
    fn a20_pidns_next_pid_ref(ns: *mut c_void) -> *mut i32;
    fn a20_pidns_nr_allocated_ref(ns: *mut c_void) -> *mut i32;
    fn a20_pidns_nr_tasks_ref(ns: *mut c_void) -> *mut i32;
    fn a20_pidns_alloc_linked(parent: *mut c_void) -> *mut c_void;
    fn a20_pidns_init_ns() -> *mut c_void;
    fn a20_pidns_get(ns: *mut c_void) -> *mut c_void;
    fn a20_pidns_put(ns: *mut c_void);
    // Task accessors.
    fn a20_pidns_task_own(task: *mut c_void) -> *mut c_void;
    fn a20_pidns_task_children(task: *mut c_void) -> *mut c_void;
    fn a20_pidns_task_level(task: *mut c_void) -> i32;
    fn a20_pidns_task_set_level(task: *mut c_void, level: i32);
    fn a20_pidns_task_id(task: *mut c_void, level: i32) -> i32;
    fn a20_pidns_task_set_id(task: *mut c_void, level: i32, id: i32);
    fn a20_pidns_task_set_own(task: *mut c_void, ns: *mut c_void);
    fn a20_pidns_task_set_children(task: *mut c_void, ns: *mut c_void);
}

#[inline]
fn ctz64(value: u64) -> i32 {
    if value == 0 { 64 } else { value.trailing_zeros() as i32 }
}

#[inline]
unsafe fn word_at(bitmap: *const u64, index: usize) -> u64 {
    unsafe { ptr::read(bitmap.add(index)) }
}

#[inline]
unsafe fn set_word(bitmap: *mut u64, index: usize, value: u64) {
    unsafe { ptr::write(bitmap.add(index), value) };
}

/// Free-id search over one namespace bitmap, mirroring the initial namespace's
/// allocator including the wrap.
fn find_free(bitmap: *const u64, words: u32, start: i32) -> i32 {
    if bitmap.is_null() || words == 0 {
        return -1;
    }
    let count = words as usize;
    let word_start = (start - 1) as usize / 64;
    for w in 0..count {
        let wi = (word_start + w) % count;
        let bits = unsafe { word_at(bitmap, wi) };
        if bits == u64::MAX {
            continue;
        }
        let mut inv = !bits;
        if wi == word_start {
            let start_bit = (start - 1) as u32 % 64;
            if start_bit > 0 {
                inv &= !((1u64 << start_bit) - 1);
            }
        }
        let first = ctz64(inv);
        if first == 64 {
            continue;
        }
        let candidate = (wi * 64) as i32 + first + 1;
        if candidate >= 1 && candidate <= PIDNS_CHILD_MAX {
            return candidate;
        }
    }
    -1
}

fn bitmap_set(bitmap: *mut u64, words: u32, pid: i32) {
    if bitmap.is_null() || pid < 1 {
        return;
    }
    let idx = (pid - 1) as usize / 64;
    if idx >= words as usize {
        return;
    }
    let bit = (pid - 1) as u32 % 64;
    let word = unsafe { word_at(bitmap, idx) };
    unsafe { set_word(bitmap, idx, word | (1u64 << bit)) };
}

fn bitmap_clear(bitmap: *mut u64, words: u32, pid: i32) {
    if bitmap.is_null() || pid < 1 {
        return;
    }
    let idx = (pid - 1) as usize / 64;
    if idx >= words as usize {
        return;
    }
    let bit = (pid - 1) as u32 % 64;
    let word = unsafe { word_at(bitmap, idx) };
    unsafe { set_word(bitmap, idx, word & !(1u64 << bit)) };
}

/// Reserve one id in `ns`; the caller holds `ns->lock`.
fn alloc_id_locked(ns: *mut c_void) -> i32 {
    let mut words = 0u32;
    let bitmap = unsafe { a20_pidns_bitmap(ns, &mut words) };
    if bitmap.is_null() || words == 0 {
        return -1;
    }
    let next_pid = unsafe { ptr::read(a20_pidns_next_pid_ref(ns)) };
    let mut pid = find_free(bitmap, words, next_pid);
    if pid < 0 && next_pid > 1 {
        pid = find_free(bitmap, words, 1);
    }
    if pid < 1 || pid > PIDNS_CHILD_MAX {
        return -EAGAIN;
    }
    bitmap_set(bitmap, words, pid);
    let next = if pid + 1 > PIDNS_CHILD_MAX { 1 } else { pid + 1 };
    unsafe { ptr::write(a20_pidns_next_pid_ref(ns), next) };
    unsafe {
        let allocated = a20_pidns_nr_allocated_ref(ns);
        ptr::write(allocated, ptr::read(allocated).wrapping_add(1));
    }
    pid
}

/// Acquire the ids `task` needs in `own` and every ancestor down to level 1.
///
/// One id per level: the ancestor's id space has no way to know which of its
/// own members is which.  The ids are written to `out` indexed by level and the
/// task is left untouched — the caller commits them.  Acquiring before
/// committing is what makes a failure non-destructive: once a task's old ids
/// were released it could not be put back, because the id it would get back is
/// no longer its own.
///
/// # Safety
/// `own` must be a live namespace and `out` four writable `i32`s.
unsafe fn ids_acquire(own: *mut c_void, out: *mut i32) -> i32 {
    let mut chain = [ptr::null_mut::<c_void>(); PID_MAX_LEVELS];
    let mut ids = [0i32; PID_MAX_LEVELS];
    let mut n = 0usize;

    for l in 0..PID_MAX_LEVELS {
        unsafe { ptr::write(out.add(l), 0) };
    }

    let mut cursor = own;
    while !cursor.is_null() && unsafe { a20_pidns_level(cursor) } > 0 {
        if n >= PID_MAX_LEVELS {
            return -EUSERSRCH;
        }
        chain[n] = cursor;
        n += 1;
        cursor = unsafe { a20_pidns_parent(cursor) };
    }

    for i in 0..n {
        let ns = chain[i];
        let flags = unsafe { a20_pidns_lock(ns) };
        let id = alloc_id_locked(ns);
        if id > 0 {
            // The C allocator already counted the id; the transaction counts
            // the reservation as well, and the `> 0` guards on release keep the
            // private counter harmless.  Reproduced rather than normalised so
            // the two implementations carry the same state.
            unsafe {
                let allocated = a20_pidns_nr_allocated_ref(ns);
                ptr::write(allocated, ptr::read(allocated).wrapping_add(1));
            }
            // nr_tasks counts MEMBERS, so only the namespace the task actually
            // joins counts it; the ancestor ids are addressability, not
            // membership.
            if i == 0 {
                unsafe {
                    let tasks = a20_pidns_nr_tasks_ref(ns);
                    ptr::write(tasks, ptr::read(tasks).wrapping_add(1));
                }
            }
        }
        unsafe { a20_pidns_unlock(ns, flags) };
        if id < 0 {
            // Unwind: a half-populated id space would leak ids forever.
            for j in 0..i {
                let victim = chain[j];
                let vf = unsafe { a20_pidns_lock(victim) };
                let mut words = 0u32;
                let bitmap = unsafe { a20_pidns_bitmap(victim, &mut words) };
                bitmap_clear(bitmap, words, ids[j]);
                unsafe {
                    let allocated = a20_pidns_nr_allocated_ref(victim);
                    let value = ptr::read(allocated);
                    if value > 0 {
                        ptr::write(allocated, value - 1);
                    }
                    if j == 0 {
                        let tasks = a20_pidns_nr_tasks_ref(victim);
                        let value = ptr::read(tasks);
                        if value > 0 {
                            ptr::write(tasks, value - 1);
                        }
                    }
                }
                unsafe { a20_pidns_unlock(victim, vf) };
            }
            return id;
        }
        ids[i] = id;
        let level = unsafe { a20_pidns_level(ns) };
        unsafe { ptr::write(out.add(level as usize), id) };
    }
    0
}

/// Release every id the task holds and clear its level bookkeeping.
///
/// # Safety
/// `task` must be live and its `pid_ns` must still point at the namespace to
/// walk.
unsafe fn ids_release(task: *mut c_void) {
    let mut ns = unsafe { a20_pidns_task_own(task) };
    let own_level = unsafe { a20_pidns_task_level(task) };

    let mut level = own_level;
    while level >= 1 && !ns.is_null() {
        let id = unsafe { a20_pidns_task_id(task, level) };
        if id > 0 {
            let flags = unsafe { a20_pidns_lock(ns) };
            let mut words = 0u32;
            let bitmap = unsafe { a20_pidns_bitmap(ns, &mut words) };
            bitmap_clear(bitmap, words, id);
            unsafe {
                let allocated = a20_pidns_nr_allocated_ref(ns);
                let value = ptr::read(allocated);
                if value > 0 {
                    ptr::write(allocated, value - 1);
                }
                if level == own_level {
                    let tasks = a20_pidns_nr_tasks_ref(ns);
                    let value = ptr::read(tasks);
                    if value > 0 {
                        ptr::write(tasks, value - 1);
                    }
                }
            }
            unsafe { a20_pidns_unlock(ns, flags) };
        }
        unsafe { a20_pidns_task_set_id(task, level, 0) };
        ns = unsafe { a20_pidns_parent(ns) };
        level -= 1;
    }
    unsafe { a20_pidns_task_set_level(task, 0) };
}

/// Move `task` between namespaces.  `own` is the namespace it becomes a member
/// of; `children` the one its children join.  Null leaves either alone.
unsafe fn assign(task: *mut c_void, own: *mut c_void, children: *mut c_void) -> i32 {
    let old_own = unsafe { a20_pidns_task_own(task) };

    if !own.is_null() && own != old_own {
        if unsafe { a20_pidns_level(own) } >= PID_MAX_LEVELS as i32 {
            return -EUSERSRCH;
        }
        let mut ids = [0i32; PID_MAX_LEVELS];
        let rc = unsafe { ids_acquire(own, ids.as_mut_ptr()) };
        if rc < 0 {
            return rc;
        }
        unsafe { a20_pidns_get(own) };
        unsafe { ids_release(task) };
        for level in 1..PID_MAX_LEVELS {
            unsafe { a20_pidns_task_set_id(task, level as i32, ids[level]) };
        }
        unsafe { a20_pidns_task_set_level(task, a20_pidns_level(own)) };
        unsafe { a20_pidns_task_set_own(task, own) };
        unsafe { a20_pidns_put(old_own) };
    }

    if !children.is_null() {
        let old = unsafe { a20_pidns_task_children(task) };
        if children != old {
            unsafe { a20_pidns_get(children) };
            unsafe { a20_pidns_task_set_children(task, children) };
            unsafe { a20_pidns_put(old) };
        }
    }
    0
}

/// Is `target` a member of `ns` or of a namespace below it?
///
/// # Safety
/// Both pointers must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pidns_visible(ns: *const c_void, target: *mut c_void) -> i32 {
    if ns.is_null() || target.is_null() {
        return 0;
    }
    let ns_level = unsafe { a20_pidns_level(ns) };
    let target_level = unsafe { a20_pidns_task_level(target) };
    if target_level < ns_level {
        return 0;
    }
    // Walk the target's own chain up to the namespace in question.  Every
    // namespace holds a reference on its parent, so this cannot dangle.
    let mut cursor = unsafe { a20_pidns_task_own(target) };
    let mut level = target_level;
    while level > ns_level && !cursor.is_null() {
        cursor = unsafe { a20_pidns_parent(cursor) };
        level -= 1;
    }
    (cursor == ns as *mut c_void) as i32
}

/// `unshare(CLONE_NEWPID)`: the caller joins a fresh namespace for its children
/// and stays where it is.
///
/// # Safety
/// `task` must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pidns_unshare(task: *mut c_void) -> i32 {
    if task.is_null() {
        return -ESRCH;
    }
    let parent = unsafe { a20_pidns_task_own(task) };
    if unsafe { a20_pidns_level(parent) } + 1 >= PID_MAX_LEVELS as i32 {
        return -EUSERSRCH;
    }
    let ns = unsafe { a20_pidns_alloc_linked(parent) };
    if ns.is_null() {
        return -ENOMEM;
    }
    let rc = unsafe { assign(task, ptr::null_mut(), ns) };
    unsafe { a20_pidns_put(ns) };
    rc
}

/// `clone(2)` placement: threads share the leader's namespaces, `CLONE_NEWPID`
/// gives the child a fresh namespace as its level-1 member, and everything else
/// joins the namespace the parent spawns into.
///
/// # Safety
/// `child` and `parent` must be live task pointers; the child must not be
/// published yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pidns_fork(
    child: *mut c_void,
    parent: *mut c_void,
    clone_flags: u64,
) -> i32 {
    if child.is_null() || parent.is_null() {
        return -ESRCH;
    }

    if clone_flags & CLONE_THREAD != 0 {
        let own = unsafe { a20_pidns_task_own(parent) };
        let kids = unsafe { a20_pidns_task_children(parent) };
        return unsafe { assign(child, own, kids) };
    }

    let mut fresh = false;
    let target;
    if clone_flags & CLONE_NEWPID != 0 {
        let base = unsafe { a20_pidns_task_own(parent) };
        if unsafe { a20_pidns_level(base) } + 1 >= PID_MAX_LEVELS as i32 {
            return -EUSERSRCH;
        }
        let ns = unsafe { a20_pidns_alloc_linked(base) };
        if ns.is_null() {
            return -ENOMEM;
        }
        target = ns;
        fresh = true;
    } else {
        let kids = unsafe { a20_pidns_task_children(parent) };
        // A task that never moved spawns into the initial namespace.
        target = if kids.is_null() { unsafe { a20_pidns_init_ns() } } else { kids };
    }

    let rc = unsafe { assign(child, target, target) };
    if fresh {
        unsafe { a20_pidns_put(target) };
    }
    rc
}

/// `setns(2)`: changes which namespace the caller's children join; it does not
/// move the caller.
///
/// # Safety
/// Both pointers must be null or live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pidns_join(task: *mut c_void, ns: *mut c_void) -> i32 {
    if task.is_null() || ns.is_null() {
        return -EINVAL;
    }
    if unsafe { a20_pidns_level(ns) } >= PID_MAX_LEVELS as i32 {
        return -EUSERSRCH;
    }
    unsafe { assign(task, ptr::null_mut(), ns) }
}

/// Drop a task's namespace references at teardown.
///
/// # Safety
/// `task` must be live and its ids not yet released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_pidns_release_task(task: *mut c_void) {
    if task.is_null() {
        return;
    }
    let own = unsafe { a20_pidns_task_own(task) };
    let children = unsafe { a20_pidns_task_children(task) };

    // Release the ids while pid_ns still points at the namespace to walk.
    unsafe { ids_release(task) };

    unsafe { a20_pidns_task_set_own(task, ptr::null_mut()) };
    unsafe { a20_pidns_task_set_children(task, ptr::null_mut()) };
    unsafe { a20_pidns_put(own) };
    unsafe { a20_pidns_put(children) };
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;
    use std::thread_local;
    use std::vec::Vec;

    // ---- test doubles --------------------------------------------------
    #[derive(Clone, Copy)]
    struct FakeNs {
        level: i32,
        parent: *mut FakeNs,
        next_pid: i32,
        nr_allocated: i32,
        nr_tasks: i32,
        words: u32,
        bitmap: [u64; 4], // enough for the tests' small id ranges
        locked: bool,
    }

    impl FakeNs {
        fn new(level: i32, parent: *mut FakeNs) -> Self {
            FakeNs {
                level,
                parent,
                next_pid: 1,
                nr_allocated: 0,
                nr_tasks: 0,
                words: 4,
                bitmap: [0; 4],
                locked: false,
            }
        }
    }

    #[derive(Clone, Copy)]
    struct FakeTask {
        own: *mut FakeNs,
        children: *mut FakeNs,
        level: i32,
        ids: [i32; PID_MAX_LEVELS],
    }

    impl FakeTask {
        fn new(own: *mut FakeNs) -> Self {
            FakeTask { own, children: own, level: 0, ids: [0; PID_MAX_LEVELS] }
        }
    }

    thread_local! {
        static EVENTS: RefCell<Vec<&'static str>> = const { RefCell::new(Vec::new()) };
        static FAIL_ALLOC_AT: RefCell<i32> = const { RefCell::new(0) };
        static INIT: std::cell::Cell<usize> = const { std::cell::Cell::new(0) };
    }

    fn record(event: &'static str) {
        EVENTS.with(|e| e.borrow_mut().push(event));
    }

    fn events() -> Vec<&'static str> {
        EVENTS.with(|e| e.borrow().clone())
    }

    fn reset() {
        EVENTS.with(|e| e.borrow_mut().clear());
        FAIL_ALLOC_AT.with(|f| *f.borrow_mut() = 0);
    }

    // Namespace accessors over FakeNs.
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_level(ns: *const c_void) -> i32 {
        if ns.is_null() { 0 } else { unsafe { (*(ns as *const FakeNs)).level } }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_parent(ns: *const c_void) -> *mut c_void {
        if ns.is_null() { ptr::null_mut() } else { unsafe { (*(ns as *const FakeNs)).parent as *mut c_void } }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_lock(ns: *mut c_void) -> u64 {
        unsafe { (*(ns as *mut FakeNs)).locked = true };
        record("lock");
        0
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_unlock(ns: *mut c_void, _flags: u64) {
        unsafe { (*(ns as *mut FakeNs)).locked = false };
        record("unlock");
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_bitmap(ns: *mut c_void, words: *mut u32) -> *mut u64 {
        let fake = unsafe { &mut *(ns as *mut FakeNs) };
        unsafe { ptr::write(words, fake.words) };
        fake.bitmap.as_mut_ptr()
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_next_pid_ref(ns: *mut c_void) -> *mut i32 {
        unsafe { &mut (*(ns as *mut FakeNs)).next_pid }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_nr_allocated_ref(ns: *mut c_void) -> *mut i32 {
        unsafe { &mut (*(ns as *mut FakeNs)).nr_allocated }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_nr_tasks_ref(ns: *mut c_void) -> *mut i32 {
        unsafe { &mut (*(ns as *mut FakeNs)).nr_tasks }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_alloc_linked(parent: *mut c_void) -> *mut c_void {
        record("alloc");
        if FAIL_ALLOC_AT.with(|f| *f.borrow()) > 0 {
            return ptr::null_mut();
        }
        let level = unsafe { a20_pidns_level(parent) } + 1;
        let boxed = std::boxed::Box::new(FakeNs::new(level, parent as *mut FakeNs));
        std::boxed::Box::into_raw(boxed) as *mut c_void
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_init_ns() -> *mut c_void {
        INIT.with(|i| i.get() as *mut c_void)
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_get(ns: *mut c_void) -> *mut c_void {
        record("get");
        ns
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_put(ns: *mut c_void) {
        if ns.is_null() {
            return;
        }
        record("put");
        // Test namespaces are leaked deliberately: a test may still hold a
        // pointer to a parent it created.
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_task_own(task: *mut c_void) -> *mut c_void {
        if task.is_null() { ptr::null_mut() } else { unsafe { (*(task as *mut FakeTask)).own as *mut c_void } }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_task_children(task: *mut c_void) -> *mut c_void {
        if task.is_null() { ptr::null_mut() } else { unsafe { (*(task as *mut FakeTask)).children as *mut c_void } }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_task_level(task: *mut c_void) -> i32 {
        unsafe { (*(task as *mut FakeTask)).level }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_task_set_level(task: *mut c_void, level: i32) {
        unsafe { (*(task as *mut FakeTask)).level = level };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_task_id(task: *mut c_void, level: i32) -> i32 {
        unsafe { (*(task as *mut FakeTask)).ids[level as usize] }
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_task_set_id(task: *mut c_void, level: i32, id: i32) {
        unsafe { (*(task as *mut FakeTask)).ids[level as usize] = id };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_task_set_own(task: *mut c_void, ns: *mut c_void) {
        unsafe { (*(task as *mut FakeTask)).own = ns as *mut FakeNs };
    }
    #[unsafe(no_mangle)]
    unsafe extern "C" fn a20_pidns_task_set_children(task: *mut c_void, ns: *mut c_void) {
        unsafe { (*(task as *mut FakeTask)).children = ns as *mut FakeNs };
    }

    fn initial() -> *mut FakeNs {
        let ns = std::boxed::Box::into_raw(std::boxed::Box::new(FakeNs::new(0, ptr::null_mut())));
        INIT.with(|i| i.set(ns as usize));
        ns
    }

    #[test]
    fn unshare_places_the_caller_in_a_new_children_namespace_only() {
        reset();
        let init = initial();
        let mut task = FakeTask::new(init);
        let t = &mut task as *mut FakeTask as *mut c_void;
        assert_eq!(unsafe { a20_pidns_unshare(t) }, 0);
        // The caller keeps its own membership (initial), and its children now
        // join the fresh namespace.
        assert_eq!(unsafe { a20_pidns_task_own(t) }, init as *mut c_void);
        let kids = unsafe { a20_pidns_task_children(t) };
        assert!(!kids.is_null());
        assert_eq!(unsafe { a20_pidns_level(kids) }, 1);
        let events = events();
        assert!(events.contains(&"alloc"), "{events:?}");
    }

    #[test]
    fn unshare_refuses_past_the_level_bound() {
        reset();
        let mut ns0 = FakeNs::new(0, ptr::null_mut());
        let mut ns1 = FakeNs::new(1, &mut ns0);
        let mut ns2 = FakeNs::new(2, &mut ns1);
        let mut ns3 = FakeNs::new(3, &mut ns2);
        let mut task = FakeTask::new(&mut ns3);
        let t = &mut task as *mut FakeTask as *mut c_void;
        assert_eq!(unsafe { a20_pidns_unshare(t) }, -EUSERSRCH);
    }

    #[test]
    fn a_fresh_namespace_child_gets_id_one_on_every_level() {
        reset();
        let init = initial();
        let mut parent = FakeTask::new(init);
        let p = &mut parent as *mut FakeTask as *mut c_void;
        let mut child = FakeTask::new(init);
        let c = &mut child as *mut FakeTask as *mut c_void;
        assert_eq!(unsafe { a20_pidns_fork(c, p, CLONE_NEWPID) }, 0);
        // The child is now level 1 in a fresh namespace, with id 1 there.
        let own = unsafe { a20_pidns_task_own(c) };
        assert_eq!(unsafe { a20_pidns_level(own) }, 1);
        assert_eq!(unsafe { a20_pidns_task_level(c) }, 1);
        assert_eq!(unsafe { a20_pidns_task_id(c, 1) }, 1);
    }

    #[test]
    fn a_thread_shares_its_leaders_namespace_and_gets_its_own_id() {
        reset();
        let init = initial();
        let mut ns = FakeNs::new(1, init as *mut FakeNs);
        let mut leader = FakeTask::new(&mut ns);
        leader.level = 1;
        leader.ids[1] = 1;
        ns.bitmap[0] |= 1; // id 1 is taken in the leader's namespace
        let mut thread = FakeTask::new(init);
        let l = &mut leader as *mut FakeTask as *mut c_void;
        let th = &mut thread as *mut FakeTask as *mut c_void;
        assert_eq!(unsafe { a20_pidns_fork(th, l, CLONE_THREAD) }, 0);
        assert_eq!(unsafe { a20_pidns_task_own(th) }, &mut ns as *mut FakeNs as *mut c_void);
        assert_eq!(unsafe { a20_pidns_task_id(th, 1) }, 2);
    }

    #[test]
    fn visibility_requires_the_targets_chain_to_contain_the_namespace() {
        reset();
        let init = initial();
        let mut ns_a = FakeNs::new(1, init as *mut FakeNs);
        let mut ns_b = FakeNs::new(1, init as *mut FakeNs); // sibling
        let mut in_a = FakeTask::new(&mut ns_a);
        in_a.level = 1;
        let mut in_b = FakeTask::new(&mut ns_b);
        in_b.level = 1;
        let a = &mut in_a as *mut FakeTask as *mut c_void;
        let b = &mut in_b as *mut FakeTask as *mut c_void;
        assert_eq!(unsafe { a20_pidns_visible(&mut ns_a as *mut FakeNs as *const c_void, a) }, 1);
        assert_eq!(unsafe { a20_pidns_visible(&mut ns_a as *mut FakeNs as *const c_void, b) }, 0,
                   "same id space shape, other chain");
        assert_eq!(unsafe { a20_pidns_visible(init as *const c_void, a) }, 1,
                   "ancestors see descendants");
        assert_eq!(unsafe { a20_pidns_visible(&mut ns_b as *mut FakeNs as *const c_void, a) }, 0);
    }

    #[test]
    fn acquisition_reserves_ids_without_committing_them() {
        reset();
        let init = initial();
        let mut ns = FakeNs::new(1, init as *mut FakeNs);
        let mut out = [0i32; PID_MAX_LEVELS];
        let rc = unsafe { ids_acquire(&mut ns as *mut FakeNs as *mut c_void, out.as_mut_ptr()) };
        assert_eq!(rc, 0);
        assert_eq!(out[1], 1);
        assert_eq!(ns.bitmap[0] & 1, 1, "id 1 is reserved");
        assert_eq!(ns.next_pid, 2);
    }

    #[test]
    fn assign_replaces_the_old_ids_and_keeps_exactly_one_reference() {
        reset();
        let init = initial();
        let mut ns1 = FakeNs::new(1, init as *mut FakeNs);
        let mut task = FakeTask::new(init);
        let t = &mut task as *mut FakeTask as *mut c_void;
        assert_eq!(unsafe { assign(t, &mut ns1 as *mut FakeNs as *mut c_void, ptr::null_mut()) }, 0);
        let events = events();
        assert!(events.contains(&"get"), "{events:?}");
        assert!(events.contains(&"put"), "old membership released: {events:?}");
        assert_eq!(unsafe { a20_pidns_task_level(t) }, 1);
        assert_eq!(unsafe { a20_pidns_task_id(t, 1) }, 1);
        assert_eq!(unsafe { a20_pidns_task_own(t) }, &mut ns1 as *mut FakeNs as *mut c_void);
    }

    #[test]
    fn a_failed_acquisition_unwinds_the_ids_it_already_took() {
        reset();
        let mut ns0 = FakeNs::new(0, ptr::null_mut());
        let mut full = FakeNs::new(1, &mut ns0);
        // Exhaust the level-1 id space.
        for pid in 1..=256 {
            let idx = (pid - 1) / 64;
            let bit = (pid - 1) % 64;
            full.bitmap[idx] |= 1 << bit;
        }
        let mut deep = FakeNs::new(2, &mut full);
        let mut out = [0i32; PID_MAX_LEVELS];
        let rc = unsafe { ids_acquire(&mut deep as *mut FakeNs as *mut c_void, out.as_mut_ptr()) };
        assert_eq!(rc, -EAGAIN);
        // The level-2 id taken before the failure was returned to the space.
        assert_eq!(deep.bitmap[0], 0, "unwind must clear the reserved bit");
        // The level-2 id that was reserved before the failure stays in `out`;
        // C does the same and its caller returns before reading the array.
        assert_eq!(out, [0, 0, 1, 0]);
    }

    #[test]
    fn release_drops_every_id_and_both_namespace_references() {
        reset();
        let init = initial();
        let mut ns1 = FakeNs::new(1, init as *mut FakeNs);
        let mut task = FakeTask::new(&mut ns1);
        task.level = 1;
        task.ids[1] = 3;
        task.children = &mut ns1;
        let t = &mut task as *mut FakeTask as *mut c_void;
        unsafe { a20_pidns_release_task(t) };
        assert_eq!(unsafe { a20_pidns_task_level(t) }, 0);
        assert_eq!(unsafe { a20_pidns_task_id(t, 1) }, 0);
        assert!(unsafe { a20_pidns_task_own(t) }.is_null());
        assert!(unsafe { a20_pidns_task_children(t) }.is_null());
        // Two namespace references were dropped.
        let puts = events().iter().filter(|e| **e == "put").count();
        assert_eq!(puts, 2, "{:?}", events());
    }

    #[test]
    fn join_only_changes_the_children_namespace() {
        reset();
        let init = initial();
        let mut target = FakeNs::new(1, init as *mut FakeNs);
        let mut task = FakeTask::new(init);
        task.level = 0;
        let t = &mut task as *mut FakeTask as *mut c_void;
        assert_eq!(unsafe { a20_pidns_join(t, &mut target as *mut FakeNs as *mut c_void) }, 0);
        assert_eq!(unsafe { a20_pidns_task_own(t) }, init as *mut c_void);
        assert_eq!(unsafe { a20_pidns_task_children(t) }, &mut target as *mut FakeNs as *mut c_void);
    }

    #[test]
    fn assignments_are_serialized_by_the_namespace_lock() {
        reset();
        let init = initial();
        let mut ns1 = FakeNs::new(1, init as *mut FakeNs);
        let mut task = FakeTask::new(init);
        let t = &mut task as *mut FakeTask as *mut c_void;
        assert_eq!(unsafe { assign(t, &mut ns1 as *mut FakeNs as *mut c_void, ptr::null_mut()) }, 0);
        // Every lock has a matching unlock and none is left held.
        let events = events();
        assert_eq!(
            events.iter().filter(|e| **e == "lock").count(),
            events.iter().filter(|e| **e == "unlock").count(),
            "{events:?}"
        );
        assert!(!ns1.locked);
    }
}
