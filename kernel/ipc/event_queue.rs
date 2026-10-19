#![no_std]

//! Native event queue watch cancellation.
//!
//! The event queue and reverse index use two locks.  Rust owns the retry and
//! unlink transaction; C supplies the locks, hash-index removal, allocation
//! release and waiter wake primitives. RAII lock guards plus explicit
//! `Missing`/`Retry`/`Found` states make unlock ordering and handle-reuse
//! retries visible, while delivery owns filtering, ring commit and wake policy.

use core::ffi::c_void;

#[repr(C)]
struct WatchEntry {
    target_handle: u32,
    target_object: *mut c_void,
    target_type: u16,
    flags: u16,
    event_mask: u64,
    user_data: u64,
    fs_prefix: [u8; 256],
    owner_queue: *mut c_void,
    next: *mut WatchEntry,
}

#[repr(C)]
struct QueueView {
    watches: *mut *mut WatchEntry,
    watch_count: *mut u32,
}

#[repr(C)]
struct HashNode {
    object: *mut c_void,
    entry: *mut WatchEntry,
    next: *mut HashNode,
}

#[repr(C)]
struct PendingEvent {
    source: u32,
    kind: u32,
    events: u64,
    user_data: u64,
    data0: u64,
    data1: u64,
    data2: u64,
    fs_name: [u8; 32],
}

#[repr(C)]
struct RingView {
    ring: *mut PendingEvent,
    cap: *mut u32,
    head: *mut u32,
    tail: *mut u32,
    count: *mut u32,
}

unsafe extern "C" {
    fn a20_eventq_cancel_lock_queue(eq: *mut c_void) -> u64;
    fn a20_eventq_cancel_unlock_queue(eq: *mut c_void, flags: u64);
    fn a20_eventq_cancel_view(eq: *mut c_void, view: *mut QueueView);
    fn a20_eventq_cancel_lock_bucket(object: *mut c_void) -> u64;
    fn a20_eventq_cancel_unlock_bucket(object: *mut c_void, flags: u64);
    fn a20_eventq_cancel_remove_reverse(object: *mut c_void, entry: *mut WatchEntry);
    fn a20_eventq_cancel_free(entry: *mut WatchEntry);
    fn a20_eventq_cancel_wake(eq: *mut c_void);
    fn a20_eventq_destroy_bucket_head(object: *mut c_void,
                                      out: *mut *mut *mut HashNode);
    fn a20_eventq_destroy_free_node(node: *mut HashNode);
    fn a20_eventq_destroy_try_ref(eq: *mut c_void) -> i32;
    fn a20_eventq_destroy_put(eq: *mut c_void);
    fn a20_eventq_notify_bucket_head(object: *mut c_void, out: *mut *mut HashNode);
    fn a20_eventq_notify_ring_view(eq: *mut c_void, view: *mut RingView);
    fn a20_eventq_notify_wake_one(eq: *mut c_void);
}

const BAD_HANDLE: i64 = -5;
const NOT_FOUND: i64 = -24;

struct QueueGuard(*mut c_void, u64);
impl QueueGuard {
    unsafe fn acquire(eq: *mut c_void) -> Self {
        Self(eq, unsafe { a20_eventq_cancel_lock_queue(eq) })
    }
}
impl Drop for QueueGuard {
    fn drop(&mut self) {
        unsafe { a20_eventq_cancel_unlock_queue(self.0, self.1) }
    }
}

struct BucketGuard(*mut c_void, u64);
impl BucketGuard {
    unsafe fn acquire(object: *mut c_void) -> Self {
        Self(object, unsafe { a20_eventq_cancel_lock_bucket(object) })
    }
}
impl Drop for BucketGuard {
    fn drop(&mut self) {
        unsafe { a20_eventq_cancel_unlock_bucket(self.0, self.1) }
    }
}

enum CancelSearch {
    Missing,
    Found(*mut WatchEntry),
    Retry,
}

