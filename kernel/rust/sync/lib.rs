//! Rust implementation of A20OS wait queues, mutexes, and completions.
//!
//! Drop-in replacement for `kernel/core/sync.c`.  Keeps the C structs and
//! public ABI while replacing the manual linked-list manipulation under
//! irqsave spinlocks with RAII-protected critical sections.

#![no_std]
#![warn(rust_2018_idioms)]

mod ffi;

use core::ffi::{c_int, c_void};
use core::ptr;

use a20rust_support::lock::{raw_irqsave_lock, spinlock_t};

use ffi::{a20_proc_current, a20_proc_make_ready, a20_sched, a20_task_set_state, a20_task_state};

const PROC_BLOCKED: c_int = 3;
const COMPLETION_DONE_ALL: u32 = u32::MAX;

/// C-compatible waiter entry.  Must match `wait_queue_entry_t` in
/// `kernel/include/core/sync.h`.
#[repr(C)]
pub struct wait_queue_entry_t {
    next: *mut wait_queue_entry_t,
    prev: *mut wait_queue_entry_t,
    task: *mut c_void,
}

/// C-compatible wait queue.  Must match `wait_queue_t` in
/// `kernel/include/core/sync.h`.
#[repr(C)]
pub struct wait_queue_t {
    lock: spinlock_t,
    head: *mut wait_queue_entry_t,
}

/// C-compatible mutex.  Must match `mutex_t` in `kernel/include/core/sync.h`.
#[repr(C)]
pub struct mutex_t {
    lock: spinlock_t,
    locked: c_int,
    owner: *mut c_void,
    waiters: wait_queue_t,
}

/// C-compatible completion.  Must match `completion_t` in
/// `kernel/include/core/sync.h`.
#[repr(C)]
pub struct completion_t {
    lock: spinlock_t,
    done: u32,
    waiters: wait_queue_t,
}

#[no_mangle]
pub unsafe extern "C" fn wait_queue_init(q: *mut wait_queue_t) {
    if q.is_null() {
        return;
    }
    unsafe {
        (*q).lock.locked = 0;
        (*q).head = ptr::null_mut();
    }
}

#[no_mangle]
pub unsafe extern "C" fn wait_queue_prepare(q: *mut wait_queue_t, entry: *mut wait_queue_entry_t) {
    let cur = a20_proc_current();
    if q.is_null() || entry.is_null() || cur.is_null() {
        return;
    }
    unsafe {
        (*entry).task = cur;
        let _g = raw_irqsave_lock(ptr::addr_of_mut!((*q).lock));
        let mut e = (*q).head;
        while !e.is_null() {
            if e == entry || (*e).task == cur {
                return;
            }
            e = (*e).next;
        }
        (*entry).next = (*q).head;
        (*entry).prev = ptr::null_mut();
        if !(*q).head.is_null() {
            (*(*q).head).prev = entry;
        }
        (*q).head = entry;
    }
}

#[no_mangle]
pub unsafe extern "C" fn wait_queue_finish(q: *mut wait_queue_t, entry: *mut wait_queue_entry_t) {
    if q.is_null() || entry.is_null() {
        return;
    }
    unsafe {
        let _g = raw_irqsave_lock(ptr::addr_of_mut!((*q).lock));
        if !(*entry).prev.is_null() {
            (*(*entry).prev).next = (*entry).next;
        } else if (*q).head == entry {
            (*q).head = (*entry).next;
        }
        if !(*entry).next.is_null() {
            (*(*entry).next).prev = (*entry).prev;
        }
        (*entry).next = ptr::null_mut();
        (*entry).prev = ptr::null_mut();
        (*entry).task = ptr::null_mut();
    }
}

#[no_mangle]
pub unsafe extern "C" fn wait_queue_sleep(q: *mut wait_queue_t) {
    let cur = a20_proc_current();
    if q.is_null() || cur.is_null() {
        return;
    }

    let mut entry: wait_queue_entry_t = unsafe { core::mem::zeroed() };
    let mut duplicate = false;
    unsafe {
        entry.task = cur;
        let entry_ptr = ptr::addr_of_mut!(entry);

        let _g = raw_irqsave_lock(ptr::addr_of_mut!((*q).lock));
        let mut e = (*q).head;
        while !e.is_null() {
            if (*e).task == cur {
                a20_task_set_state(cur, PROC_BLOCKED);
                duplicate = true;
                break;
            }
            e = (*e).next;
        }
        if !duplicate {
            (*entry_ptr).next = (*q).head;
            (*entry_ptr).prev = ptr::null_mut();
            if !(*q).head.is_null() {
                (*(*q).head).prev = entry_ptr;
            }
            (*q).head = entry_ptr;
            a20_task_set_state(cur, PROC_BLOCKED);
        }
    }

    a20_sched();
    if !duplicate {
        unsafe { wait_queue_finish(q, ptr::addr_of_mut!(entry)) };
    }
}

