use core::ffi::{c_char, c_int, c_long, c_ulong, c_void};

#[repr(C)]
pub struct vfile_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct vfile_ops_t {
    pub read: Option<extern "C" fn(*mut vfile_t, *mut c_char, usize) -> c_int>,
    pub write: Option<extern "C" fn(*mut vfile_t, *const c_char, usize) -> c_int>,
    pub lseek: Option<extern "C" fn(*mut vfile_t, c_long, c_int) -> c_long>,
    pub readdir: Option<extern "C" fn(*mut vfile_t, *mut c_void, usize) -> c_int>,
    pub ioctl: Option<extern "C" fn(*mut vfile_t, c_ulong, *mut c_void) -> c_int>,
    pub close: Option<extern "C" fn(*mut vfile_t) -> c_int>,
}

#[repr(C)]
pub struct wait_queue_entry_t {
    pub next: *mut wait_queue_entry_t,
    pub prev: *mut wait_queue_entry_t,
    pub task: *mut c_void,
}

#[repr(C)]
pub struct wait_queue_t {
    pub lock: spinlock_t,
    pub head: *mut wait_queue_entry_t,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct spinlock_t {
    pub locked: i32,
}

unsafe extern "C" {
    pub fn kmalloc(size: usize) -> *mut c_void;
    pub fn kfree(ptr: *mut c_void);

    pub fn wait_queue_init(q: *mut wait_queue_t);
    pub fn wait_queue_sleep(q: *mut wait_queue_t);
    pub fn wait_queue_wake_all(q: *mut wait_queue_t);

    pub fn timer_get_ticks() -> u64;

    pub fn proc_current() -> *mut c_void;
    pub fn proc_set_wake_time(t: *mut c_void, wake_time: u64);

    pub fn vfs_get_file_ref(fd: c_int) -> *mut vfile_t;
    pub fn vfs_put_file_ref(fd: c_int, vf: *mut vfile_t);

    pub fn a20_arch_timer_freq() -> u64;

    pub fn a20_timerfd_vfile_alloc() -> *mut vfile_t;
    pub fn a20_timerfd_vfile_free(vf: *mut vfile_t);
    pub fn a20_timerfd_vfile_priv(vf: *mut vfile_t) -> *mut c_void;
    pub fn a20_timerfd_vfile_init(vf: *mut vfile_t, ops: *mut vfile_ops_t, data: *mut c_void, flags: c_int);
    pub fn a20_timerfd_install_vfile(vf: *mut vfile_t, flags: c_int) -> c_int;
    pub fn a20_timerfd_vfile_ops_match(vf: *mut vfile_t, ops: *mut vfile_ops_t) -> c_int;
}

extern "C" {
    pub fn anonfd_free_priv_close(vf: *mut vfile_t) -> c_int;
}

pub const O_NONBLOCK: c_int = 0x800;
pub const O_CLOEXEC: c_int = 0x80000;
pub const O_RDONLY: c_int = 0;

pub const TFD_TIMER_ABSTIME: c_int = 1;

pub const EBADF: c_int = 9;
pub const EAGAIN: c_int = 11;
pub const ENOMEM: c_int = 12;
pub const EINVAL: c_int = 22;
