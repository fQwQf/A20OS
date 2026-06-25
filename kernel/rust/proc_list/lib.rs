#![no_std]
#![warn(rust_2018_idioms)]
#![allow(static_mut_refs)]

mod ffi;

use core::ptr;

use ffi::TaskT;

static mut LIST_HEAD: *mut TaskT = ptr::null_mut();
static mut LIST_TAIL: *mut TaskT = ptr::null_mut();

fn is_aligned(p: *mut TaskT) -> bool {
    let mask = core::mem::size_of::<*mut TaskT>() - 1;
    (p as usize) & mask == 0
}

#[no_mangle]
pub unsafe extern "C" fn proc_link_task_locked(t: *mut TaskT) {
    if t.is_null() {
        return;
    }
    let tail = LIST_TAIL;
    ffi::a20_task_set_all_prev(t, tail);
    ffi::a20_task_set_all_next(t, ptr::null_mut());
    if !tail.is_null() {
        ffi::a20_task_set_all_next(tail, t);
    } else {
        LIST_HEAD = t;
    }
    LIST_TAIL = t;
}

#[no_mangle]
pub unsafe extern "C" fn proc_unlink_task_locked(t: *mut TaskT) {
    if t.is_null() {
        return;
    }
    let prev = ffi::a20_task_all_prev(t);
    let next = ffi::a20_task_all_next(t);
    if !prev.is_null() {
        ffi::a20_task_set_all_next(prev, next);
    } else if LIST_HEAD == t {
        LIST_HEAD = next;
    }
    if !next.is_null() {
        ffi::a20_task_set_all_prev(next, prev);
    } else if LIST_TAIL == t {
        LIST_TAIL = prev;
    }
    ffi::a20_task_set_all_next(t, ptr::null_mut());
    ffi::a20_task_set_all_prev(t, ptr::null_mut());
}

#[no_mangle]
pub unsafe extern "C" fn proc_first_task_locked() -> *mut TaskT {
    LIST_HEAD
}

#[no_mangle]
pub unsafe extern "C" fn proc_next_task_locked(t: *mut TaskT) -> *mut TaskT {
    let next = ffi::a20_task_all_next(t);
    if !next.is_null()
        && (!is_aligned(next) || ffi::a20_arch_is_kernel_address(next as usize) == 0)
    {
        let pid = if t.is_null() { -1 } else { ffi::a20_task_pid(t) };
        ffi::a20_proc_list_corrupt(pid, next);
        return ptr::null_mut();
    }
    next
}
