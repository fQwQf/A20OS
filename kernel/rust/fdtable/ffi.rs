use core::ffi::{c_int, c_void};

#[repr(C)]
pub struct task_t {
    _opaque: [u8; 0],
}

pub const MAX_FILES: usize = 1024;
pub const FDTABLE_WORDS: usize = (MAX_FILES + 63) / 64;

unsafe extern "C" {
    pub fn kmalloc(size: usize) -> *mut c_void;
    pub fn kfree(ptr: *mut c_void);

    pub fn proc_current() -> *mut task_t;

    pub fn vfs_ref_fd(fd: c_int) -> c_int;
    pub fn vfs_close(fd: c_int) -> c_int;

    pub fn a20_fdtable_get_files(task: *mut task_t) -> *mut c_void;
    pub fn a20_fdtable_set_files(task: *mut task_t, files: *mut c_void);
    pub fn a20_fdtable_fd_limit(task: *mut task_t) -> c_int;
    pub fn a20_fdtable_panic_oom();
}

pub const O_CLOEXEC: c_int = 0x80000;

pub const EBADF: c_int = 9;
pub const ESRCH: c_int = 3;
pub const EINVAL: c_int = 22;
pub const EMFILE: c_int = 24;
pub const ENOMEM: c_int = 12;

pub const FD_CLOEXEC: c_int = 1;