#[no_mangle]
pub unsafe extern "C" fn a20_eventq_cancel_rust(eq: *mut c_void, target: u32) -> i64 {
    if eq.is_null() {
        return BAD_HANDLE;
    }

    loop {
        // First discover the reverse-index bucket without holding both locks.
        let queue_guard = unsafe { QueueGuard::acquire(eq) };
        let mut view = QueueView { watches: core::ptr::null_mut(), watch_count: core::ptr::null_mut() };
        unsafe { a20_eventq_cancel_view(eq, &mut view) };
        let mut entry = unsafe { *view.watches };
        let mut object = core::ptr::null_mut();
        while !entry.is_null() {
            if unsafe { (*entry).target_handle } == target {
                object = unsafe { (*entry).target_object };
                break;
            }
            entry = unsafe { (*entry).next };
        }
        drop(queue_guard);
        if object.is_null() {
            return NOT_FOUND;
        }

        // Re-search in the required bucket -> queue lock order. A handle may
        // have been cancelled/reused during the discovery gap; retry if its
        // replacement belongs to another bucket.
        let bucket_guard = unsafe { BucketGuard::acquire(object) };
        let queue_guard = unsafe { QueueGuard::acquire(eq) };
        unsafe { a20_eventq_cancel_view(eq, &mut view) };
        let mut link = view.watches;
        let mut found = core::ptr::null_mut();
        while !link.is_null() {
            let candidate = unsafe { *link };
            if candidate.is_null() {
                break;
            }
            if unsafe { (*candidate).target_handle } == target {
                found = candidate;
                break;
            }
            link = unsafe { core::ptr::addr_of_mut!((*candidate).next) };
        }
        let outcome = if found.is_null() {
            CancelSearch::Missing
        } else if unsafe { (*found).target_object } != object {
            CancelSearch::Retry
        } else { CancelSearch::Found(found) };
        match outcome {
            CancelSearch::Missing => return NOT_FOUND,
            CancelSearch::Retry => { drop(queue_guard); drop(bucket_guard); continue; }
            CancelSearch::Found(found) => {
                unsafe {
                    *link = (*found).next;
                    *view.watch_count = (*view.watch_count).wrapping_sub(1);
                    a20_eventq_cancel_remove_reverse(object, found);
                }
                drop(queue_guard);
                drop(bucket_guard);
                unsafe {
                    a20_eventq_cancel_free(found);
                    a20_eventq_cancel_wake(eq);
                }
                return 0;
            }
        }
    }
}

/// Remove every watch for a dying object from both indexes. The bucket lock
/// serializes notify/cancel/release; the nested queue lock commits removal
/// from the owning list. Restarting after each unlink avoids retaining a link
/// into a node that a concurrent final queue release may free after unlock.
#[no_mangle]
pub unsafe extern "C" fn a20_eventq_destroy_object_rust(
    object: *mut c_void, object_type: u16,
) {
    if object.is_null() { return; }
    loop {
        let bucket_guard = unsafe { BucketGuard::acquire(object) };
        let mut head = core::ptr::null_mut();
        unsafe { a20_eventq_destroy_bucket_head(object, &mut head); }
        let mut link = head;
        let mut removed = None;
        while !link.is_null() {
            let node = unsafe { *link };
            if node.is_null() { break; }
            let entry = unsafe { (*node).entry };
            if unsafe { (*node).object == object && (*entry).target_type == object_type } {
                let eq = unsafe { (*entry).owner_queue };
                if unsafe { a20_eventq_destroy_try_ref(eq) } == 0 {
                    // Final release owns this still-linked node. Keep scanning
                    // the bucket: another queue may still own a live watch for
                    // the same object and must be detached before it is freed.
                    link = unsafe { core::ptr::addr_of_mut!((*node).next) };
                    continue;
                }
                let queue_guard = unsafe { QueueGuard::acquire(eq) };
                unsafe { *link = (*node).next; }
                let mut view = QueueView {
                    watches: core::ptr::null_mut(),
                    watch_count: core::ptr::null_mut(),
                };
                unsafe { a20_eventq_cancel_view(eq, &mut view); }
                let mut watch_link = view.watches;
                let mut queue_owned = false;
                while !watch_link.is_null() {
                    let watch = unsafe { *watch_link };
                    if watch.is_null() { break; }
                    if watch == entry {
                        unsafe {
                            *watch_link = (*watch).next;
                            *view.watch_count = (*view.watch_count).wrapping_sub(1);
                            queue_owned = true;
                        }
                        break;
                    }
                    watch_link = unsafe { core::ptr::addr_of_mut!((*watch).next) };
                }
                drop(queue_guard);
                removed = Some((node, entry, eq, queue_owned));
                break;
            }
            link = unsafe { core::ptr::addr_of_mut!((*node).next) };
        }
        if let Some((node, entry, eq, queue_owned)) = removed {
            unsafe {
                a20_eventq_destroy_free_node(node);
                if queue_owned { a20_eventq_cancel_free(entry); }
                a20_eventq_cancel_wake(eq);
            }
            drop(bucket_guard);
            unsafe { a20_eventq_destroy_put(eq); }
        } else {
            drop(bucket_guard);
            break;
        }
    }
}

