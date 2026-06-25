use a20rust_support::lock::spinlock_t;
use core::ffi::{c_int, c_void};

#[repr(C)]
pub struct Task {
    _opaque: [u8; 0],
}

unsafe extern "C" {
    pub static mut proc_lock: spinlock_t;

    pub fn proc_current() -> *mut Task;
    pub fn proc_set_current(next: *mut Task) -> *mut Task;
    pub fn proc_idle_task() -> *mut Task;
    pub fn proc_first_task_locked() -> *mut Task;
    pub fn proc_next_task_locked(task: *mut Task) -> *mut Task;
    pub fn proc_unlink_task_locked(task: *mut Task);
    pub fn proc_destroy_task(task: *mut Task);
    pub fn proc_make_ready(task: *mut Task);

    pub fn timer_get_ticks() -> u64;
    pub fn timer_set_interval(interval: u64);

    pub fn kernel_progress_run_bottom_halves();
    pub fn signal_send(pid: c_int, signum: c_int) -> c_int;

    pub fn cg_cpu_account(cg: *mut c_void, elapsed_ns: u64, now: u64) -> c_int;
    pub fn cg_cpu_check_unthrottle(cg: *mut c_void, now: u64);

    pub fn a20_sched_ticks_per_sec() -> u64;
    pub fn a20_sched_nr_cpus() -> u32;
    pub fn a20_sched_current_cpu_id() -> u32;
    pub fn a20_sched_send_reschedule(cpu: u32);
    pub fn a20_sched_posix_timer_tick();
    pub fn a20_sched_native_timer_tick();
    pub fn a20_sched_low_level_switch(old: *mut Task, next_kstack: u64);
    pub fn a20_sched_task_cpu_mask(task: *mut Task) -> u32;
    pub fn a20_sched_task_sched_policy(task: *mut Task) -> c_int;
    pub fn a20_sched_task_sched_level(task: *mut Task) -> c_int;
    pub fn a20_sched_task_set_sched_level(task: *mut Task, level: c_int);
    pub fn a20_sched_task_priority(task: *mut Task) -> c_int;
    pub fn a20_sched_task_state(task: *mut Task) -> c_int;
    pub fn a20_sched_task_set_state(task: *mut Task, state: c_int);
    pub fn a20_sched_task_cpu_id(task: *mut Task) -> u32;
    pub fn a20_sched_task_set_cpu_id(task: *mut Task, cpu: u32);
    pub fn a20_sched_task_on_rq(task: *mut Task) -> c_int;
    pub fn a20_sched_task_set_on_rq(task: *mut Task, on_rq: c_int);
    pub fn a20_sched_task_ready_since(task: *mut Task) -> u64;
    pub fn a20_sched_task_set_ready_since(task: *mut Task, ready_since: u64);
    pub fn a20_sched_task_exec_start(task: *mut Task) -> u64;
    pub fn a20_sched_task_set_exec_start(task: *mut Task, exec_start: u64);
    pub fn a20_sched_task_wake_time(task: *mut Task) -> u64;
    pub fn a20_sched_task_set_wake_time(task: *mut Task, wake_time: u64);
    pub fn a20_sched_task_alarm_expire(task: *mut Task) -> u64;
    pub fn a20_sched_task_set_alarm_expire(task: *mut Task, alarm_expire: u64);
    pub fn a20_sched_task_itimer_real_interval(task: *mut Task) -> u64;
    pub fn a20_sched_task_kstack(task: *mut Task) -> u64;
    pub fn a20_sched_task_cg_throttled(task: *mut Task) -> c_int;
    pub fn a20_sched_task_set_cg_throttled(task: *mut Task, throttled: c_int);
    pub fn a20_sched_task_cgroup(task: *mut Task) -> *mut c_void;
    pub fn a20_sched_task_cg_cpu_start(task: *mut Task) -> u64;
    pub fn a20_sched_task_set_cg_cpu_start(task: *mut Task, start: u64);
    pub fn a20_sched_task_pid(task: *mut Task) -> c_int;
    pub fn a20_sched_task_ppid(task: *mut Task) -> c_int;
    pub fn a20_sched_task_clone_flags(task: *mut Task) -> c_int;
    pub fn a20_sched_task_rq_next(task: *mut Task) -> *mut Task;
    pub fn a20_sched_task_set_rq_next(task: *mut Task, next: *mut Task);
    pub fn a20_sched_task_rq_prev(task: *mut Task) -> *mut Task;
    pub fn a20_sched_task_set_rq_prev(task: *mut Task, prev: *mut Task);
    pub fn a20_sched_task_should_reap_zombie(task: *mut Task) -> c_int;
    pub fn a20_sched_trace_ctxsw(prev_pid: c_int, next_pid: c_int);
    pub fn a20_sched_trace_fall_to_idle(cur_pid: c_int, state: c_int);
    pub fn a20_sched_trace_yield(pid: c_int);
}

pub const SCHED_LEVELS: usize = 8;
pub const SCHED_NO_DEADLINE: u64 = !0u64;

pub const SCHED_NORMAL: c_int = 0;
pub const SCHED_FIFO: c_int = 1;
pub const SCHED_RR: c_int = 2;

pub const PROC_UNUSED: c_int = 0;
pub const PROC_READY: c_int = 1;
pub const PROC_RUNNING: c_int = 2;
pub const PROC_BLOCKED: c_int = 3;
pub const PROC_ZOMBIE: c_int = 5;

pub const CLONE_THREAD: c_int = 0x0001_0000;
pub const SIGALRM: c_int = 14;
