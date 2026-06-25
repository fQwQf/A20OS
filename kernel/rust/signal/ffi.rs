use core::ffi::{c_int, c_long, c_void};

#[repr(C)]
pub struct task_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct trap_context_t {
    _opaque: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct refcount_t {
    pub value: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct sigaction_t {
    pub sa_handler: usize,
    pub sa_mask: u64,
    pub sa_flags: c_int,
}

#[repr(C)]
pub struct signal_state_t {
    pub refcount: refcount_t,
    pub actions: [sigaction_t; NSIG as usize],
    pub pending: u64,
    pub pending_has_info: [u8; NSIG as usize],
    pub pending_info: [[u8; SIGNAL_INFO_SIZE as usize]; NSIG as usize],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct user_rt_sigaction_t {
    pub handler: usize,
    pub flags: u64,
    pub mask: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct user_sigset_t {
    pub bits: [u64; 1],
}

#[repr(C, align(16))]
#[derive(Clone, Copy)]
pub struct siginfo_t {
    pub si_signo: c_int,
    pub si_errno: c_int,
    pub si_code: c_int,
    pub _sifields: [c_int; 29],
}

unsafe extern "C" {
    pub fn memset(dst: *mut c_void, val: c_int, n: usize) -> *mut c_void;
    pub fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

    pub fn proc_current() -> *mut task_t;
    pub fn proc_find(pid: c_int) -> *mut task_t;
    pub fn proc_make_ready(task: *mut task_t);
    pub fn proc_force_exit(task: *mut task_t, exit_code: c_int);
    pub fn proc_exit_group(exit_code: c_int) -> !;
    pub fn sched();

    pub fn copy_to_user(dst: *mut c_void, src: *const c_void, n: usize) -> c_long;
    pub fn copy_from_user(dst: *mut c_void, src: *const c_void, n: usize) -> c_long;

    pub fn a20_signal_task_signals(task: *mut task_t) -> *mut signal_state_t;
    pub fn a20_signal_task_has_pgdir(task: *mut task_t) -> c_int;
    pub fn a20_signal_task_pid(task: *mut task_t) -> c_int;
    pub fn a20_signal_task_uid(task: *mut task_t) -> c_int;
    pub fn a20_signal_task_state(task: *mut task_t) -> c_int;
    pub fn a20_signal_task_set_state(task: *mut task_t, state: c_int);
    pub fn a20_signal_task_set_exit_code(task: *mut task_t, code: c_int);
    pub fn a20_signal_task_exit_pending(task: *mut task_t) -> c_int;
    pub fn a20_signal_task_sig_blocked(task: *mut task_t) -> u64;
    pub fn a20_signal_task_set_sig_blocked(task: *mut task_t, mask: u64);
    pub fn a20_signal_task_thread_pending(task: *mut task_t) -> u64;
    pub fn a20_signal_task_set_thread_pending(task: *mut task_t, mask: u64);
    pub fn a20_signal_task_sigsuspend_active(task: *mut task_t) -> c_int;
    pub fn a20_signal_task_sigsuspend_old_blocked(task: *mut task_t) -> u64;
    pub fn a20_signal_task_set_sigsuspend_active(task: *mut task_t, active: c_int);
    pub fn a20_signal_task_set_sig_old_blocked(task: *mut task_t, mask: u64);
    pub fn a20_signal_task_set_sig_handling(task: *mut task_t, sig: c_int);
    pub fn a20_signal_task_save_sig_ctx(task: *mut task_t, ctx: *const trap_context_t);
    pub fn a20_signal_task_restore_sigsuspend_mask(task: *mut task_t);
    pub fn a20_signal_task_setup_user_frame(
        task: *mut task_t,
        ctx: *mut trap_context_t,
        sig: c_int,
        action: *const sigaction_t,
        old_blocked: u64,
        info: *const siginfo_t,
    ) -> c_int;
    pub fn a20_signal_rt_sigreturn(task: *mut task_t, ctx: *mut trap_context_t) -> i64;
}

pub const NSIG: c_int = 64;
pub const SIGNAL_INFO_SIZE: c_int = 128;

pub const SIG_DFL: usize = 0;
pub const SIG_IGN: usize = 1;

pub const SA_SIGINFO: c_int = 4;
pub const SA_ONSTACK: c_int = 0x08000000;
pub const SA_NODEFER: c_int = 0x40000000;
pub const SA_RESETHAND: c_int = 0x80000000u32 as c_int;

pub const SI_USER: c_int = 0;
pub const SI_KERNEL: c_int = 0x80;
pub const SI_TKILL: c_int = -6;

pub const SIGQUIT: c_int = 3;
pub const SIGILL: c_int = 4;
pub const SIGABRT: c_int = 6;
pub const SIGBUS: c_int = 7;
pub const SIGFPE: c_int = 8;
pub const SIGKILL: c_int = 9;
pub const SIGUSR1: c_int = 10;
pub const SIGSEGV: c_int = 11;
pub const SIGUSR2: c_int = 12;
pub const SIGPIPE: c_int = 13;
pub const SIGALRM: c_int = 14;
pub const SIGTERM: c_int = 15;
pub const SIGCHLD: c_int = 17;
pub const SIGCONT: c_int = 18;
pub const SIGSTOP: c_int = 19;
pub const SIGTSTP: c_int = 20;
pub const SIGTTIN: c_int = 21;
pub const SIGTTOU: c_int = 22;
pub const SIGURG: c_int = 23;
pub const SIGWINCH: c_int = 28;

pub const SIG_BLOCK: c_int = 0;
pub const SIG_UNBLOCK: c_int = 1;
pub const SIG_SETMASK: c_int = 2;

pub const PROC_BLOCKED: c_int = 3;
pub const PROC_STOPPED: c_int = 4;
pub const PROC_RUNNING: c_int = 2;

pub const EFAULT: c_int = 14;
pub const EINVAL: c_int = 22;
pub const ESRCH: c_int = 3;
