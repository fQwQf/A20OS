#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]
#![warn(rust_2018_idioms)]

mod ffi;

use a20rust_support::lock::raw_irqsave_lock;
use core::ffi::{c_char, c_void};
use core::mem::size_of;
use core::ptr;

use ffi::{
    a20_event_kfree, a20_event_kmalloc, a20_event_memset, a20_event_spin_init,
    a20_event_spin_set_debug, a20_event_wait_queue_init, a20_event_wait_queue_wake_all,
    a20_event_wait_queue_wake_one, a20_eventq_t, a20_handle_t, a20_pending_event_t,
    a20_watch_entry_t, spinlock_t, A20_ERR_BAD_HANDLE, A20_ERR_FAULT,
    A20_ERR_INVALID_ARGUMENT, A20_ERR_NO_MEMORY, A20_ERR_NOT_FOUND, A20_ERR_NO_SPACE,
    A20_ERR_WOULD_BLOCK, A20_EVQ_DEFAULT_CAP, A20_OK,
};

const A20_EVQ_HASH_BITS: usize = 8;
const A20_EVQ_HASH_SIZE: usize = 1 << A20_EVQ_HASH_BITS;
const A20_EVQ_HASH_MASK: usize = A20_EVQ_HASH_SIZE - 1;

const A20_EVENTQ_NAME: &[u8] = b"a20_eventq\0";
const A20_EVENTQ_WAITERS_NAME: &[u8] = b"a20_eventq.waiters\0";

#[repr(C)]
struct ObjWatchNode {
    object: *mut c_void,
    entry: *mut a20_watch_entry_t,
    next: *mut ObjWatchNode,
}

static mut G_EVQ_HASH_LOCK: spinlock_t = spinlock_t { locked: 0 };
static mut G_EVQ_HASH: [*mut ObjWatchNode; A20_EVQ_HASH_SIZE] = [ptr::null_mut(); A20_EVQ_HASH_SIZE];
static mut G_EVQ_HASH_INITIALIZED: bool = false;

#[inline(always)]
unsafe fn irqsave_lock(lock: *mut spinlock_t) -> a20rust_support::lock::RawIrqSaveGuard {
    unsafe { raw_irqsave_lock(lock.cast()) }
}

#[inline(always)]
unsafe fn zeroed_alloc(size: usize) -> *mut c_void {
    let ptr = unsafe { a20_event_kmalloc(size) };
    if !ptr.is_null() {
        unsafe { a20_event_memset(ptr, 0, size) };
    }
    ptr
}

#[inline(always)]
fn evq_hash_ptr(ptr_: *mut c_void) -> usize {
    let mut v = ptr_ as usize;
    v = ((v >> 4) ^ (v >> 16)) & A20_EVQ_HASH_MASK;
    v
}

unsafe fn evq_hash_init() {
    if unsafe { !G_EVQ_HASH_INITIALIZED } {
        unsafe {
            a20_event_spin_init(ptr::addr_of_mut!(G_EVQ_HASH_LOCK));
            a20_event_memset(
                ptr::addr_of_mut!(G_EVQ_HASH).cast(),
                0,
                size_of::<[*mut ObjWatchNode; A20_EVQ_HASH_SIZE]>(),
            );
            G_EVQ_HASH_INITIALIZED = true;
        }
    }
}

unsafe fn evq_hash_insert(object: *mut c_void, entry: *mut a20_watch_entry_t) {
    unsafe { evq_hash_init() };
    let node = unsafe { zeroed_alloc(size_of::<ObjWatchNode>()) }.cast::<ObjWatchNode>();
    if node.is_null() {
        return;
    }

    unsafe {
        (*node).object = object;
        (*node).entry = entry;
        (*node).next = ptr::null_mut();
    }

    let idx = evq_hash_ptr(object);
    let _guard = unsafe { irqsave_lock(ptr::addr_of_mut!(G_EVQ_HASH_LOCK)) };
    unsafe {
        (*node).next = G_EVQ_HASH[idx];
        G_EVQ_HASH[idx] = node;
    }
}