#[no_mangle]
pub unsafe extern "C" fn wait_queue_wake_one(q: *mut wait_queue_t) {
    if q.is_null() {
        return;
    }
    let entry = unsafe {
        let _g = raw_irqsave_lock(ptr::addr_of_mut!((*q).lock));
        let e = (*q).head;
        if !e.is_null() {
            (*q).head = (*e).next;
            if !(*q).head.is_null() {
                (*(*q).head).prev = ptr::null_mut();
            }
            (*e).next = ptr::null_mut();
            (*e).prev = ptr::null_mut();
        }
        e
    };

    if !entry.is_null() {
        let t = unsafe { (*entry).task };
        unsafe { (*entry).task = ptr::null_mut() };
        if !t.is_null() && a20_task_state(t) == PROC_BLOCKED {
            a20_proc_make_ready(t);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn wait_queue_wake_all(q: *mut wait_queue_t) {
    if q.is_null() {
        return;
    }
    let mut list = unsafe {
        let _g = raw_irqsave_lock(ptr::addr_of_mut!((*q).lock));
        let head = (*q).head;
        (*q).head = ptr::null_mut();
        head
    };

    while !list.is_null() {
        unsafe {
            let t = (*list).task;
            let next = (*list).next;
            (*list).next = ptr::null_mut();
            (*list).prev = ptr::null_mut();
            (*list).task = ptr::null_mut();
            if !t.is_null() && a20_task_state(t) == PROC_BLOCKED {
                a20_proc_make_ready(t);
            }
            list = next;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn mutex_init(m: *mut mutex_t) {
    if m.is_null() {
        return;
    }
    unsafe {
        (*m).lock.locked = 0;
        (*m).locked = 0;
        (*m).owner = ptr::null_mut();
        wait_queue_init(ptr::addr_of_mut!((*m).waiters));
    }
}

#[no_mangle]
pub unsafe extern "C" fn mutex_trylock(m: *mut mutex_t) -> c_int {
    if m.is_null() {
        return 0;
    }
    let cur = a20_proc_current();
    unsafe {
        let _g = raw_irqsave_lock(ptr::addr_of_mut!((*m).lock));
        if (*m).locked == 0 {
            (*m).locked = 1;
            (*m).owner = cur;
            return 1;
        }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn mutex_lock(m: *mut mutex_t) {
    if m.is_null() {
        return;
    }
    let cur = a20_proc_current();
    if cur.is_null() {
        while mutex_trylock(m) == 0 {
            a20_sched();
        }
        return;
    }

    let mut entry: wait_queue_entry_t = unsafe { core::mem::zeroed() };
    loop {
        unsafe {
            let _g = raw_irqsave_lock(ptr::addr_of_mut!((*m).lock));
            if (*m).locked == 0 {
                (*m).locked = 1;
                (*m).owner = cur;
                return;
            }

            entry.task = cur;
            let entry_ptr = ptr::addr_of_mut!(entry);

            {
                let _wg = raw_irqsave_lock(ptr::addr_of_mut!((*m).waiters.lock));
                let mut e = (*m).waiters.head;
                while !e.is_null() {
                    if (*e).task == cur {
                        a20_task_set_state(cur, PROC_BLOCKED);
                        break;
                    }
                    e = (*e).next;
                }
                if e.is_null() {
                    (*entry_ptr).next = (*m).waiters.head;
                    (*entry_ptr).prev = ptr::null_mut();
                    if !(*m).waiters.head.is_null() {
                        (*(*m).waiters.head).prev = entry_ptr;
                    }
                    (*m).waiters.head = entry_ptr;
                    a20_task_set_state(cur, PROC_BLOCKED);
                }
            }
        }

        a20_sched();
        unsafe { wait_queue_finish(ptr::addr_of_mut!((*m).waiters), ptr::addr_of_mut!(entry)) };
        entry.next = ptr::null_mut();
        entry.prev = ptr::null_mut();
        entry.task = ptr::null_mut();
    }
}

#[no_mangle]
pub unsafe extern "C" fn mutex_unlock(m: *mut mutex_t) {
    if m.is_null() {
        return;
    }
    unsafe {
        let _g = raw_irqsave_lock(ptr::addr_of_mut!((*m).lock));
        (*m).locked = 0;
        (*m).owner = ptr::null_mut();
    }
    unsafe { wait_queue_wake_one(ptr::addr_of_mut!((*m).waiters)) };
}

#[no_mangle]
pub unsafe extern "C" fn completion_init(c: *mut completion_t) {
    if c.is_null() {
        return;
    }
    unsafe {
        (*c).lock.locked = 0;
        (*c).done = 0;
        wait_queue_init(ptr::addr_of_mut!((*c).waiters));
    }
}

#[no_mangle]
pub unsafe extern "C" fn complete(c: *mut completion_t) {
    if c.is_null() {
        return;
    }
    unsafe {
        let _g = raw_irqsave_lock(ptr::addr_of_mut!((*c).lock));
        (*c).done = (*c).done.wrapping_add(1);
    }
    unsafe { wait_queue_wake_one(ptr::addr_of_mut!((*c).waiters)) };
}

#[no_mangle]
pub unsafe extern "C" fn complete_all(c: *mut completion_t) {
    if c.is_null() {
        return;
    }
    unsafe {
        let _g = raw_irqsave_lock(ptr::addr_of_mut!((*c).lock));
        (*c).done = COMPLETION_DONE_ALL;
    }
    unsafe { wait_queue_wake_all(ptr::addr_of_mut!((*c).waiters)) };
}

#[no_mangle]
pub unsafe extern "C" fn wait_for_completion(c: *mut completion_t) {
    if c.is_null() {
        return;
    }
    loop {
        unsafe {
            let _g = raw_irqsave_lock(ptr::addr_of_mut!((*c).lock));
            if (*c).done != 0 {
                if (*c).done != COMPLETION_DONE_ALL {
                    (*c).done -= 1;
                }
                return;
            }
        }
        unsafe { wait_queue_sleep(ptr::addr_of_mut!((*c).waiters)) };
    }
}
