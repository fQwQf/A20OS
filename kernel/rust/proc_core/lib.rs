#![no_std]

mod ffi;

use a20rust_support::lock::{raw_irqsave_lock, spinlock_t, SPINLOCK_INIT};
use core::ffi::{c_char, c_int};
use core::ptr;

#[no_mangle]
pub static mut proc_lock: spinlock_t = SPINLOCK_INIT;

const KMSG_INIT: &[u8] = b"proc_init: no memory for idle stack\0";
const KMSG_PT: &[u8] = b"proc_init: pt_create failed\0";
const KTHREAD_NAME: &[u8] = b"kthread\0";
const USER_NAME: &[u8] = b"user\0";

#[no_mangle]
pub extern "C" fn idle_loop() {
    loop {
        unsafe { ffi::a20_proc_core_arch_local_irq_enable() };
        unsafe { ffi::kernel_progress_run_bottom_halves() };
        unsafe { ffi::sched_reap_zombies() };
        unsafe { ffi::sched() };
        core::hint::spin_loop();
    }
}

#[inline]
unsafe fn panic_cstr(msg: &[u8]) -> ! {
    ffi::a20_proc_core_panic(msg.as_ptr().cast::<c_char>());
    loop {
        core::hint::spin_loop();
    }
}

#[no_mangle]
pub extern "C" fn proc_init() {
    unsafe {
        ffi::proc_pid_init();
        ffi::proc_sched_runq_init();
        ffi::a20_proc_core_spin_init(core::ptr::addr_of_mut!(proc_lock));

        let idle = ffi::a20_proc_core_idle_task_slot(0);
        ffi::a20_proc_core_zero_task(idle);
        ffi::proc_link_task_locked(idle);
        ffi::a20_proc_core_init_idle_task_fields(idle);
        ffi::proc_pid_register(idle);
        ffi::fdtable_init(idle);
        ffi::a20_proc_core_task_alloc_signals(idle);

        let idle_stack = ffi::a20_proc_core_alloc_zero_kstack();
        if idle_stack.is_null() {
            panic_cstr(KMSG_INIT);
        }

        let kpdir = ffi::a20_proc_core_create_kernel_pgdir();
        if kpdir.is_null() {
            panic_cstr(KMSG_PT);
        }
        ffi::a20_proc_core_kernel_pgdir_shared_set(kpdir);

        if ffi::a20_proc_core_setup_idle_context(idle, idle_stack, kpdir, idle_loop) < 0 {
            panic_cstr(KMSG_INIT);
        }

        ffi::a20_proc_core_activate_idle(idle);
    }
}

#[no_mangle]
pub extern "C" fn proc_idle_task() -> *mut ffi::Task {
    unsafe { ffi::a20_proc_core_idle_task_slot(ffi::a20_proc_core_current_cpu_id()) }
}

#[no_mangle]
pub extern "C" fn proc_kernel_pgdir_shared() -> *mut u64 {
    unsafe { ffi::a20_proc_core_kernel_pgdir_shared_get() }
}

#[no_mangle]
pub extern "C" fn proc_alloc_task_slot() -> *mut ffi::Task {
    unsafe {
        let task = ffi::a20_proc_core_task_alloc_zero();
        if task.is_null() {
            return ptr::null_mut();
        }
        ffi::a20_proc_core_task_set_blocked_dynamic(task);
        let _guard = raw_irqsave_lock(core::ptr::addr_of_mut!(proc_lock));
        ffi::proc_link_task_locked(task);
        task
    }
}

#[no_mangle]
pub extern "C" fn proc_make_ready(task: *mut ffi::Task) {
    unsafe {
        if task.is_null() {
            return;
        }
        let state = ffi::a20_proc_core_task_state(task);
        if state == ffi::PROC_UNUSED || state == ffi::PROC_ZOMBIE {
            return;
        }
        let current_cpu = ffi::a20_proc_core_current_cpu_id();
        let mut target_cpu = current_cpu;
        let _guard = raw_irqsave_lock(core::ptr::addr_of_mut!(proc_lock));
        if ffi::a20_proc_core_task_vfork_waiting(task) != 0 {
            return;
        }
        if ffi::a20_proc_core_task_state(task) != ffi::PROC_READY {
            ffi::a20_proc_core_task_set_state(task, ffi::PROC_READY);
            if ffi::a20_proc_core_task_wake_time(task) == 0 && ffi::a20_proc_core_task_sched_level(task) > 0 {
                ffi::a20_proc_core_task_set_sched_level(task, ffi::a20_proc_core_task_sched_level(task) - 1);
            }
        }
        if ffi::a20_proc_core_task_on_rq(task) == 0 {
            if task == ffi::proc_current() {
                ffi::a20_proc_core_task_set_cpu_id(task, current_cpu);
            } else {
                ffi::a20_proc_core_task_set_cpu_id(task, ffi::proc_sched_select_cpu_locked(task));
            }
        }
        target_cpu = ffi::a20_proc_core_task_cpu_id(task);
        ffi::proc_runq_enqueue_locked(task);
        if target_cpu != current_cpu {
            ffi::proc_sched_kick_cpu(target_cpu);
        }
    }
}