unsafe fn evq_hash_remove(object: *mut c_void, entry: *mut a20_watch_entry_t) {
    unsafe { evq_hash_init() };
    let idx = evq_hash_ptr(object);
    let _guard = unsafe { irqsave_lock(ptr::addr_of_mut!(G_EVQ_HASH_LOCK)) };
    let mut link = unsafe { ptr::addr_of_mut!(G_EVQ_HASH[idx]) };
    loop {
        let cur = unsafe { *link };
        if cur.is_null() {
            break;
        }
        if unsafe { (*cur).entry == entry } {
            unsafe {
                *link = (*cur).next;
                a20_event_kfree(cur.cast());
            }
            break;
        }
        link = unsafe { ptr::addr_of_mut!((*cur).next) };
    }
}

#[inline(always)]
unsafe fn evq_ring_put(eq: *mut a20_eventq_t, ev: &a20_pending_event_t) -> i64 {
    unsafe {
        if (*eq).ring_count >= (*eq).ring_cap {
            return -A20_ERR_NO_SPACE;
        }
        *(*eq).ring.add((*eq).ring_tail as usize) = *ev;
        (*eq).ring_tail = ((*eq).ring_tail + 1) % (*eq).ring_cap;
        (*eq).ring_count += 1;
    }
    A20_OK
}

#[no_mangle]
pub unsafe extern "C" fn a20_eventq_create(capacity_hint: u32) -> *mut a20_eventq_t {
    let cap = if capacity_hint == 0 {
        A20_EVQ_DEFAULT_CAP
    } else {
        capacity_hint
    };

    let eq = unsafe { zeroed_alloc(size_of::<a20_eventq_t>()) }.cast::<a20_eventq_t>();
    if eq.is_null() {
        return ptr::null_mut();
    }

    unsafe {
        (*eq).refcount.set(1);
        a20_event_spin_init(ptr::addr_of_mut!((*eq).lock));
        a20_event_spin_set_debug(
            ptr::addr_of_mut!((*eq).lock),
            A20_EVENTQ_NAME.as_ptr().cast::<c_char>(),
            eq.cast(),
        );
        a20_event_wait_queue_init(ptr::addr_of_mut!((*eq).waiters));
        a20_event_spin_set_debug(
            ptr::addr_of_mut!((*eq).waiters.lock),
            A20_EVENTQ_WAITERS_NAME.as_ptr().cast::<c_char>(),
            eq.cast(),
        );
        (*eq).ring_cap = cap;
    }

    let ring_bytes = match (cap as usize).checked_mul(size_of::<a20_pending_event_t>()) {
        Some(v) => v,
        None => {
            unsafe { a20_event_kfree(eq.cast()) };
            return ptr::null_mut();
        }
    };
    let ring = unsafe { zeroed_alloc(ring_bytes) }.cast::<a20_pending_event_t>();
    if ring.is_null() {
        unsafe { a20_event_kfree(eq.cast()) };
        return ptr::null_mut();
    }

    unsafe {
        (*eq).ring = ring;
    }
    eq
}

