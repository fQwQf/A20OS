use core::ffi::{c_int, c_void};

extern "C" {
    pub fn a20_proc_current() -> *mut c_void;
    pub fn a20_proc_make_ready(task: *mut c_void);
    pub fn a20_sched();
    pub fn a20_task_state(task: *mut c_void) -> c_int;
    pub fn a20_task_set_state(task: *mut c_void, state: c_int);
}