#[no_mangle]
pub extern "C" fn proc_block_until(task: *mut ffi::Task, wake_time: u64) {
    unsafe {
        if task.is_null() {
            return;
        }
        if wake_time != 0 {
            ffi::proc_set_wake_time(task, wake_time);
        }
        let _guard = raw_irqsave_lock(core::ptr::addr_of_mut!(proc_lock));
        ffi::a20_proc_core_task_set_state(task, ffi::PROC_BLOCKED);
    }
}

#[no_mangle]
pub extern "C" fn proc_alloc(entry: extern "C" fn()) -> c_int {
    unsafe {
        let task = proc_alloc_task_slot();
        if task.is_null() {
            return -11;
        }
        let pid = ffi::proc_pid_alloc();
        if pid < 0 {
            ffi::proc_destroy_task(task);
            return -11;
        }
        ffi::a20_proc_core_task_set_pid(task, pid);
        ffi::a20_proc_core_task_set_entry_pgdir(task, 0, ptr::null_mut());
        ffi::proc_task_init_common(task, ffi::proc_current());
        ffi::proc_pid_register(task);
        ffi::fdtable_close_all(task);
        ffi::fdtable_init_stdio(task);
        ffi::proc_set_name(task, KTHREAD_NAME.as_ptr().cast::<c_char>());
        let stack = ffi::a20_proc_core_alloc_zero_kstack();
        if stack.is_null() {
            ffi::proc_destroy_task(task);
            return -12;
        }
        let pgdir = ffi::a20_proc_core_kernel_pgdir_shared_get();
        if ffi::a20_proc_core_setup_kthread_context(task, stack, pgdir, entry) < 0 {
            ffi::proc_destroy_task(task);
            return -12;
        }
        proc_make_ready(task);
        ffi::a20_proc_core_task_pid(task)
    }
}

#[no_mangle]
pub extern "C" fn proc_alloc_user_image(
    entry: u64,
    sp: u64,
    pgdir: *mut u64,
    mmap: *mut ffi::VmArea,
    brk: u64,
    stack_top: u64,
    total_vm: usize,
) -> c_int {
    unsafe {
        let task = proc_alloc_task_slot();
        if task.is_null() {
            return -11;
        }
        let pid = ffi::proc_pid_alloc();
        if pid < 0 {
            ffi::proc_destroy_task(task);
            return -11;
        }
        ffi::a20_proc_core_task_set_pid(task, pid);
        ffi::a20_proc_core_task_set_entry_pgdir(task, entry, pgdir);
        ffi::proc_task_init_common(task, ffi::proc_current());
        ffi::proc_pid_register(task);
        ffi::proc_set_name(task, USER_NAME.as_ptr().cast::<c_char>());

        let stack = ffi::a20_proc_core_alloc_zero_kstack();
        if stack.is_null() {
            ffi::proc_destroy_task(task);
            return -12;
        }
        if ffi::a20_proc_core_setup_user_context(task, stack, entry, sp, pgdir) < 0 {
            ffi::proc_destroy_task(task);
            return -12;
        }
        let mm = ffi::a20_proc_core_create_user_mm(pgdir, mmap, brk, stack_top, sp, total_vm);
        if !mm.is_null() {
            ffi::a20_proc_core_task_set_mm(task, mm);
            ffi::a20_proc_core_task_set_entry_pgdir(task, entry, pgdir);
        }
        proc_make_ready(task);
        pid
    }
}

#[no_mangle]
pub extern "C" fn proc_alloc_user(entry: u64, sp: u64, pgdir: *mut u64) -> c_int {
    proc_alloc_user_image(entry, sp, pgdir, ptr::null_mut(), 0, sp, 0)
}

#[no_mangle]
pub extern "C" fn proc_kill(pid: c_int, signum: c_int) -> c_int {
    unsafe { ffi::signal_send_user(pid, signum) }
}

#[no_mangle]
pub extern "C" fn proc_kill_pgid(pgid: c_int, signum: c_int, skip_self: c_int) -> c_int {
    if signum <= 0 || signum >= ffi::NSIG {
        return -22;
    }
    let self_task = unsafe { ffi::proc_current() };
    let mut count = 0;
    let mut pids = [0i32; 64];
    loop {
        let mut pid_count = 0usize;
        let mut seen = 0;
        unsafe {
            let _guard = raw_irqsave_lock(core::ptr::addr_of_mut!(proc_lock));
            let mut task = ffi::proc_first_task_locked();
            while !task.is_null() {
                if task != proc_idle_task()
                    && ffi::a20_proc_core_task_state(task) != ffi::PROC_UNUSED
                    && ffi::a20_proc_core_task_pgid(task) == pgid
                    && !(skip_self != 0 && task == self_task)
                {
                    if seen >= count {
                        pids[pid_count] = ffi::a20_proc_core_task_pid(task);
                        pid_count += 1;
                        if pid_count == pids.len() {
                            break;
                        }
                    }
                    seen += 1;
                }
                task = ffi::proc_next_task_locked(task);
            }
        }
        if pid_count == 0 {
            break;
        }
        for pid in pids[..pid_count].iter().copied() {
            unsafe { ffi::signal_send_user(pid, signum) };
        }
        count += pid_count as i32;
    }
    if count > 0 { count } else { -3 }
}

