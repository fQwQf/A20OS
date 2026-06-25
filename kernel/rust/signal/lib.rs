#![no_std]

mod ffi;

use core::ffi::{c_int, c_void};
use core::{mem, ptr};

use ffi::{sigaction_t, siginfo_t, signal_state_t, task_t, trap_context_t, user_rt_sigaction_t, user_sigset_t};

#[inline]
fn signal_mask_bit(sig: c_int) -> u64 {
    if sig > 0 && sig < 64 {
        1u64 << sig
    } else {
        0
    }
}

#[inline]
fn signal_mask_from_user(user_mask: u64) -> u64 {
    user_mask << 1
}

#[inline]
fn signal_mask_to_user(kernel_mask: u64) -> u64 {
    kernel_mask >> 1
}

fn signal_core_dump_default(sig: c_int) -> bool {
    matches!(
        sig,
        ffi::SIGQUIT | ffi::SIGILL | ffi::SIGABRT | ffi::SIGBUS | ffi::SIGFPE | ffi::SIGSEGV | 31
    )
}

fn signal_wait_status(sig: c_int) -> c_int {
    let mut status = sig & 0x7f;
    if signal_core_dump_default(sig) {
        status |= 0x80;
    }
    status
}

fn signal_default_terminate(sig: c_int) -> bool {
    !matches!(
        sig,
        ffi::SIGCHLD
            | ffi::SIGURG
            | ffi::SIGWINCH
            | ffi::SIGSTOP
            | ffi::SIGTSTP
            | ffi::SIGTTIN
            | ffi::SIGTTOU
            | ffi::SIGCONT
    )
}

fn signal_default_stop(sig: c_int) -> bool {
    matches!(sig, ffi::SIGSTOP | ffi::SIGTSTP | ffi::SIGTTIN | ffi::SIGTTOU)
}

fn signal_default_ignore(sig: c_int) -> bool {
    matches!(sig, ffi::SIGCHLD | ffi::SIGURG | ffi::SIGWINCH)
}

unsafe fn current() -> *mut task_t {
    unsafe { ffi::proc_current() }
}

unsafe fn task_signals(task: *mut task_t) -> *mut signal_state_t {
    unsafe { ffi::a20_signal_task_signals(task) }
}

unsafe fn task_is_user(task: *mut task_t) -> bool {
    unsafe { ffi::a20_signal_task_has_pgdir(task) != 0 }
}

fn build_siginfo_code(si: &mut siginfo_t, sig: c_int, sender: *mut task_t, code: c_int) {
    *si = siginfo_t {
        si_signo: 0,
        si_errno: 0,
        si_code: 0,
        _sifields: [0; 29],
    };
    si.si_signo = sig;
    si.si_code = code;
    if !sender.is_null() {
        si._sifields[0] = 0;
        si._sifields[1] = unsafe { ffi::a20_signal_task_pid(sender) };
        si._sifields[2] = unsafe { ffi::a20_signal_task_uid(sender) };
    }
}

fn build_siginfo(si: &mut siginfo_t, sig: c_int, sender: *mut task_t) {
    build_siginfo_code(si, sig, sender, ffi::SI_USER);
}

unsafe fn clear_signal_pending(task: *mut task_t, ss: *mut signal_state_t, sig: c_int) {
    unsafe {
        (*ss).pending &= !signal_mask_bit(sig);
        let tp = ffi::a20_signal_task_thread_pending(task) & !signal_mask_bit(sig);
        ffi::a20_signal_task_set_thread_pending(task, tp);
        (*ss).pending_has_info[sig as usize] = 0;
    }
}

unsafe fn restore_sigsuspend_after_ignored(task: *mut task_t) {
    if unsafe { ffi::a20_signal_task_sigsuspend_active(task) } != 0 {
        unsafe { ffi::a20_signal_task_restore_sigsuspend_mask(task) };
        unsafe { ffi::a20_signal_task_set_sigsuspend_active(task, 0) };
    }
}

unsafe fn wake_if_needed(task: *mut task_t) {
    let state = unsafe { ffi::a20_signal_task_state(task) };
    if state == ffi::PROC_BLOCKED || state == ffi::PROC_STOPPED {
        unsafe { ffi::proc_make_ready(task) };
    }
}

