#![allow(non_camel_case_types)]

use core::ffi::c_void;

pub use a20rust_support::lock::spinlock_t;
pub use a20rust_support::vfs::{wait_queue_entry_t, wait_queue_t};

pub const IPC_CREAT: i32 = 0o1000;
pub const IPC_EXCL: i32 = 0o2000;
pub const IPC_NOWAIT: i32 = 0o4000;
pub const IPC_64_BIT: i32 = 0x100;
pub const IPC_RMID: i32 = 0;
pub const IPC_STAT: i32 = 2;

pub const GETPID: i32 = 11;
pub const GETVAL: i32 = 12;
pub const GETALL: i32 = 13;
pub const GETNCNT: i32 = 14;
pub const GETZCNT: i32 = 15;
pub const SETVAL: i32 = 16;
pub const SETALL: i32 = 17;
pub const SEM_STAT: i32 = 18;
pub const SEM_INFO: i32 = 19;
pub const SEM_STAT_ANY: i32 = 20;

pub const SYSV_SEM_MAX: usize = 32;
pub const SYSV_SEM_PER_SET: usize = 64;
pub const SYSV_SEM_OPS_MAX: usize = 64;

pub const EINVAL: i32 = 22;
pub const EEXIST: i32 = 17;
pub const ENOENT: i32 = 2;
pub const ENOSPC: i32 = 28;
pub const EFAULT: i32 = 14;
pub const EFBIG: i32 = 27;
pub const EAGAIN: i32 = 11;
pub const EINTR: i32 = 4;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct sysv_sembuf_t {
    pub sem_num: u16,
    pub sem_op: i16,
    pub sem_flg: i16,
}

#[repr(C)]
pub struct task_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct sysv_sem_set_t {
    pub used: i32,
    pub key: i32,
    pub nsems: i32,
    pub val: [u16; SYSV_SEM_PER_SET],
    pub last_pid: i32,
    pub waiters: wait_queue_t,
}

unsafe extern "C" {
    pub fn sysv_sem_wait_queue_init(q: *mut wait_queue_t);
    pub fn sysv_sem_wait_queue_wake_all(q: *mut wait_queue_t);
    pub fn sysv_sem_wait_queue_finish(q: *mut wait_queue_t, entry: *mut wait_queue_entry_t);
    pub fn sysv_sem_copy_to_user(dst: *mut c_void, src: *const c_void, n: usize) -> isize;
    pub fn sysv_sem_copy_from_user(dst: *mut c_void, src: *const c_void, n: usize) -> isize;
    pub fn sysv_sem_proc_current() -> *mut task_t;
    pub fn sysv_sem_proc_pid(task: *mut task_t) -> i32;
    pub fn sysv_sem_signal_task_has_unblocked(task: *mut task_t) -> i32;
    pub fn sysv_sem_timer_get_ticks() -> u64;
    pub fn sysv_sem_proc_set_wake_time(task: *mut task_t, wake_time: u64);
    pub fn sysv_sem_sched();
    pub fn sysv_sem_task_set_blocked(task: *mut task_t);
}