#[no_mangle]
pub extern "C" fn proc_get_vm_stats(stats: *mut ffi::ProcVmStats) {
    unsafe {
        if stats.is_null() {
            return;
        }
        (*stats).anon_huge_pages = 0;
        (*stats).shmem_huge_pages = 0;
        (*stats).file_huge_pages = 0;
        let mut seen = [ptr::null_mut::<ffi::Mm>(); 256];
        let mut seen_count = 0usize;
        let _guard = raw_irqsave_lock(core::ptr::addr_of_mut!(proc_lock));
        let mut task = ffi::proc_first_task_locked();
        while !task.is_null() {
            let mm = ffi::a20_proc_core_task_mm(task);
            if ffi::a20_proc_core_task_state(task) != ffi::PROC_UNUSED && !mm.is_null() {
                let mut duplicate = false;
                for known in seen[..seen_count].iter().copied() {
                    if known == mm {
                        duplicate = true;
                        break;
                    }
                }
                if !duplicate {
                    if seen_count < seen.len() {
                        seen[seen_count] = mm;
                        seen_count += 1;
                    }
                    let mut vma = ffi::a20_proc_core_mm_first_vma(mm);
                    while !vma.is_null() {
                        ffi::a20_proc_core_count_vma_huge_pages(mm, vma, stats);
                        vma = ffi::a20_proc_core_vma_next(vma);
                    }
                }
            }
            task = ffi::proc_next_task_locked(task);
        }
    }
}

#[no_mangle]
pub extern "C" fn proc_format_pidmap(buf: *mut c_char, bufsz: usize) -> usize {
    unsafe {
        if buf.is_null() || bufsz == 0 {
            return 0;
        }
        let mut used = 0;
        let _guard = raw_irqsave_lock(core::ptr::addr_of_mut!(proc_lock));
        let mut task = ffi::proc_first_task_locked();
        while !task.is_null() {
            if ffi::a20_proc_core_task_state(task) != ffi::PROC_UNUSED {
                used += 1;
            }
            task = ffi::proc_next_task_locked(task);
        }
        let mut off = ffi::a20_proc_core_pidmap_write_header(buf, bufsz, ffi::proc_pid_max(), ffi::proc_pid_next_value(), used);
        task = ffi::proc_first_task_locked();
        while !task.is_null() && off + 16 < bufsz {
            if ffi::a20_proc_core_task_state(task) != ffi::PROC_UNUSED {
                off = ffi::a20_proc_core_pidmap_append_pid(buf, bufsz, off, ffi::a20_proc_core_task_pid(task));
            }
            task = ffi::proc_next_task_locked(task);
        }
        ffi::a20_proc_core_pidmap_finish(buf, bufsz, off)
    }
}

#[no_mangle]
pub extern "C" fn proc_brk(newbrk: u64) -> u64 {
    unsafe { ffi::a20_proc_core_proc_brk(ffi::proc_current(), newbrk) }
}

#[no_mangle]
pub extern "C" fn proc_mmap(addr: u64, len: usize, prot: c_int, flags: c_int, fd: c_int, off: core::ffi::c_long) -> u64 {
    if off < 0 {
        return -(ffi::EINVAL as i64) as u64;
    }
    unsafe { ffi::a20_proc_core_proc_mmap(ffi::proc_current(), addr, len, prot, flags, fd, off as u64) }
}

#[no_mangle]
pub extern "C" fn proc_munmap(addr: u64, len: usize) -> c_int {
    unsafe { ffi::a20_proc_core_proc_munmap(ffi::proc_current(), addr, len) }
}

#[no_mangle]
pub extern "C" fn proc_dump() {
    unsafe {
        let _guard = raw_irqsave_lock(core::ptr::addr_of_mut!(proc_lock));
        let mut task = ffi::proc_first_task_locked();
        while !task.is_null() {
            let state = ffi::a20_proc_core_task_state(task);
            if state != ffi::PROC_UNUSED {
                ffi::a20_proc_core_dump_task_line(
                    ffi::a20_proc_core_task_pid(task),
                    ffi::a20_proc_core_task_ppid(task),
                    state,
                    ffi::a20_proc_core_task_priority(task),
                    ffi::a20_proc_core_task_name(task),
                );
            }
            task = ffi::proc_next_task_locked(task);
        }
    }
}