#[no_mangle]
pub unsafe extern "C" fn a20_eventq_watch(
    eq: *mut a20_eventq_t,
    target_h: a20_handle_t,
    target_obj: *mut c_void,
    target_type: u16,
    event_mask: u64,
    user_data: u64,
) -> i64 {
    if eq.is_null() || target_obj.is_null() {
        return -A20_ERR_INVALID_ARGUMENT;
    }

    let new_watch = unsafe { zeroed_alloc(size_of::<a20_watch_entry_t>()) }.cast::<a20_watch_entry_t>();
    if new_watch.is_null() {
        return -A20_ERR_NO_MEMORY;
    }

    {
        let _guard = unsafe { irqsave_lock(ptr::addr_of_mut!((*eq).lock)) };
        let mut watch = unsafe { (*eq).watches };
        while !watch.is_null() {
            if unsafe { (*watch).target_object == target_obj } {
                unsafe {
                    (*watch).event_mask = event_mask;
                    (*watch).user_data = user_data;
                }
                unsafe { a20_event_kfree(new_watch.cast()) };
                return A20_OK;
            }
            watch = unsafe { (*watch).next };
        }

        unsafe {
            (*new_watch).target_handle = target_h;
            (*new_watch).target_object = target_obj;
            (*new_watch).target_type = target_type;
            (*new_watch)._pad = 0;
            (*new_watch).event_mask = event_mask;
            (*new_watch).user_data = user_data;
            (*new_watch).owner_queue = eq;
            (*new_watch).next = (*eq).watches;
            (*eq).watches = new_watch;
            (*eq).watch_count = (*eq).watch_count.wrapping_add(1);
        }
    }

    unsafe { evq_hash_insert(target_obj, new_watch) };
    A20_OK
}

#[no_mangle]
pub unsafe extern "C" fn a20_eventq_wait(
    eq: *mut a20_eventq_t,
    out: *mut a20_pending_event_t,
    timeout_ns: u64,
) -> i64 {
    if eq.is_null() || out.is_null() {
        return -A20_ERR_FAULT;
    }

    {
        let _guard = unsafe { irqsave_lock(ptr::addr_of_mut!((*eq).lock)) };
        if unsafe { (*eq).ring_count > 0 } {
            unsafe {
                *out = *(*eq).ring.add((*eq).ring_head as usize);
                (*eq).ring_head = ((*eq).ring_head + 1) % (*eq).ring_cap;
                (*eq).ring_count -= 1;
            }
            return A20_OK;
        }
    }

    if timeout_ns == 0 {
        return -A20_ERR_WOULD_BLOCK;
    }
    -A20_ERR_WOULD_BLOCK
}

#[no_mangle]
pub unsafe extern "C" fn a20_eventq_cancel(eq: *mut a20_eventq_t, target_h: a20_handle_t) -> i64 {
    if eq.is_null() {
        return -A20_ERR_BAD_HANDLE;
    }

    let mut removed = ptr::null_mut::<a20_watch_entry_t>();
    {
        let _guard = unsafe { irqsave_lock(ptr::addr_of_mut!((*eq).lock)) };
        let mut link = unsafe { ptr::addr_of_mut!((*eq).watches) };
        loop {
            let cur = unsafe { *link };
            if cur.is_null() {
                break;
            }
            if unsafe { (*cur).target_handle == target_h } {
                unsafe {
                    *link = (*cur).next;
                    (*eq).watch_count -= 1;
                }
                removed = cur;
                break;
            }
            link = unsafe { ptr::addr_of_mut!((*cur).next) };
        }
    }

    if removed.is_null() {
        return -A20_ERR_NOT_FOUND;
    }

    let target_obj = unsafe { (*removed).target_object };
    unsafe {
        evq_hash_remove(target_obj, removed);
        a20_event_kfree(removed.cast());
    }
    A20_OK
}

