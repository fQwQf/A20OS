use core::ffi::c_int;
use core::ffi::c_void;

pub type TaskT = c_void;

extern "C" {
    pub fn a20_task_all_next(t: *mut TaskT) -> *mut TaskT;
    pub fn a20_task_set_all_next(t: *mut TaskT, n: *mut TaskT);
    pub fn a20_task_all_prev(t: *mut TaskT) -> *mut TaskT;
    pub fn a20_task_set_all_prev(t: *mut TaskT, p: *mut TaskT);
    pub fn a20_task_pid(t: *mut TaskT) -> c_int;
    pub fn a20_arch_is_kernel_address(addr: usize) -> c_int;
    pub fn a20_proc_list_corrupt(pid: c_int, ptr: *mut TaskT);
}
