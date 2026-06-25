use core::ffi::{c_int, c_void};

#[repr(C)]
pub struct vfile_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct task_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct wait_queue_entry_t {
    pub next: *mut wait_queue_entry_t,
    pub prev: *mut wait_queue_entry_t,
    pub task: *mut c_void,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct spinlock_t {
    pub locked: i32,
}

#[repr(C)]
pub struct wait_queue_t {
    pub lock: spinlock_t,
    pub head: *mut wait_queue_entry_t,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct fs_flock_t {
    pub l_type: i16,
    pub l_whence: i16,
    pub l_start: i64,
    pub l_len: i64,
    pub l_pid: c_int,
}

unsafe extern "C" {
    pub fn wait_queue_init(q: *mut wait_queue_t);
    pub fn wait_queue_prepare(q: *mut wait_queue_t, entry: *mut wait_queue_entry_t);
    pub fn wait_queue_finish(q: *mut wait_queue_t, entry: *mut wait_queue_entry_t);
    pub fn wait_queue_wake_all(q: *mut wait_queue_t);

    pub fn sched();
    pub fn proc_current() -> *mut task_t;
    pub fn proc_yield();
    pub fn signal_task_has_unblocked(task: *mut task_t) -> c_int;

    pub fn a20_locks_file_key(vf: *mut vfile_t) -> usize;
    pub fn a20_locks_file_size(vf: *mut vfile_t) -> i64;
    pub fn a20_locks_file_offset(vf: *mut vfile_t) -> i64;
    pub fn a20_locks_current_pid() -> c_int;
}

pub const FS_LOCK_OWNER_PID: c_int = 1;
pub const FS_LOCK_OWNER_OFD: c_int = 2;

pub const SEEK_SET: c_int = 0;
pub const SEEK_CUR: c_int = 1;
pub const SEEK_END: c_int = 2;

pub const LOCK_SH: c_int = 1;
pub const LOCK_EX: c_int = 2;
pub const LOCK_NB: c_int = 4;
pub const LOCK_UN: c_int = 8;

pub const F_RDLCK: i16 = 0;
pub const F_WRLCK: i16 = 1;
pub const F_UNLCK: i16 = 2;

pub const EBADF: c_int = 9;
pub const EAGAIN: c_int = 11;
pub const EINVAL: c_int = 22;
pub const EDEADLK: c_int = 35;
pub const ENOLCK: c_int = 37;
pub const ERESTARTSYS: c_int = 512;
