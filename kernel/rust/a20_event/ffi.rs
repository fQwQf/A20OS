#![allow(non_camel_case_types)]

use core::ffi::{c_char, c_void};

pub use a20rust_support::vfs::{refcount_t, spinlock_t, wait_queue_t};

pub type a20_handle_t = u32;

pub const A20_OK: i64 = 0;
pub const A20_ERR_BAD_HANDLE: i64 = 5;
pub const A20_ERR_NO_MEMORY: i64 = 6;
pub const A20_ERR_FAULT: i64 = 8;
pub const A20_ERR_INVALID_ARGUMENT: i64 = 12;
pub const A20_ERR_NO_SPACE: i64 = 13;
pub const A20_ERR_WOULD_BLOCK: i64 = 18;
pub const A20_ERR_NOT_FOUND: i64 = 24;

pub const A20_EVQ_DEFAULT_CAP: u32 = 256;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct a20_pending_event_t {
    pub source: a20_handle_t,
    pub type_: u32,
    pub events: u64,
    pub user_data: u64,
    pub data0: u64,
    pub data1: u64,
    pub data2: u64,
}

#[repr(C)]
pub struct a20_watch_entry_t {
    pub target_handle: a20_handle_t,
    pub target_object: *mut c_void,
    pub target_type: u16,
    pub _pad: u16,
    pub event_mask: u64,
    pub user_data: u64,
    pub owner_queue: *mut a20_eventq_t,
    pub next: *mut a20_watch_entry_t,
}

#[repr(C)]
pub struct a20_eventq_t {
    pub refcount: refcount_t,
    pub lock: spinlock_t,
    pub waiters: wait_queue_t,
    pub watches: *mut a20_watch_entry_t,
    pub watch_count: u32,
    pub ring: *mut a20_pending_event_t,
    pub ring_cap: u32,
    pub ring_head: u32,
    pub ring_tail: u32,
    pub ring_count: u32,
}

unsafe extern "C" {
    pub fn a20_event_wait_queue_init(q: *mut wait_queue_t);
    pub fn a20_event_wait_queue_wake_all(q: *mut wait_queue_t);
    pub fn a20_event_wait_queue_wake_one(q: *mut wait_queue_t);
    pub fn a20_event_kmalloc(size: usize) -> *mut c_void;
    pub fn a20_event_kfree(ptr: *mut c_void);
    pub fn a20_event_memset(ptr: *mut c_void, value: i32, size: usize) -> *mut c_void;
    pub fn a20_event_spin_init(lock: *mut spinlock_t);
    pub fn a20_event_spin_set_debug(lock: *mut spinlock_t, name: *const c_char, container: *mut c_void);
}