#[no_mangle]
pub extern "C" fn signal_init(ss: *mut signal_state_t) {
    if ss.is_null() {
        return;
    }
    unsafe {
        ffi::memset(ss as *mut c_void, 0, mem::size_of::<signal_state_t>());
        (*ss).refcount.value = 1;
    }
}

#[no_mangle]
pub extern "C" fn signal_copy(src: *const signal_state_t, dst: *mut signal_state_t) {
    if src.is_null() || dst.is_null() {
        return;
    }
    unsafe {
        ffi::memcpy(
            dst as *mut c_void,
            src as *const c_void,
            mem::size_of::<signal_state_t>(),
        );
        (*dst).refcount.value = 1;
        (*dst).pending = 0;
        ffi::memset(
            (*dst).pending_has_info.as_mut_ptr() as *mut c_void,
            0,
            mem::size_of_val(&(*dst).pending_has_info),
        );
        ffi::memset(
            (*dst).pending_info.as_mut_ptr() as *mut c_void,
            0,
            mem::size_of_val(&(*dst).pending_info),
        );
    }
}

#[no_mangle]
pub extern "C" fn signal_send_info(
    pid: c_int,
    signum: c_int,
    info: *const c_void,
    info_size: usize,
) -> c_int {
    if signum <= 0 || signum >= ffi::NSIG {
        return -ffi::EINVAL;
    }
    let task = unsafe { ffi::proc_find(pid) };
    if task.is_null() {
        return -ffi::ESRCH;
    }
    let ss = unsafe { task_signals(task) };
    if ss.is_null() {
        return -ffi::EINVAL;
    }

    unsafe {
        let sa = &mut (*ss).actions[signum as usize];
        if sa.sa_handler == ffi::SIG_IGN {
            return 0;
        }

        let is_user = task_is_user(task);
        let blocked = ffi::a20_signal_task_sig_blocked(task);
        if !is_user
            && (blocked & signal_mask_bit(signum)) == 0
            && sa.sa_handler == ffi::SIG_DFL
            && signal_default_terminate(signum)
        {
            ffi::proc_force_exit(task, -signal_wait_status(signum));
            return 0;
        }

        if !info.is_null() && info_size != 0 {
            let n = core::cmp::min(info_size, ffi::SIGNAL_INFO_SIZE as usize);
            ffi::memcpy(
                (*ss).pending_info[signum as usize].as_mut_ptr() as *mut c_void,
                info,
                n,
            );
            if n < ffi::SIGNAL_INFO_SIZE as usize {
                ffi::memset(
                    (*ss).pending_info[signum as usize].as_mut_ptr().add(n) as *mut c_void,
                    0,
                    ffi::SIGNAL_INFO_SIZE as usize - n,
                );
            }
            (*ss).pending_has_info[signum as usize] = 1;
        } else {
            (*ss).pending_has_info[signum as usize] = 0;
            ffi::memset(
                (*ss).pending_info[signum as usize].as_mut_ptr() as *mut c_void,
                0,
                ffi::SIGNAL_INFO_SIZE as usize,
            );
            let signum_bytes = signum.to_ne_bytes();
            for (i, b) in signum_bytes.iter().enumerate() {
                (*ss).pending_info[signum as usize][i] = *b;
            }
        }
        (*ss).pending |= signal_mask_bit(signum);
        wake_if_needed(task);

        if !is_user && ptr::eq(task, current()) {
            signal_deliver();
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn signal_send_user(pid: c_int, signum: c_int) -> c_int {
    let mut si = siginfo_t {
        si_signo: 0,
        si_errno: 0,
        si_code: 0,
        _sifields: [0; 29],
    };
    build_siginfo(&mut si, signum, unsafe { current() });
    signal_send_info(pid, signum, &si as *const _ as *const c_void, mem::size_of::<siginfo_t>())
}

#[no_mangle]
pub extern "C" fn signal_send_thread(tid: c_int, signum: c_int) -> c_int {
    if signum <= 0 || signum >= ffi::NSIG {
        return -ffi::EINVAL;
    }
    let task = unsafe { ffi::proc_find(tid) };
    if task.is_null() {
        return -ffi::ESRCH;
    }
    let ss = unsafe { task_signals(task) };
    if ss.is_null() {
        return -ffi::EINVAL;
    }
    unsafe {
        let sa = &(*ss).actions[signum as usize];
        if sa.sa_handler == ffi::SIG_IGN {
            return 0;
        }
        let is_user = task_is_user(task);
        let blocked = ffi::a20_signal_task_sig_blocked(task);
        if !is_user
            && (blocked & signal_mask_bit(signum)) == 0
            && sa.sa_handler == ffi::SIG_DFL
            && signal_default_terminate(signum)
        {
            ffi::proc_force_exit(task, -signal_wait_status(signum));
            return 0;
        }
        let pending = ffi::a20_signal_task_thread_pending(task) | signal_mask_bit(signum);
        ffi::a20_signal_task_set_thread_pending(task, pending);
        wake_if_needed(task);
    }
    0
}

#[no_mangle]
pub extern "C" fn signal_send_thread_user(tid: c_int, signum: c_int) -> c_int {
    if signum <= 0 || signum >= ffi::NSIG {
        return -ffi::EINVAL;
    }
    let task = unsafe { ffi::proc_find(tid) };
    if task.is_null() {
        return -ffi::ESRCH;
    }
    let ss = unsafe { task_signals(task) };
    if ss.is_null() {
        return -ffi::EINVAL;
    }
    unsafe {
        let sa = &(*ss).actions[signum as usize];
        if sa.sa_handler == ffi::SIG_IGN {
            return 0;
        }
        let is_user = task_is_user(task);
        let blocked = ffi::a20_signal_task_sig_blocked(task);
        if !is_user
            && (blocked & signal_mask_bit(signum)) == 0
            && sa.sa_handler == ffi::SIG_DFL
            && signal_default_terminate(signum)
        {
            ffi::proc_force_exit(task, -signal_wait_status(signum));
            return 0;
        }
        let mut si = siginfo_t {
            si_signo: 0,
            si_errno: 0,
            si_code: 0,
            _sifields: [0; 29],
        };
        build_siginfo_code(&mut si, signum, current(), ffi::SI_TKILL);
        ffi::memset(
            (*ss).pending_info[signum as usize].as_mut_ptr() as *mut c_void,
            0,
            ffi::SIGNAL_INFO_SIZE as usize,
        );
        ffi::memcpy(
            (*ss).pending_info[signum as usize].as_mut_ptr() as *mut c_void,
            &si as *const _ as *const c_void,
            mem::size_of::<siginfo_t>(),
        );
        (*ss).pending_has_info[signum as usize] = 1;
        let pending = ffi::a20_signal_task_thread_pending(task) | signal_mask_bit(signum);
        ffi::a20_signal_task_set_thread_pending(task, pending);
        wake_if_needed(task);
    }
    0
}

#[no_mangle]
pub extern "C" fn signal_send(pid: c_int, signum: c_int) -> c_int {
    signal_send_info(pid, signum, ptr::null(), 0)
}

#[no_mangle]
pub extern "C" fn signal_task_has_unblocked(task: *mut c_void) -> c_int {
    let task = task as *mut task_t;
    if task.is_null() {
        return 0;
    }
    let ss = unsafe { task_signals(task) };
    if ss.is_null() {
        return 0;
    }
    if unsafe { ffi::a20_signal_task_exit_pending(task) } != 0 {
        return 1;
    }
    let deliverable = unsafe {
        ((*ss).pending | ffi::a20_signal_task_thread_pending(task))
            & !ffi::a20_signal_task_sig_blocked(task)
    };
    if deliverable == 0 {
        return 0;
    }
    for sig in 1..ffi::NSIG {
        if (deliverable & signal_mask_bit(sig)) == 0 {
            continue;
        }
        let sa = unsafe { &(*ss).actions[sig as usize] };
        if sa.sa_handler == ffi::SIG_IGN {
            continue;
        }
        if sa.sa_handler == ffi::SIG_DFL && signal_default_ignore(sig) {
            continue;
        }
        return 1;
    }
    0
}

#[no_mangle]
pub extern "C" fn signal_deliver() {
    let task = unsafe { current() };
    if task.is_null() {
        return;
    }
    let ss = unsafe { task_signals(task) };
    if ss.is_null() {
        return;
    }
    let deliverable = unsafe {
        ((*ss).pending | ffi::a20_signal_task_thread_pending(task))
            & !ffi::a20_signal_task_sig_blocked(task)
    };
    if deliverable == 0 || unsafe { task_is_user(task) } {
        return;
    }

    for sig in 1..ffi::NSIG {
        if (deliverable & signal_mask_bit(sig)) == 0 {
            continue;
        }
        unsafe {
            let sa = &mut (*ss).actions[sig as usize];
            if sa.sa_handler == ffi::SIG_IGN {
                clear_signal_pending(task, ss, sig);
                continue;
            }
            if sa.sa_handler == ffi::SIG_DFL {
                clear_signal_pending(task, ss, sig);
                if signal_default_ignore(sig) {
                    continue;
                }
                if signal_default_stop(sig) {
                    ffi::a20_signal_task_set_state(task, ffi::PROC_STOPPED);
                    ffi::a20_signal_task_set_exit_code(task, sig);
                    ffi::sched();
                    continue;
                }
                ffi::proc_exit_group(-signal_wait_status(sig));
            }

            clear_signal_pending(task, ss, sig);
            let handler: extern "C" fn(c_int) = mem::transmute(sa.sa_handler);
            handler(sig);
            return;
        }
    }
}

#[no_mangle]
pub extern "C" fn signal_deliver_user(ctx: *mut trap_context_t) {
    if ctx.is_null() {
        return;
    }
    let task = unsafe { current() };
    if task.is_null() || unsafe { !task_is_user(task) } {
        return;
    }
    let ss = unsafe { task_signals(task) };
    if ss.is_null() {
        return;
    }
    let deliverable = unsafe {
        ((*ss).pending | ffi::a20_signal_task_thread_pending(task))
            & !ffi::a20_signal_task_sig_blocked(task)
    };
    if deliverable == 0 {
        return;
    }

    for sig in 1..ffi::NSIG {
        if (deliverable & signal_mask_bit(sig)) == 0 {
            continue;
        }
        unsafe {
            let sa = &mut (*ss).actions[sig as usize];
            if sa.sa_handler == ffi::SIG_IGN {
                clear_signal_pending(task, ss, sig);
                restore_sigsuspend_after_ignored(task);
                continue;
            }

            if sa.sa_handler == ffi::SIG_DFL {
                clear_signal_pending(task, ss, sig);
                if signal_default_ignore(sig) {
                    restore_sigsuspend_after_ignored(task);
                    continue;
                }
                if signal_default_stop(sig) {
                    if ffi::a20_signal_task_sigsuspend_active(task) != 0 {
                        ffi::a20_signal_task_restore_sigsuspend_mask(task);
                    }
                    ffi::a20_signal_task_set_sigsuspend_active(task, 0);
                    ffi::a20_signal_task_set_state(task, ffi::PROC_STOPPED);
                    ffi::a20_signal_task_set_exit_code(task, sig);
                    ffi::sched();
                    ffi::a20_signal_task_set_state(task, ffi::PROC_RUNNING);
                    continue;
                }
                ffi::proc_exit_group(-signal_wait_status(sig));
            }

            let mut queued_info = siginfo_t {
                si_signo: 0,
                si_errno: 0,
                si_code: 0,
                _sifields: [0; 29],
            };
            let has_queued_info = (*ss).pending_has_info[sig as usize] != 0;
            if has_queued_info {
                ptr::copy_nonoverlapping(
                    (*ss).pending_info[sig as usize].as_ptr() as *const siginfo_t,
                    &mut queued_info,
                    1,
                );
            }

            clear_signal_pending(task, ss, sig);
            if (sa.sa_flags & ffi::SA_RESETHAND) != 0 {
                sa.sa_handler = ffi::SIG_DFL;
            }

            ffi::a20_signal_task_save_sig_ctx(task, ctx);
            let old_blocked = if ffi::a20_signal_task_sigsuspend_active(task) != 0 {
                ffi::a20_signal_task_sigsuspend_old_blocked(task)
            } else {
                ffi::a20_signal_task_sig_blocked(task)
            };
            ffi::a20_signal_task_set_sig_old_blocked(task, old_blocked);
            ffi::a20_signal_task_set_sigsuspend_active(task, 0);

            let mut blocked = ffi::a20_signal_task_sig_blocked(task) | sa.sa_mask;
            if (sa.sa_flags & ffi::SA_NODEFER) == 0 {
                blocked |= signal_mask_bit(sig);
            }
            ffi::a20_signal_task_set_sig_blocked(task, blocked);
            ffi::a20_signal_task_set_sig_handling(task, sig);

            let info_ptr = if has_queued_info {
                &queued_info as *const siginfo_t
            } else {
                ptr::null()
            };
            if ffi::a20_signal_task_setup_user_frame(task, ctx, sig, sa as *const _, old_blocked, info_ptr)
                < 0
            {
                ffi::proc_exit_group(-signal_wait_status(ffi::SIGSEGV));
            }
            return;
        }
    }
}

#[no_mangle]
pub extern "C" fn sys_sigaction_impl(
    signum: c_int,
    act: *const c_void,
    oldact: *mut c_void,
    sigsetsize: usize,
) -> c_int {
    if signum <= 0 || signum >= ffi::NSIG {
        return -ffi::EINVAL;
    }
    if signum == ffi::SIGKILL || signum == ffi::SIGSTOP {
        return -ffi::EINVAL;
    }
    if sigsetsize != mem::size_of::<user_sigset_t>() {
        return -ffi::EINVAL;
    }
    let task = unsafe { current() };
    if task.is_null() {
        return -ffi::EINVAL;
    }
    let ss = unsafe { task_signals(task) };
    if ss.is_null() {
        return -ffi::EINVAL;
    }
    unsafe {
        if !oldact.is_null() {
            let oldk = user_rt_sigaction_t {
                handler: (*ss).actions[signum as usize].sa_handler,
                flags: ((*ss).actions[signum as usize].sa_flags as u32) as u64,
                mask: signal_mask_to_user((*ss).actions[signum as usize].sa_mask),
            };
            if ffi::copy_to_user(oldact, &oldk as *const _ as *const c_void, mem::size_of::<user_rt_sigaction_t>()) < 0 {
                return -ffi::EFAULT;
            }
        }
        if !act.is_null() {
            let mut ukact = user_rt_sigaction_t {
                handler: 0,
                flags: 0,
                mask: 0,
            };
            if ffi::copy_from_user(
                &mut ukact as *mut _ as *mut c_void,
                act,
                mem::size_of::<user_rt_sigaction_t>(),
            ) < 0
            {
                return -ffi::EFAULT;
            }
            (*ss).actions[signum as usize].sa_handler = ukact.handler;
            (*ss).actions[signum as usize].sa_mask = signal_mask_from_user(ukact.mask);
            (*ss).actions[signum as usize].sa_flags = ukact.flags as c_int;
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn sys_sigprocmask_impl(
    how: c_int,
    set: *const c_void,
    oldset: *mut c_void,
    sigsetsize: usize,
) -> c_int {
    if sigsetsize != mem::size_of::<user_sigset_t>() {
        return -ffi::EINVAL;
    }
    let task = unsafe { current() };
    if task.is_null() || unsafe { task_signals(task) }.is_null() {
        return -ffi::EINVAL;
    }
    unsafe {
        if !oldset.is_null() {
            let oldmask = user_sigset_t {
                bits: [signal_mask_to_user(ffi::a20_signal_task_sig_blocked(task))],
            };
            if ffi::copy_to_user(oldset, &oldmask as *const _ as *const c_void, mem::size_of::<user_sigset_t>()) < 0 {
                return -ffi::EFAULT;
            }
        }
        if set.is_null() {
            return 0;
        }
        let mut usermask = user_sigset_t { bits: [0] };
        if ffi::copy_from_user(&mut usermask as *mut _ as *mut c_void, set, mem::size_of::<user_sigset_t>()) < 0 {
            return -ffi::EFAULT;
        }
        let mut mask = signal_mask_from_user(usermask.bits[0]);
        mask &= !(signal_mask_bit(ffi::SIGKILL) | signal_mask_bit(ffi::SIGSTOP));
        let cur = ffi::a20_signal_task_sig_blocked(task);
        let next = match how {
            ffi::SIG_BLOCK => cur | mask,
            ffi::SIG_UNBLOCK => cur & !mask,
            ffi::SIG_SETMASK => mask,
            _ => return -ffi::EINVAL,
        };
        ffi::a20_signal_task_set_sig_blocked(task, next);
    }
    0
}

#[no_mangle]
pub extern "C" fn sys_rt_sigreturn_impl(ctx: *mut trap_context_t) -> i64 {
    let task = unsafe { current() };
    if task.is_null() || unsafe { task_signals(task) }.is_null() || ctx.is_null() {
        return -(ffi::EFAULT as i64);
    }
    unsafe { ffi::a20_signal_rt_sigreturn(task, ctx) }
}
