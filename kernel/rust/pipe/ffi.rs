use core::ffi::{c_char, c_int, c_long, c_ulong, c_void};

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
pub struct vfile_ops_t {
    pub read: Option<extern "C" fn(*mut vfile_t, *mut c_char, usize) -> c_int>,
    pub write: Option<extern "C" fn(*mut vfile_t, *const c_char, usize) -> c_int>,
    pub lseek: Option<extern "C" fn(*mut vfile_t, c_long, c_int) -> c_long>,
    pub readdir: Option<extern "C" fn(*mut vfile_t, *mut c_void, usize) -> c_int>,
    pub ioctl: Option<extern "C" fn(*mut vfile_t, c_ulong, *mut c_void) -> c_int>,
    pub close: Option<extern "C" fn(*mut vfile_t) -> c_int>,
}

unsafe extern "C" {
    pub fn kmalloc(size: usize) -> *mut c_void;
    pub fn kfree(ptr: *mut c_void);

    pub fn wait_queue_init(q: *mut wait_queue_t);
    pub fn wait_queue_prepare(q: *mut wait_queue_t, entry: *mut wait_queue_entry_t);
    pub fn wait_queue_finish(q: *mut wait_queue_t, entry: *mut wait_queue_entry_t);
    pub fn wait_queue_wake_all(q: *mut wait_queue_t);

    pub fn proc_current() -> *mut task_t;
    pub fn proc_yield();
    pub fn sched();
    pub fn signal_task_has_unblocked(task: *mut task_t) -> c_int;
    pub fn signal_send(pid: c_int, signum: c_int) -> c_int;

    pub fn vfile_alloc() -> *mut vfile_t;
    pub fn vfile_free(vf: *mut vfile_t);
    pub fn vfile_ref_read(vf: *mut vfile_t) -> c_int;
    pub fn vfs_alloc_fd(vf: *mut vfile_t) -> c_int;
    pub fn vfs_close(fd: c_int) -> c_int;

    pub fn a20_pipe_vfile_priv(vf: *mut vfile_t) -> *mut c_void;
    pub fn a20_pipe_vfile_flags(vf: *mut vfile_t) -> c_int;
    pub fn a20_pipe_vfile_init(
        vf: *mut vfile_t,
        ops: *mut vfile_ops_t,
        priv_data: *mut c_void,
        flags: c_int,
    );
    pub fn a20_pipe_vfile_ops_eq(vf: *mut vfile_t, ops: *mut vfile_ops_t) -> c_int;
    pub fn a20_pipe_task_pid(task: *mut task_t) -> c_int;
    pub fn a20_pipe_task_set_blocked(task: *mut task_t);
}

pub const O_RDONLY: c_int = 0;
pub const O_WRONLY: c_int = 1;
pub const O_NONBLOCK: c_int = 0x800;

pub const POLLIN: i16 = 0x001;
pub const POLLOUT: i16 = 0x004;
pub const POLLERR: i16 = 0x008;
pub const POLLHUP: i16 = 0x010;
pub const POLLNVAL: i16 = 0x020;

pub const SIGPIPE: c_int = 13;

pub const PIPE_BUF_SIZE: usize = 4096;

pub const EBADF: c_int = 9;
pub const EAGAIN: c_int = 11;
pub const ENOMEM: c_int = 12;
pub const EBUSY: c_int = 16;
pub const EINVAL: c_int = 22;
pub const EMFILE: c_int = 24;
pub const EPIPE: c_int = 32;
pub const ERESTARTSYS: c_int = 512;
