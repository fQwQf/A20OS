#![no_std]

mod ffi;

use a20rust_support::lock::raw_irqsave_lock;
use core::ffi::{c_int, c_void};
use core::ptr;

#[inline]
fn wait_accumulate_child_time(parent: *mut ffi::Task, child: *mut ffi::Task) {
    if parent.is_null() || child.is_null() {
        return;
    }
    let total = unsafe { ffi::a20_wait_task_total_time(child) };
    unsafe { ffi::a20_wait_task_add_child_utime(parent, total) };
}

#[inline]
fn wait_task_tgid(task: *mut ffi::Task) -> c_int {
    if task.is_null() {
        -1
    } else {
        let tgid = unsafe { ffi::a20_wait_task_tgid(task) };
        if tgid > 0 {
            tgid
        } else {
            unsafe { ffi::a20_wait_task_pid(task) }
        }
    }
}

#[inline]
fn wait_is_direct_child(child: *mut ffi::Task, parent: *mut ffi::Task) -> bool {
    if child.is_null() || parent.is_null() {
        return false;
    }
    unsafe {
        ffi::a20_wait_task_parent(child) == parent
            || ffi::a20_wait_task_ppid(child) == ffi::a20_wait_task_pid(parent)
    }
}

#[inline]
fn wait_is_child_for_waiter_locked(child: *mut ffi::Task, waiter: *mut ffi::Task, options: c_int) -> bool {
    if (options & ffi::__WNOTHREAD) != 0 {
        return wait_is_direct_child(child, waiter);
    }

    if wait_is_direct_child(child, waiter) {
        return true;
    }

    let parent = unsafe { ffi::a20_wait_task_parent(child) };
    wait_task_tgid(parent) == wait_task_tgid(waiter)
}

#[inline]
fn wait_clone_matches_locked(child: *mut ffi::Task, options: c_int) -> bool {
    if (options & ffi::__WALL) != 0 {
        return true;
    }
    let is_clone = unsafe { ffi::a20_wait_task_is_clone_child(child) != 0 };
    if (options & ffi::__WCLONE) != 0 {
        is_clone
    } else {
        !is_clone
    }
}

#[inline]
fn wait_child_matches_locked(child: *mut ffi::Task, waiter: *mut ffi::Task, pid: c_int, options: c_int) -> bool {
    if !wait_is_child_for_waiter_locked(child, waiter, options) {
        return false;
    }
    if !wait_clone_matches_locked(child, options) {
        return false;
    }

    unsafe {
        if pid > 0 && ffi::a20_wait_task_pid(child) != pid {
            return false;
        }
        if pid == 0 && ffi::a20_wait_task_pgid(child) != ffi::a20_wait_task_pgid(waiter) {
            return false;
        }
        if pid < -1 && ffi::a20_wait_task_pgid(child) != -pid {
            return false;
        }
    }
    true
}

#[no_mangle]
pub extern "C" fn proc_wait4(pid: c_int, status: *mut c_int, options: c_int) -> c_int {
    let waiter = unsafe { ffi::proc_current() };
    if waiter.is_null() {
        return -ffi::ECHILD;
    }

    loop {
        let mut found = false;
        let mut reap_child: *mut ffi::Task = ptr::null_mut();
        let mut result_pid = -1;

        {
            let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(ffi::proc_lock)) };
            let mut child = unsafe { ffi::proc_first_task_locked() };
            while !child.is_null() {
                let next = unsafe { ffi::proc_next_task_locked(child) };
                let state = unsafe { ffi::a20_wait_task_state_acquire(child) };
                if state != ffi::PROC_UNUSED && wait_child_matches_locked(child, waiter, pid, options) {
                    found = true;
                    if state == ffi::PROC_ZOMBIE {
                        let code = unsafe { ffi::a20_wait_task_exit_code_acquire(child) };
                        if !status.is_null() {
                            unsafe {
                                if code >= 0 {
                                    *status = (code & 0xff) << 8;
                                } else {
                                    *status = (-code) & 0xff;
                                }
                            }
                        }
                        result_pid = unsafe { ffi::a20_wait_task_pid(child) };
                        wait_accumulate_child_time(waiter, child);
                        unsafe {
                            ffi::a20_wait_task_set_state(child, ffi::PROC_UNUSED);
                            ffi::proc_unlink_task_locked(child);
                        }
                        reap_child = child;
                        break;
                    }

                    if (options & ffi::WUNTRACED) != 0 && state == ffi::PROC_STOPPED {
                        let sig = unsafe { ffi::a20_wait_task_exit_code_acquire(child) };
                        if !status.is_null() {
                            unsafe { *status = (sig << 8) | 0x7f };
                        }
                        result_pid = unsafe { ffi::a20_wait_task_pid(child) };
                        break;
                    }
                }
                child = next;
            }

            if result_pid <= 0 && found && (options & ffi::WNOHANG) == 0 {
                unsafe {
                    ffi::a20_wait_task_set_waiting_for_child(waiter, 1);
                    ffi::a20_wait_task_set_state(waiter, ffi::PROC_BLOCKED);
                }
                let sig = unsafe { ffi::signal_task_has_unblocked(waiter.cast::<c_void>()) };
                if sig != 0 {
                    unsafe {
                        ffi::a20_wait_task_set_waiting_for_child(waiter, 0);
                        ffi::a20_wait_task_set_state(waiter, ffi::PROC_RUNNING);
                    }
                    return -ffi::ERESTARTSYS;
                }
            }
        }

        if !reap_child.is_null() {
            unsafe { ffi::proc_destroy_task(reap_child) };
            return result_pid;
        }

        if result_pid > 0 {
            return result_pid;
        }

        if !found {
            return -ffi::ECHILD;
        }

        if (options & ffi::WNOHANG) != 0 {
            return 0;
        }

        if unsafe { ffi::a20_wait_task_state_acquire(waiter) } == ffi::PROC_BLOCKED {
            unsafe { ffi::sched() };
        }

        {
            let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(ffi::proc_lock)) };
            unsafe {
                ffi::a20_wait_task_set_waiting_for_child(waiter, 0);
                ffi::a20_wait_task_set_state(waiter, ffi::PROC_RUNNING);
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn proc_wait(status: *mut c_int) -> c_int {
    proc_wait4(-1, status, 0)
}
