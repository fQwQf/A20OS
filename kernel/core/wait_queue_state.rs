#![no_std]

//! Wait-queue entry membership transitions.
//!
//! C owns queue locks, task references, wake batching, and lifetime counters.
//! Every exported operation is called while the relevant queue lock(s) are
//! held. This unit owns the intrusive list and published owner pointer.

use core::ffi::c_void;
use core::mem::{offset_of, size_of};
use core::ptr;
use core::sync::atomic::{AtomicPtr, Ordering};

#[repr(C)]
pub struct Entry {
    next: *mut Entry,
    prev: *mut Entry,
    queue: *mut c_void,
    task: *mut c_void,
    wait_seq: u64,
    flags: u32,
    key: usize,
    priv_data: *mut c_void,
    linked: bool,
}

const _: () = {
    let p = size_of::<usize>();
    assert!(offset_of!(Entry, next) == 0);
    assert!(offset_of!(Entry, prev) == p);
    assert!(offset_of!(Entry, queue) == 2 * p);
    assert!(offset_of!(Entry, task) == 3 * p);
    assert!(offset_of!(Entry, wait_seq) == if p == 8 { 32 } else { 16 });
    assert!(offset_of!(Entry, flags) == if p == 8 { 40 } else { 24 });
    assert!(offset_of!(Entry, key) == if p == 8 { 48 } else { 28 });
    assert!(offset_of!(Entry, priv_data) == if p == 8 { 56 } else { 32 });
    assert!(offset_of!(Entry, linked) == if p == 8 { 64 } else { 36 });
    assert!(size_of::<Entry>() == if p == 8 { 72 } else { 40 });
};

unsafe fn owner(entry: *mut Entry) -> *mut c_void {
    // SAFETY: C supplies a live wait_queue_entry_t with the asserted ABI.
    let field = unsafe { ptr::addr_of_mut!((*entry).queue).cast::<AtomicPtr<c_void>>() };
    // SAFETY: AtomicPtr has pointer layout and queue is naturally aligned.
    unsafe { &*field }.load(Ordering::Acquire)
}

unsafe fn publish_owner(entry: *mut Entry, queue: *mut c_void) {
    // SAFETY: see owner(); queue ownership is published under the queue lock.
    let field = unsafe { ptr::addr_of_mut!((*entry).queue).cast::<AtomicPtr<c_void>>() };
    // SAFETY: AtomicPtr has pointer layout and queue is naturally aligned.
    unsafe { &*field }.store(queue, Ordering::Release);
}

#[no_mangle]
pub unsafe extern "C" fn a20_wait_queue_insert_rust(
    head: *mut *mut Entry,
    queue: *mut c_void,
    entry: *mut Entry,
    task: *mut c_void,
    seq: u64,
    key: usize,
) -> bool {
    if head.is_null() || queue.is_null() || entry.is_null() || task.is_null() || seq == 0 {
        return false;
    }
    // SAFETY: caller holds queue lock and passed live pointers.
    if unsafe { (*entry).linked } || !unsafe { owner(entry) }.is_null() {
        return false;
    }
    // SAFETY: fields are protected by caller-held queue lock.
    unsafe {
        (*entry).task = task;
        (*entry).wait_seq = seq;
        (*entry).flags = 0;
        (*entry).key = key;
        // `priv_data` belongs to the caller (for example, futex matching
        // stores its wait key there). The C implementation leaves this
        // opaque payload untouched when linking an entry.
        (*entry).prev = ptr::null_mut();
        (*entry).next = *head;
        if !(*head).is_null() {
            (**head).prev = entry;
        }
        *head = entry;
        (*entry).linked = true;
        publish_owner(entry, queue);
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn a20_wait_queue_detach_rust(
    head: *mut *mut Entry,
    queue: *mut c_void,
    entry: *mut Entry,
) -> *mut c_void {
    if head.is_null() || queue.is_null() || entry.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: caller holds queue lock; owner is revalidated by C before call.
    if unsafe { owner(entry) } != queue || !unsafe { (*entry).linked } {
        return ptr::null_mut();
    }
    // SAFETY: links/head are consistent under the caller-held lock.
    unsafe {
        let task = (*entry).task;
        let prev = (*entry).prev;
        let next = (*entry).next;
        if !prev.is_null() {
            (*prev).next = next;
        } else if *head == entry {
            *head = next;
        } else {
            return ptr::null_mut();
        }
        if !next.is_null() {
            (*next).prev = prev;
        }
        (*entry).next = ptr::null_mut();
        (*entry).prev = ptr::null_mut();
        (*entry).task = ptr::null_mut();
        (*entry).wait_seq = 0;
        (*entry).flags = 0;
        (*entry).key = 0;
        (*entry).priv_data = ptr::null_mut();
        (*entry).linked = false;
        publish_owner(entry, ptr::null_mut());
        task
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_wait_queue_requeue_rust(
    from_head: *mut *mut Entry,
    to_head: *mut *mut Entry,
    from: *mut c_void,
    to: *mut c_void,
    entry: *mut Entry,
) -> bool {
    if from_head.is_null() || to_head.is_null() || from.is_null() || to.is_null() || entry.is_null() {
        return false;
    }
    // SAFETY: both queue locks are held in address order by the C caller.
    if unsafe { owner(entry) } != from || !unsafe { (*entry).linked } {
        return false;
    }
    // SAFETY: caller holds both locks; update both lists before publishing new owner.
    unsafe {
        let prev = (*entry).prev;
        let next = (*entry).next;
        if !prev.is_null() {
            (*prev).next = next;
        } else if *from_head == entry {
            *from_head = next;
        } else {
            return false;
        }
        if !next.is_null() {
            (*next).prev = prev;
        }
        (*entry).prev = ptr::null_mut();
        (*entry).next = *to_head;
        if !(*to_head).is_null() {
            (**to_head).prev = entry;
        }
        *to_head = entry;
        publish_owner(entry, to);
    }
    true
}