unsafe fn notify_one(eq: *mut c_void, watch: *mut WatchEntry, kind: u32,
                     data0: u64, data1: u64, fs_name: *const u8) {
    let queue_guard = unsafe { QueueGuard::acquire(eq) };
    let mut view = RingView {
        ring: core::ptr::null_mut(), cap: core::ptr::null_mut(),
        head: core::ptr::null_mut(), tail: core::ptr::null_mut(),
        count: core::ptr::null_mut(),
    };
    unsafe { a20_eventq_notify_ring_view(eq, &mut view) };
    let cap = unsafe { *view.cap };
    let count = unsafe { *view.count };
    if count < cap {
        let mut event: PendingEvent = unsafe { core::mem::zeroed() };
        event.source = unsafe { (*watch).target_handle };
        event.kind = kind;
        event.events = 1u64 << kind;
        event.user_data = unsafe { (*watch).user_data };
        event.data0 = data0;
        event.data1 = data1;
        if !fs_name.is_null() {
            let mut n = 0;
            while n < event.fs_name.len() - 1 && unsafe { *fs_name.add(n) } != 0 {
                event.fs_name[n] = unsafe { *fs_name.add(n) };
                n += 1;
            }
        }
        let tail = unsafe { *view.tail };
        unsafe {
            view.ring.add(tail as usize).write(event);
            *view.tail = (tail + 1) % cap;
            *view.count = count + 1;
        }
    }
    drop(queue_guard);
    // Full-ring notifications still wake readers so they can recheck their
    // condition; this preserves the established wake-then-keep policy.
    unsafe { a20_eventq_notify_wake_one(eq) };
}

unsafe fn prefix_matches(name: *const u8, prefix: *const u8) -> bool {
    if unsafe { *prefix } == 0 {
        return true;
    }
    if name.is_null() {
        return false;
    }
    let mut i = 0usize;
    loop {
        let p = unsafe { *prefix.add(i) };
        if p == 0 {
            return true;
        }
        if unsafe { *name.add(i) } != p {
            return false;
        }
        i += 1;
    }
}

unsafe fn notify_hash_with_guard(object: *mut c_void, kind: u16, event_type: u32,
                      data0: u64, data1: u64, name: *const u8, fs: bool,
                      bucket_guard: BucketGuard) {
    let mut node = core::ptr::null_mut();
    unsafe { a20_eventq_notify_bucket_head(object, &mut node) };
    while !node.is_null() {
        let entry = unsafe { (*node).entry };
        let next = unsafe { (*node).next };
        if unsafe { (*node).object == object && (*entry).target_type == kind &&
                    (*entry).event_mask & (1u64 << event_type) != 0 } {
            let matches = if fs {
                unsafe { prefix_matches(name, (*entry).fs_prefix.as_ptr()) }
            } else { true };
            if matches {
                unsafe { notify_one((*entry).owner_queue, entry, event_type, data0, data1,
                                    if fs { name } else { core::ptr::null() }) };
            }
        }
        node = next;
    }
    drop(bucket_guard);
}