#[no_mangle]
pub unsafe extern "C" fn a20_eventq_release(eq: *mut a20_eventq_t) {
    if eq.is_null() {
        return;
    }
    if !unsafe { (*eq).refcount.dec_and_test() } {
        return;
    }

    let mut watch = unsafe { (*eq).watches };
    while !watch.is_null() {
        let next = unsafe { (*watch).next };
        unsafe {
            evq_hash_remove((*watch).target_object, watch);
            a20_event_kfree(watch.cast());
        }
        watch = next;
    }

    unsafe {
        a20_event_wait_queue_wake_all(ptr::addr_of_mut!((*eq).waiters));
        a20_event_kfree((*eq).ring.cast());
        a20_event_kfree(eq.cast());
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_event_notify(
    target_object: *mut c_void,
    _target_type: u16,
    event_type: u32,
    data0: u64,
    data1: u64,
) {
    unsafe { evq_hash_init() };
    let idx = evq_hash_ptr(target_object);
    let event_bit = if event_type < 64 { 1u64 << event_type } else { 0 };
    let mut wake_count = 0usize;
    let mut wake_queues: [*mut a20_eventq_t; 32] = [ptr::null_mut(); 32];

    {
        let _hash_guard = unsafe { irqsave_lock(ptr::addr_of_mut!(G_EVQ_HASH_LOCK)) };
        let mut node = unsafe { G_EVQ_HASH[idx] };
        while !node.is_null() {
            let entry = unsafe { (*node).entry };
            if !entry.is_null()
                && unsafe { (*entry).target_object == target_object }
                && unsafe { (*entry).event_mask & event_bit } != 0
            {
                let eq = unsafe { (*entry).owner_queue };
                if !eq.is_null() {
                    let should_wake = {
                        let _eq_guard = unsafe { irqsave_lock(ptr::addr_of_mut!((*eq).lock)) };
                        if unsafe { (*eq).ring_count < (*eq).ring_cap } {
                            let ev = a20_pending_event_t {
                                source: unsafe { (*entry).target_handle },
                                type_: event_type,
                                events: event_bit,
                                user_data: unsafe { (*entry).user_data },
                                data0,
                                data1,
                                data2: 0,
                            };
                            let _ = unsafe { evq_ring_put(eq, &ev) };
                            true
                        } else {
                            true
                        }
                    };
                    if should_wake && wake_count < wake_queues.len() {
                        wake_queues[wake_count] = eq;
                        wake_count += 1;
                    }
                }
            }
            node = unsafe { (*node).next };
        }
    }

    let mut i = 0usize;
    while i < wake_count {
        let eq = wake_queues[i];
        if !eq.is_null() {
            unsafe { a20_event_wait_queue_wake_one(ptr::addr_of_mut!((*eq).waiters)) };
        }
        i += 1;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_eventq_on_object_destroy(object: *mut c_void) {
    unsafe { evq_hash_init() };

    loop {
        let mut node = ptr::null_mut::<ObjWatchNode>();
        let mut watch = ptr::null_mut::<a20_watch_entry_t>();
        let mut eq = ptr::null_mut::<a20_eventq_t>();

        {
            let _hash_guard = unsafe { irqsave_lock(ptr::addr_of_mut!(G_EVQ_HASH_LOCK)) };
            let mut link = unsafe { ptr::addr_of_mut!(G_EVQ_HASH[evq_hash_ptr(object)]) };
            loop {
                let cur = unsafe { *link };
                if cur.is_null() {
                    break;
                }
                if unsafe { (*cur).object == object } {
                    let we = unsafe { (*cur).entry };
                    node = cur;
                    watch = we;
                    eq = if we.is_null() {
                        ptr::null_mut()
                    } else {
                        unsafe { (*we).owner_queue }
                    };
                    unsafe {
                        *link = (*cur).next;
                    }
                    break;
                }
                link = unsafe { ptr::addr_of_mut!((*cur).next) };
            }
        }

        if node.is_null() {
            break;
        }

        if !eq.is_null() && !watch.is_null() {
            {
                let _eq_guard = unsafe { irqsave_lock(ptr::addr_of_mut!((*eq).lock)) };
                let mut wlink = unsafe { ptr::addr_of_mut!((*eq).watches) };
                loop {
                    let cur = unsafe { *wlink };
                    if cur.is_null() {
                        break;
                    }
                    if cur == watch {
                        unsafe {
                            *wlink = (*cur).next;
                            (*eq).watch_count -= 1;
                        }
                        break;
                    }
                    wlink = unsafe { ptr::addr_of_mut!((*cur).next) };
                }
            }
            unsafe { a20_event_kfree(watch.cast()) };
        }

        unsafe { a20_event_kfree(node.cast()) };
    }
}
