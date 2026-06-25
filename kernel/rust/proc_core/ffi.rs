use a20rust_support::lock::spinlock_t;
use core::ffi::{c_char, c_int, c_uint, c_ulong, c_void};

#[repr(C)]
pub struct Task {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct Mm {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct VmArea {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct ProcVmStats {
    pub anon_huge_pages: usize,
    pub shmem_huge_pages: usize,
    pub file_huge_pages: usize,
}

unsafe extern "C" {
    pub fn proc_pid_init();
    pub fn proc_pid_alloc() -> c_int;
    pub fn proc_pid_register(task: *mut Task);
    pub fn proc_pid_max() -> c_int;
    pub fn proc_pid_next_value() -> c_int;

    pub fn proc_link_task_locked(task: *mut Task);
    pub fn proc_first_task_locked() -> *mut Task;
    pub fn proc_next_task_locked(task: *mut Task) -> *mut Task;

    pub fn proc_current() -> *mut Task;

    pub fn proc_task_init_common(task: *mut Task, parent: *mut Task);
    pub fn proc_destroy_task(task: *mut Task);

    pub fn proc_sched_runq_init();
    pub fn proc_sched_select_cpu_locked(task: *mut Task) -> c_uint;
    pub fn proc_sched_kick_cpu(cpu: c_uint);
    pub fn proc_runq_enqueue_locked(task: *mut Task);

    pub fn fdtable_init(task: *mut Task);
    pub fn fdtable_init_stdio(task: *mut Task);
    pub fn fdtable_close_all(task: *mut Task);

    pub fn proc_set_name(task: *mut Task, name: *const c_char);
    pub fn signal_send_user(pid: c_int, signum: c_int) -> c_int;

    pub fn sched();
    pub fn sched_reap_zombies();
    pub fn kernel_progress_run_bottom_halves();

    pub fn a20_proc_core_current_cpu_id() -> c_uint;
    pub fn a20_proc_core_arch_local_irq_enable();
    pub fn a20_proc_core_spin_init(lock: *mut spinlock_t);
    pub fn a20_proc_core_panic(msg: *const c_char);

    pub fn a20_proc_core_idle_task_slot(cpu: c_uint) -> *mut Task;
    pub fn a20_proc_core_kernel_pgdir_shared_get() -> *mut u64;
    pub fn a20_proc_core_kernel_pgdir_shared_set(pgdir: *mut u64);

    pub fn a20_proc_core_zero_task(task: *mut Task);
    pub fn a20_proc_core_init_idle_task_fields(task: *mut Task);
    pub fn a20_proc_core_task_alloc_zero() -> *mut Task;
    pub fn a20_proc_core_task_set_blocked_dynamic(task: *mut Task);
    pub fn a20_proc_core_task_set_entry_pgdir(task: *mut Task, entry: u64, pgdir: *mut u64);

    pub fn a20_proc_core_task_state(task: *mut Task) -> c_int;
    pub fn a20_proc_core_task_set_state(task: *mut Task, state: c_int);
    pub fn a20_proc_core_task_pid(task: *mut Task) -> c_int;
    pub fn a20_proc_core_task_set_pid(task: *mut Task, pid: c_int);
    pub fn a20_proc_core_task_ppid(task: *mut Task) -> c_int;
    pub fn a20_proc_core_task_pgid(task: *mut Task) -> c_int;
    pub fn a20_proc_core_task_cpu_id(task: *mut Task) -> c_uint;
    pub fn a20_proc_core_task_set_cpu_id(task: *mut Task, cpu: c_uint);
    pub fn a20_proc_core_task_on_rq(task: *mut Task) -> c_int;
    pub fn a20_proc_core_task_vfork_waiting(task: *mut Task) -> c_int;
    pub fn a20_proc_core_task_set_wake_time(task: *mut Task, wake_time: u64);
    pub fn proc_set_wake_time(task: *mut Task, wake_time: u64);
    pub fn a20_proc_core_task_wake_time(task: *mut Task) -> u64;
    pub fn a20_proc_core_task_sched_level(task: *mut Task) -> c_int;
    pub fn a20_proc_core_task_set_sched_level(task: *mut Task, level: c_int);
    pub fn a20_proc_core_task_priority(task: *mut Task) -> c_int;
    pub fn a20_proc_core_task_name(task: *mut Task) -> *const c_char;
    pub fn a20_proc_core_task_mm(task: *mut Task) -> *mut Mm;
    pub fn a20_proc_core_task_set_mm(task: *mut Task, mm: *mut Mm);

    pub fn a20_proc_core_task_alloc_signals(task: *mut Task);
    pub fn a20_proc_core_alloc_zero_kstack() -> *mut c_void;
    pub fn a20_proc_core_create_kernel_pgdir() -> *mut u64;
    pub fn a20_proc_core_setup_idle_context(task: *mut Task, stack: *mut c_void, pgdir: *mut u64, entry: extern "C" fn()) -> c_int;
    pub fn a20_proc_core_activate_idle(task: *mut Task);
    pub fn a20_proc_core_setup_kthread_context(task: *mut Task, stack: *mut c_void, pgdir: *mut u64, entry: extern "C" fn()) -> c_int;
    pub fn a20_proc_core_setup_user_context(task: *mut Task, stack: *mut c_void, entry: u64, sp: u64, pgdir: *mut u64) -> c_int;
    pub fn a20_proc_core_create_user_mm(pgdir: *mut u64, mmap: *mut VmArea, brk: u64, stack_top: u64, sp: u64, total_vm: usize) -> *mut Mm;

    pub fn a20_proc_core_mm_first_vma(mm: *mut Mm) -> *mut VmArea;
    pub fn a20_proc_core_vma_next(vma: *mut VmArea) -> *mut VmArea;
    pub fn a20_proc_core_count_vma_huge_pages(mm: *mut Mm, vma: *mut VmArea, stats: *mut ProcVmStats);

    pub fn a20_proc_core_pidmap_write_header(buf: *mut c_char, bufsz: usize, pid_max: c_int, next_pid: c_int, used: c_int) -> usize;
    pub fn a20_proc_core_pidmap_append_pid(buf: *mut c_char, bufsz: usize, off: usize, pid: c_int) -> usize;
    pub fn a20_proc_core_pidmap_finish(buf: *mut c_char, bufsz: usize, off: usize) -> usize;
    pub fn a20_proc_core_dump_task_line(pid: c_int, ppid: c_int, state: c_int, priority: c_int, name: *const c_char);

    pub fn a20_proc_core_proc_brk(task: *mut Task, newbrk: u64) -> u64;
    pub fn a20_proc_core_proc_mmap(task: *mut Task, addr: u64, len: usize, prot: c_int, flags: c_int, fd: c_int, off: c_ulong) -> u64;
    pub fn a20_proc_core_proc_munmap(task: *mut Task, addr: u64, len: usize) -> c_int;
}

pub const PROC_UNUSED: c_int = 0;
pub const PROC_READY: c_int = 1;
pub const PROC_RUNNING: c_int = 2;
pub const PROC_BLOCKED: c_int = 3;
pub const PROC_ZOMBIE: c_int = 5;

pub const NSIG: c_int = 64;