unsafe fn notify_hash(object: *mut c_void, kind: u16, event_type: u32,
                      data0: u64, data1: u64, name: *const u8, fs: bool) {
    unsafe { notify_hash_with_guard(object, kind, event_type, data0, data1,
        name, fs, BucketGuard::acquire(object)) }
}

unsafe fn notify_hash_locked(object: *mut c_void, kind: u16, event_type: u32,
                      data0: u64, data1: u64, name: *const u8, fs: bool,
                      bucket_flags: u64) {
    unsafe { notify_hash_with_guard(object, kind, event_type, data0, data1,
        name, fs, BucketGuard(object, bucket_flags)) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_event_notify_rust(object: *mut c_void, kind: u16,
    event_type: u32, data0: u64, data1: u64) {
    unsafe { notify_hash(object, kind, event_type, data0, data1, core::ptr::null(), false) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_event_notify_rust_locked(object: *mut c_void,
    kind: u16, event_type: u32, data0: u64, data1: u64, bucket_flags: u64) {
    unsafe { notify_hash_locked(object, kind, event_type, data0, data1,
        core::ptr::null(), false, bucket_flags) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_fs_notify_rust(object: *mut c_void, event_type: u32,
    name: *const u8, data0: u64, data1: u64) {
    unsafe { notify_hash(object, 4, event_type, data0, data1, name, true) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_fs_notify_rust_locked(object: *mut c_void,
    event_type: u32, name: *const u8, data0: u64, data1: u64,
    bucket_flags: u64) {
    unsafe { notify_hash_locked(object, 4, event_type, data0, data1,
        name, true, bucket_flags) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::sync::atomic::{AtomicUsize, Ordering};

    #[repr(C)]
    struct FakeQueue {
        head: *mut WatchEntry,
        count: u32,
        ring: [PendingEvent; 2],
        ring_cap: u32,
        ring_head: u32,
        ring_tail: u32,
        ring_count: u32,
    }

    static REMOVED: AtomicUsize = AtomicUsize::new(0);
    static FREED: AtomicUsize = AtomicUsize::new(0);
    static WOKEN: AtomicUsize = AtomicUsize::new(0);
    static VIEWS: AtomicUsize = AtomicUsize::new(0);
    static REPLACE_OBJECT: AtomicUsize = AtomicUsize::new(0);
    static BUCKET_LOCKS: AtomicUsize = AtomicUsize::new(0);
    static NODE_FREED: AtomicUsize = AtomicUsize::new(0);
    static DESTROY_REF_OK: AtomicUsize = AtomicUsize::new(1);
    static DESTROY_REF_FAIL_EQ: AtomicUsize = AtomicUsize::new(0);
    static DESTROY_PUTS: AtomicUsize = AtomicUsize::new(0);
    static HASH_HEAD: core::sync::atomic::AtomicPtr<HashNode> =
        core::sync::atomic::AtomicPtr::new(core::ptr::null_mut());
    static mut DESTROY_HEAD: *mut HashNode = core::ptr::null_mut();

    #[export_name = "a20_eventq_cancel_lock_queue"]
    unsafe extern "C" fn mock_lock_queue(_: *mut c_void) -> u64 { 0 }
    #[export_name = "a20_eventq_cancel_unlock_queue"]
    unsafe extern "C" fn mock_unlock_queue(_: *mut c_void, _: u64) {}
    #[export_name = "a20_eventq_cancel_view"]
    unsafe extern "C" fn mock_view(eq: *mut c_void, out: *mut QueueView) {
        let queue = unsafe { &mut *eq.cast::<FakeQueue>() };
        if VIEWS.fetch_add(1, Ordering::SeqCst) == 1 {
            let replacement = REPLACE_OBJECT.swap(0, Ordering::SeqCst);
            if replacement != 0 && !queue.head.is_null() {
                unsafe { (*queue.head).target_object = replacement as *mut c_void; }
            }
        }
        unsafe { (*out).watches = &mut queue.head; (*out).watch_count = &mut queue.count; }
    }
    #[export_name = "a20_eventq_cancel_lock_bucket"]
    unsafe extern "C" fn mock_lock_bucket(_: *mut c_void) -> u64 {
        BUCKET_LOCKS.fetch_add(1, Ordering::SeqCst);
        0
    }
    #[export_name = "a20_eventq_cancel_unlock_bucket"]
    unsafe extern "C" fn mock_unlock_bucket(_: *mut c_void, _: u64) {}
    #[export_name = "a20_eventq_cancel_remove_reverse"]
    unsafe extern "C" fn mock_remove_reverse(_: *mut c_void, _: *mut WatchEntry) {
        REMOVED.fetch_add(1, Ordering::SeqCst);
    }
    #[export_name = "a20_eventq_cancel_free"]
    unsafe extern "C" fn mock_free(_: *mut WatchEntry) {
        FREED.fetch_add(1, Ordering::SeqCst);
    }
    #[export_name = "a20_eventq_cancel_wake"]
    unsafe extern "C" fn mock_wake(_: *mut c_void) {
        WOKEN.fetch_add(1, Ordering::SeqCst);
    }
    #[export_name = "a20_eventq_destroy_bucket_head"]
    unsafe extern "C" fn mock_destroy_bucket_head(_: *mut c_void,
        out: *mut *mut *mut HashNode) {
        unsafe { out.write(core::ptr::addr_of_mut!(DESTROY_HEAD)); }
    }
    #[export_name = "a20_eventq_destroy_free_node"]
    unsafe extern "C" fn mock_destroy_free_node(_: *mut HashNode) {
        NODE_FREED.fetch_add(1, Ordering::SeqCst);
    }
    #[export_name = "a20_eventq_destroy_try_ref"]
    unsafe extern "C" fn mock_destroy_try_ref(eq: *mut c_void) -> i32 {
        if DESTROY_REF_FAIL_EQ.load(Ordering::SeqCst) == eq as usize {
            0
        } else {
            DESTROY_REF_OK.load(Ordering::SeqCst) as i32
        }
    }
    #[export_name = "a20_eventq_destroy_put"]
    unsafe extern "C" fn mock_destroy_put(_: *mut c_void) {
        DESTROY_PUTS.fetch_add(1, Ordering::SeqCst);
    }
    #[export_name = "a20_eventq_notify_bucket_head"]
    unsafe extern "C" fn mock_bucket_head(_: *mut c_void, out: *mut *mut HashNode) {
        unsafe { *out = HASH_HEAD.load(Ordering::SeqCst); }
    }
    #[export_name = "a20_eventq_notify_ring_view"]
    unsafe extern "C" fn mock_ring_view(eq: *mut c_void, out: *mut RingView) {
        let queue = unsafe { &mut *eq.cast::<FakeQueue>() };
        unsafe {
            (*out).ring = queue.ring.as_mut_ptr();
            (*out).cap = &mut queue.ring_cap;
            (*out).head = &mut queue.ring_head;
            (*out).tail = &mut queue.ring_tail;
            (*out).count = &mut queue.ring_count;
        }
    }
    #[export_name = "a20_eventq_notify_wake_one"]
    unsafe extern "C" fn mock_wake_one(eq: *mut c_void) { mock_wake(eq); }

    fn watch(handle: u32, object: usize, next: *mut WatchEntry) -> WatchEntry {
        WatchEntry { target_handle: handle, target_object: object as *mut c_void,
            target_type: 0, flags: 0, event_mask: 0, user_data: 0,
            fs_prefix: [0; 256], owner_queue: core::ptr::null_mut(), next }
    }

    fn empty_queue(head: *mut WatchEntry, count: u32) -> FakeQueue {
        FakeQueue { head, count,
            ring: unsafe { core::mem::zeroed() }, ring_cap: 2,
            ring_head: 0, ring_tail: 0, ring_count: 0 }
    }

    #[test]
    fn cancellation_unlinks_once_and_releases_reverse_index_and_waiters() {
        REMOVED.store(0, Ordering::SeqCst);
        FREED.store(0, Ordering::SeqCst);
        WOKEN.store(0, Ordering::SeqCst);
        let mut tail = watch(8, 0x2000, core::ptr::null_mut());
        let mut first = watch(7, 0x1000, &mut tail);
        let mut queue = empty_queue(&mut first, 2);
        let result = unsafe { a20_eventq_cancel_rust((&mut queue as *mut FakeQueue).cast(), 7) };
        assert_eq!(result, 0);
        assert_eq!(queue.head, &mut tail as *mut WatchEntry);
        assert_eq!(queue.count, 1);
        assert_eq!(REMOVED.load(Ordering::SeqCst), 1);
        assert_eq!(FREED.load(Ordering::SeqCst), 1);
        assert_eq!(WOKEN.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn missing_handle_does_not_mutate_or_wake() {
        REMOVED.store(0, Ordering::SeqCst);
        FREED.store(0, Ordering::SeqCst);
        WOKEN.store(0, Ordering::SeqCst);
        let mut entry = watch(7, 0x1000, core::ptr::null_mut());
        let mut queue = empty_queue(&mut entry, 1);
        let result = unsafe { a20_eventq_cancel_rust((&mut queue as *mut FakeQueue).cast(), 8) };
        assert_eq!(result, NOT_FOUND);
        assert_eq!(queue.head, &mut entry as *mut WatchEntry);
        assert_eq!(queue.count, 1);
        assert_eq!(REMOVED.load(Ordering::SeqCst), 0);
        assert_eq!(FREED.load(Ordering::SeqCst), 0);
        assert_eq!(WOKEN.load(Ordering::SeqCst), 0);
    }

    #[test]
    fn object_destroy_removes_reverse_and_queue_links_before_freeing() {
        REMOVED.store(0, Ordering::SeqCst);
        FREED.store(0, Ordering::SeqCst);
        NODE_FREED.store(0, Ordering::SeqCst);
        DESTROY_REF_OK.store(1, Ordering::SeqCst);
        DESTROY_REF_FAIL_EQ.store(0, Ordering::SeqCst);
        DESTROY_PUTS.store(0, Ordering::SeqCst);
        WOKEN.store(0, Ordering::SeqCst);
        let object = 0x5000usize as *mut c_void;
        let mut queue = empty_queue(core::ptr::null_mut(), 1);
        let mut entry = watch(21, object as usize, core::ptr::null_mut());
        entry.target_type = 7;
        entry.owner_queue = (&mut queue as *mut FakeQueue).cast();
        queue.head = &mut entry;
        let mut node = HashNode { object, entry: &mut entry, next: core::ptr::null_mut() };
        unsafe { DESTROY_HEAD = &mut node; }

        unsafe { a20_eventq_destroy_object_rust(object, 7); }

        assert!(queue.head.is_null());
        assert_eq!(queue.count, 0);
        assert!(unsafe { DESTROY_HEAD }.is_null());
        assert_eq!(NODE_FREED.load(Ordering::SeqCst), 1);
        assert_eq!(FREED.load(Ordering::SeqCst), 1);
        assert_eq!(WOKEN.load(Ordering::SeqCst), 1);
        assert_eq!(DESTROY_PUTS.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn object_destroy_leaves_detached_entry_for_queue_release_owner() {
        FREED.store(0, Ordering::SeqCst);
        NODE_FREED.store(0, Ordering::SeqCst);
        WOKEN.store(0, Ordering::SeqCst);
        DESTROY_REF_OK.store(0, Ordering::SeqCst);
        DESTROY_REF_FAIL_EQ.store(0, Ordering::SeqCst);
        let object = 0x6000usize as *mut c_void;
        let mut queue = empty_queue(core::ptr::null_mut(), 0);
        let mut entry = watch(22, object as usize, core::ptr::null_mut());
        entry.target_type = 7;
        entry.owner_queue = (&mut queue as *mut FakeQueue).cast();
        let mut node = HashNode { object, entry: &mut entry, next: core::ptr::null_mut() };
        unsafe { DESTROY_HEAD = &mut node; }

        unsafe { a20_eventq_destroy_object_rust(object, 7); }

        assert_eq!(unsafe { DESTROY_HEAD }, &mut node as *mut HashNode);
        assert_eq!(NODE_FREED.load(Ordering::SeqCst), 0);
        assert_eq!(FREED.load(Ordering::SeqCst), 0);
        assert_eq!(WOKEN.load(Ordering::SeqCst), 0);
        assert_eq!(DESTROY_PUTS.load(Ordering::SeqCst), 0);
        // Final release remains responsible for this still-linked node.
    }

    #[test]
    fn object_destroy_skips_finalizing_queue_and_purges_live_owner() {
        FREED.store(0, Ordering::SeqCst);
        NODE_FREED.store(0, Ordering::SeqCst);
        WOKEN.store(0, Ordering::SeqCst);
        DESTROY_PUTS.store(0, Ordering::SeqCst);
        DESTROY_REF_OK.store(1, Ordering::SeqCst);
        let object = 0x7000usize as *mut c_void;
        // Final release already owns the first entry's queue list. The second
        // entry belongs to a live queue and must still be purged in this pass.
        let mut final_queue = empty_queue(core::ptr::null_mut(), 0);
        let mut live_queue = empty_queue(core::ptr::null_mut(), 1);
        let mut final_entry = watch(23, object as usize, core::ptr::null_mut());
        final_entry.target_type = 7;
        final_entry.owner_queue = (&mut final_queue as *mut FakeQueue).cast();
        let mut live_entry = watch(24, object as usize, core::ptr::null_mut());
        live_entry.target_type = 7;
        live_entry.owner_queue = (&mut live_queue as *mut FakeQueue).cast();
        live_queue.head = &mut live_entry;
        let mut live_node = HashNode { object, entry: &mut live_entry, next: core::ptr::null_mut() };
        let mut final_node = HashNode { object, entry: &mut final_entry, next: &mut live_node };
        DESTROY_REF_FAIL_EQ.store((&mut final_queue as *mut FakeQueue) as usize, Ordering::SeqCst);
        unsafe { DESTROY_HEAD = &mut final_node; }

        unsafe { a20_eventq_destroy_object_rust(object, 7); }

        assert_eq!(unsafe { DESTROY_HEAD }, &mut final_node as *mut HashNode);
        assert_eq!(final_node.next, core::ptr::null_mut());
        assert!(live_queue.head.is_null());
        assert_eq!(live_queue.count, 0);
        assert_eq!(NODE_FREED.load(Ordering::SeqCst), 1);
        assert_eq!(FREED.load(Ordering::SeqCst), 1);
        assert_eq!(WOKEN.load(Ordering::SeqCst), 1);
        assert_eq!(DESTROY_PUTS.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn handle_reuse_with_new_bucket_retries_before_unlink() {
        REMOVED.store(0, Ordering::SeqCst);
        FREED.store(0, Ordering::SeqCst);
        WOKEN.store(0, Ordering::SeqCst);
        VIEWS.store(0, Ordering::SeqCst);
        BUCKET_LOCKS.store(0, Ordering::SeqCst);
        REPLACE_OBJECT.store(0x3000, Ordering::SeqCst);
        let mut entry = watch(9, 0x1000, core::ptr::null_mut());
        let mut queue = empty_queue(&mut entry, 1);
        let result = unsafe { a20_eventq_cancel_rust((&mut queue as *mut FakeQueue).cast(), 9) };
        assert_eq!(result, 0);
        assert!(queue.head.is_null());
        assert_eq!(queue.count, 0);
        assert_eq!(BUCKET_LOCKS.load(Ordering::SeqCst), 2);
        assert_eq!(REMOVED.load(Ordering::SeqCst), 1);
        assert_eq!(FREED.load(Ordering::SeqCst), 1);
        assert_eq!(WOKEN.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn notification_commits_event_payload_before_waking() {
        WOKEN.store(0, Ordering::SeqCst);
        let mut entry = watch(23, 0x4400, core::ptr::null_mut());
        entry.target_type = 7;
        entry.event_mask = 1 << 6;
        entry.user_data = 0xfeed;
        let mut queue = empty_queue(&mut entry, 1);
        entry.owner_queue = (&mut queue as *mut FakeQueue).cast();
        let mut node = HashNode { object: 0x4400 as *mut c_void,
            entry: &mut entry, next: core::ptr::null_mut() };
        HASH_HEAD.store(&mut node, Ordering::SeqCst);
        unsafe { a20_event_notify_rust(0x4400 as *mut c_void, 7, 6, 0xabc, 0xdef) };
        HASH_HEAD.store(core::ptr::null_mut(), Ordering::SeqCst);
        assert_eq!(queue.ring_count, 1);
        assert_eq!(queue.ring_tail, 1);
        assert_eq!(queue.ring[0].source, 23);
        assert_eq!(queue.ring[0].kind, 6);
        assert_eq!(queue.ring[0].events, 1 << 6);
        assert_eq!(queue.ring[0].user_data, 0xfeed);
        assert_eq!(queue.ring[0].data0, 0xabc);
        assert_eq!(queue.ring[0].data1, 0xdef);
        assert_eq!(WOKEN.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn filesystem_notification_filters_full_prefix_and_copies_bounded_name() {
        WOKEN.store(0, Ordering::SeqCst);
        let mut entry = watch(31, 0x5500, core::ptr::null_mut());
        entry.target_type = 4;
        entry.event_mask = 1 << 11;
        entry.fs_prefix[..6].copy_from_slice(b"prefix");
        let mut queue = empty_queue(&mut entry, 1);
        entry.owner_queue = (&mut queue as *mut FakeQueue).cast();
        let mut node = HashNode { object: 0x5500 as *mut c_void,
            entry: &mut entry, next: core::ptr::null_mut() };
        HASH_HEAD.store(&mut node, Ordering::SeqCst);
        let name = b"prefix-abcdefghijklmnopqrstuvwxyz-0123456789\0";
        unsafe { a20_fs_notify_rust(0x5500 as *mut c_void, 11, name.as_ptr(), 4, 5) };
        assert_eq!(queue.ring_count, 1);
        assert_eq!(&queue.ring[0].fs_name, b"prefix-abcdefghijklmnopqrstuvwx\0");
        let before = queue.ring_tail;
        unsafe { a20_fs_notify_rust(0x5500 as *mut c_void, 11, b"other\0".as_ptr(), 0, 0) };
        HASH_HEAD.store(core::ptr::null_mut(), Ordering::SeqCst);
        assert_eq!(queue.ring_count, 1);
        assert_eq!(queue.ring_tail, before);
        assert_eq!(WOKEN.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn full_ring_keeps_events_but_still_wakes_waiters() {
        WOKEN.store(0, Ordering::SeqCst);
        let mut entry = watch(45, 0x6600, core::ptr::null_mut());
        entry.target_type = 7;
        entry.event_mask = 1 << 2;
        let mut queue = empty_queue(&mut entry, 1);
        queue.ring_count = queue.ring_cap;
        entry.owner_queue = (&mut queue as *mut FakeQueue).cast();
        let mut node = HashNode { object: 0x6600 as *mut c_void,
            entry: &mut entry, next: core::ptr::null_mut() };
        HASH_HEAD.store(&mut node, Ordering::SeqCst);
        unsafe { a20_event_notify_rust(0x6600 as *mut c_void, 7, 2, 0, 0) };
        HASH_HEAD.store(core::ptr::null_mut(), Ordering::SeqCst);
        assert_eq!(queue.ring_count, 2);
        assert_eq!(queue.ring_tail, 0);
        assert_eq!(queue.ring[0].source, 0);
        assert_eq!(WOKEN.load(Ordering::SeqCst), 1);
    }
}
