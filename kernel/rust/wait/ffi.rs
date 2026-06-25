use a20rust_support::lock::spinlock_t;
use core::ffi::c_int;

#[repr(C)]
pub struct Task {
    _opaque: [u8; 0],
}

unsafe extern "C" {
    pub static mut proc_lock: spinlock_t;

    pub fn proc_current() -> *mut Task;
    pub fn proc_first_task_locked() -> *mut Task;
    pub fn proc_next_task_locked(task: *mut Task) -> *mut Task;
    pub fn proc_unlink_task_locked(task: *mut Task);
    pub fn proc_destroy_task(task: *mut Task);
    pub fn sched();
    pub fn signal_task_has_unblocked(task: *mut core::ffi::c_void) -> c_int;

    pub fn a20_wait_task_pid(task: *mut Task) -> c_int;
    pub fn a20_wait_task_tgid(task: *mut Task) -> c_int;
    pub fn a20_wait_task_ppid(task: *mut Task) -> c_int;
    pub fn a20_wait_task_pgid(task: *mut Task) -> c_int;
    pub fn a20_wait_task_parent(task: *mut Task) -> *mut Task;
    pub fn a20_wait_task_state_acquire(task: *mut Task) -> c_int;
    pub fn a20_wait_task_set_state(task: *mut Task, state: c_int);
    pub fn a20_wait_task_exit_code_acquire(task: *mut Task) -> c_int;
    pub fn a20_wait_task_total_time(task: *mut Task) -> u64;
    pub fn a20_wait_task_add_child_utime(task: *mut Task, delta: u64);
    pub fn a20_wait_task_waiting_for_child(task: *mut Task) -> c_int;
    pub fn a20_wait_task_set_waiting_for_child(task: *mut Task, waiting: c_int);
    pub fn a20_wait_task_is_clone_child(task: *mut Task) -> c_int;
}

pub const WNOHANG: c_int = 1;
pub const WUNTRACED: c_int = 2;
pub const WCONTINUED: c_int = 8;
pub const __WNOTHREAD: c_int = 0x2000_0000u32 as c_int;
pub const __WALL: c_int = 0x4000_0000u32 as c_int;
pub const __WCLONE: c_int = 0x8000_0000u32 as c_int;

pub const PROC_UNUSED: c_int = 0;
pub const PROC_RUNNING: c_int = 2;
pub const PROC_BLOCKED: c_int = 3;
pub const PROC_STOPPED: c_int = 4;
pub const PROC_ZOMBIE: c_int = 5;

pub const ECHILD: c_int = 10;
pub const ERESTARTSYS: c_int = 512;
