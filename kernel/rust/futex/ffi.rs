use core::ffi::{c_int, c_long, c_void};

#[repr(C)]
pub struct task_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct mm_struct_t {
    _opaque: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct robust_list {
    pub next: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct robust_list_head {
    pub list: robust_list,
    pub futex_offset: u64,
    pub list_op_pending: *mut robust_list,
}

unsafe extern "C" {
    pub fn copy_from_user(dst: *mut c_void, src: *const c_void, n: usize) -> c_long;
    pub fn copy_to_user(dst: *mut c_void, src: *const c_void, n: usize) -> c_long;

    pub fn proc_current() -> *mut task_t;
    pub fn proc_make_ready(task: *mut task_t);
    pub fn proc_set_wake_time(task: *mut task_t, wake_time: u64);
    pub fn sched();
    pub fn signal_task_has_unblocked(task: *mut task_t) -> c_int;

    pub fn a20_futex_task_state(task: *mut task_t) -> c_int;
    pub fn a20_futex_task_mm(task: *mut task_t) -> *mut mm_struct_t;
    pub fn a20_futex_task_pid(task: *mut task_t) -> c_int;
    pub fn a20_futex_task_robust_list_head(task: *mut task_t) -> usize;
    pub fn a20_futex_task_clear_robust_list_head(task: *mut task_t);
    pub fn a20_futex_phys_key(uaddr: *mut c_int) -> usize;
    pub fn a20_futex_timeout_ticks(
        timeout: *mut c_void,
        absolute: c_int,
        realtime: c_int,
        ticks_out: *mut u64,
    ) -> c_int;
    pub fn a20_futex_get_monotonic(ts: *mut u64);
    pub fn a20_futex_get_realtime(ts: *mut u64);
    pub fn a20_futex_get_ticks() -> u64;
    pub fn a20_futex_set_blocked(task: *mut task_t);
}

pub const FUTEX_WAIT: c_int = 0;
pub const FUTEX_WAKE: c_int = 1;
pub const FUTEX_REQUEUE: c_int = 3;
pub const FUTEX_CMP_REQUEUE: c_int = 4;
pub const FUTEX_WAKE_OP: c_int = 5;
pub const FUTEX_WAIT_BITSET: c_int = 9;
pub const FUTEX_WAKE_BITSET: c_int = 10;
pub const FUTEX_CMD_MASK: c_int = 0x7f;
pub const FUTEX_CLOCK_REALTIME: c_int = 0x100;
pub const FUTEX_BITSET_MATCH_ANY: u32 = 0xffff_ffff;

pub const FUTEX_OP_SET: c_int = 0;
pub const FUTEX_OP_ADD: c_int = 1;
pub const FUTEX_OP_OR: c_int = 2;
pub const FUTEX_OP_ANDN: c_int = 3;
pub const FUTEX_OP_XOR: c_int = 4;
pub const FUTEX_OP_OPARG_SHIFT: c_int = 8;
pub const FUTEX_OP_CMP_EQ: c_int = 0;
pub const FUTEX_OP_CMP_NE: c_int = 1;
pub const FUTEX_OP_CMP_LT: c_int = 2;
pub const FUTEX_OP_CMP_LE: c_int = 3;
pub const FUTEX_OP_CMP_GT: c_int = 4;
pub const FUTEX_OP_CMP_GE: c_int = 5;

pub const FUTEX_WAITERS: u32 = 0x8000_0000;
pub const FUTEX_OWNER_DIED: u32 = 0x4000_0000;
pub const FUTEX_TID_MASK: u32 = 0x3fff_ffff;
pub const ROBUST_LIST_LIMIT: usize = 2048;

pub const PROC_UNUSED: c_int = 0;
pub const PROC_ZOMBIE: c_int = 5;
pub const PROC_BLOCKED: c_int = 3;

pub const EAGAIN: c_int = 11;
pub const ENOMEM: c_int = 12;
pub const EFAULT: c_int = 14;
pub const EBUSY: c_int = 16;
pub const EINVAL: c_int = 22;
pub const ENOSYS: c_int = 38;
pub const ETIMEDOUT: c_int = 110;
pub const ERESTARTSYS: c_int = 512;
pub const ESRCH: c_int = 3;
