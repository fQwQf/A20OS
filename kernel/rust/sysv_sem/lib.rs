#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]
#![warn(rust_2018_idioms)]

mod ffi;

use a20rust_support::lock::raw_irqsave_lock;
use a20rust_support::vfs::spinlock_t as wait_spinlock_t;
use core::ffi::c_void;
use core::mem::{size_of, zeroed};
use core::ptr;

use ffi::{
    sysv_sembuf_t, sysv_sem_set_t, wait_queue_entry_t, EAGAIN, EEXIST, EFAULT, EFBIG, EINVAL,
    EINTR, ENOENT, ENOSPC, GETALL, GETNCNT, GETPID, GETVAL, GETZCNT, IPC_64_BIT, IPC_CREAT,
    IPC_EXCL, IPC_NOWAIT, IPC_RMID, IPC_STAT, SEM_INFO, SEM_STAT, SEM_STAT_ANY, SETALL,
    SETVAL, SYSV_SEM_MAX, SYSV_SEM_OPS_MAX, SYSV_SEM_PER_SET,
};

static mut G_SEM: [sysv_sem_set_t; SYSV_SEM_MAX] = [const {
    sysv_sem_set_t {
        used: 0,
        key: 0,
        nsems: 0,
        val: [0; SYSV_SEM_PER_SET],
        last_pid: 0,
        waiters: ffi::wait_queue_t {
            lock: wait_spinlock_t { locked: 0 },
            head: ptr::null_mut(),
        },
    }
}; SYSV_SEM_MAX];
static mut G_SEM_LOCK: ffi::spinlock_t = ffi::spinlock_t { locked: 0 };

#[inline]
unsafe fn raw_irqsave_lock_wait(lock: *mut wait_spinlock_t) -> a20rust_support::lock::RawIrqSaveGuard {
    unsafe { raw_irqsave_lock(lock.cast()) }
}

#[inline]
fn sem_valid_locked(semid: i32) -> bool {
    semid >= 0
        && (semid as usize) < SYSV_SEM_MAX
        && unsafe { G_SEM[semid as usize].used != 0 }
}

#[inline]
unsafe fn current_pid() -> i32 {
    let cur = unsafe { ffi::sysv_sem_proc_current() };
    if cur.is_null() {
        0
    } else {
        unsafe { ffi::sysv_sem_proc_pid(cur) }
    }
}

unsafe fn sem_copy_all_to_user(set: *const sysv_sem_set_t, arg: *mut c_void) -> i32 {
    if arg.is_null() {
        return -EINVAL;
    }
    let len = unsafe { (*set).nsems as usize } * size_of::<u16>();
    if unsafe { ffi::sysv_sem_copy_to_user(arg, (*set).val.as_ptr().cast(), len) } < 0 {
        -EFAULT
    } else {
        0
    }
}

unsafe fn sem_copy_all_from_user(set: *mut sysv_sem_set_t, arg: *mut c_void) -> i32 {
    if arg.is_null() {
        return -EINVAL;
    }
    let len = unsafe { (*set).nsems as usize } * size_of::<u16>();
    if unsafe { ffi::sysv_sem_copy_from_user((*set).val.as_mut_ptr().cast(), arg.cast_const(), len) } < 0 {
        -EFAULT
    } else {
        0
    }
}

fn sem_ops_can_apply(set: &sysv_sem_set_t, ops: &[sysv_sembuf_t]) -> i32 {
    for op in ops {
        let n = op.sem_num as usize;
        let sem_op = op.sem_op;
        if n >= set.nsems as usize {
            return -EFBIG;
        }
        if sem_op < 0 && set.val[n] < ((-sem_op) as u16) {
            return 0;
        }
        if sem_op == 0 && set.val[n] != 0 {
            return 0;
        }
    }
    1
}

unsafe fn sem_ops_apply(set: *mut sysv_sem_set_t, ops: &[sysv_sembuf_t]) {
    for op in ops {
        let n = op.sem_num as usize;
        if op.sem_op < 0 {
            unsafe {
                (*set).val[n] = ((*set).val[n]).wrapping_sub((-op.sem_op) as u16);
            }
        } else {
            unsafe {
                (*set).val[n] = ((*set).val[n]).wrapping_add(op.sem_op as u16);
            }
        }
    }
    unsafe {
        (*set).last_pid = current_pid();
    }
}

#[no_mangle]
pub unsafe extern "C" fn sysv_sem_get(key: i32, nsems: i32, semflg: i32) -> i32 {
    if nsems < 0 || nsems > SYSV_SEM_PER_SET as i32 {
        return -EINVAL;
    }

    let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(G_SEM_LOCK)) };
    for i in 0..SYSV_SEM_MAX {
        let set = unsafe { &G_SEM[i] };
        if set.used != 0 && set.key == key && key != 0 {
            if (semflg & IPC_CREAT) != 0 && (semflg & IPC_EXCL) != 0 {
                return -EEXIST;
            }
            if nsems > 0 && nsems > set.nsems {
                return -EINVAL;
            }
            return i as i32;
        }
    }

    if (semflg & IPC_CREAT) == 0 {
        return -ENOENT;
    }
    if nsems <= 0 {
        return -EINVAL;
    }

    for i in 0..SYSV_SEM_MAX {
        let set = unsafe { &mut G_SEM[i] };
        if set.used == 0 {
            unsafe {
                *set = zeroed();
                set.used = 1;
                set.key = key;
                set.nsems = nsems;
                ffi::sysv_sem_wait_queue_init(ptr::addr_of_mut!(set.waiters));
            }
            return i as i32;
        }
    }

    -ENOSPC
}

#[no_mangle]
pub unsafe extern "C" fn sysv_sem_control(semid: i32, semnum: i32, mut cmd: i32, arg: *mut c_void) -> i32 {
    cmd &= !IPC_64_BIT;

    let mut wake_waiters: *mut ffi::wait_queue_t = ptr::null_mut();
    let ret;
    {
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(G_SEM_LOCK)) };
        if !sem_valid_locked(semid) {
            return -EINVAL;
        }
        let set = unsafe { &mut G_SEM[semid as usize] };
        if semnum < 0
            || (semnum >= set.nsems
                && cmd != IPC_RMID
                && cmd != IPC_STAT
                && cmd != SEM_STAT
                && cmd != SEM_STAT_ANY
                && cmd != SEM_INFO)
        {
            return -EINVAL;
        }

        match cmd {
            IPC_RMID => {
                set.used = 0;
                wake_waiters = ptr::addr_of_mut!(set.waiters);
                ret = 0;
            }
            GETVAL => ret = set.val[semnum as usize] as i32,
            GETPID => ret = set.last_pid,
            GETNCNT | GETZCNT => ret = 0,
            SETVAL => {
                let raw = arg as usize;
                set.val[semnum as usize] = (raw & 0xffff) as u16;
                set.last_pid = unsafe { current_pid() };
                wake_waiters = ptr::addr_of_mut!(set.waiters);
                ret = 0;
            }
            GETALL => {
                ret = unsafe { sem_copy_all_to_user(set, arg) };
            }
            SETALL => {
                ret = unsafe { sem_copy_all_from_user(set, arg) };
                if ret == 0 {
                    set.last_pid = unsafe { current_pid() };
                    wake_waiters = ptr::addr_of_mut!(set.waiters);
                }
            }
            IPC_STAT | SEM_STAT | SEM_STAT_ANY => {
                if !arg.is_null() {
                    let zero = [0u8; 64];
                    if unsafe { ffi::sysv_sem_copy_to_user(arg, zero.as_ptr().cast(), zero.len()) } < 0 {
                        return -EFAULT;
                    }
                }
                ret = if cmd == IPC_STAT { 0 } else { semid };
            }
            SEM_INFO => {
                if !arg.is_null() {
                    let zero = [0u8; 64];
                    if unsafe { ffi::sysv_sem_copy_to_user(arg, zero.as_ptr().cast(), zero.len()) } < 0 {
                        return -EFAULT;
                    }
                }
                ret = SYSV_SEM_MAX as i32;
            }
            _ => ret = -EINVAL,
        }
    }

    if ret == 0 && !wake_waiters.is_null() {
        unsafe { ffi::sysv_sem_wait_queue_wake_all(wake_waiters) };
    }
    ret
}

#[no_mangle]
pub unsafe extern "C" fn sysv_sem_timedop(
    semid: i32,
    sops: *const c_void,
    nsops: usize,
    deadline: u64,
) -> i32 {
    if sops.is_null() || nsops == 0 || nsops > SYSV_SEM_OPS_MAX {
        return -EINVAL;
    }

    let mut ops = [sysv_sembuf_t {
        sem_num: 0,
        sem_op: 0,
        sem_flg: 0,
    }; SYSV_SEM_OPS_MAX];
    let ops_len = nsops * size_of::<sysv_sembuf_t>();
    if unsafe { ffi::sysv_sem_copy_from_user(ops.as_mut_ptr().cast(), sops, ops_len) } < 0 {
        return -EFAULT;
    }
    let ops = &ops[..nsops];

    loop {
        let wake_waiters;
        {
            let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(G_SEM_LOCK)) };
            if !sem_valid_locked(semid) {
                return -EINVAL;
            }
            let set = unsafe { &mut G_SEM[semid as usize] };
            let can = sem_ops_can_apply(set, ops);
            if can < 0 {
                return can;
            }
            if can > 0 {
                unsafe { sem_ops_apply(set, ops) };
                wake_waiters = ptr::addr_of_mut!(set.waiters);
            } else {
                let nowait = ops.iter().any(|op| (op.sem_flg as i32 & IPC_NOWAIT) != 0);
                if nowait {
                    return -EAGAIN;
                }
                let cur = unsafe { ffi::sysv_sem_proc_current() };
                if unsafe { ffi::sysv_sem_signal_task_has_unblocked(cur) } != 0 {
                    return -EINTR;
                }
                if deadline != 0 && (unsafe { ffi::sysv_sem_timer_get_ticks() } as i64 - deadline as i64) >= 0 {
                    return -EAGAIN;
                }

                let mut entry: wait_queue_entry_t = unsafe { zeroed() };
                entry.task = cur.cast();
                {
                    let _wait_guard = unsafe { raw_irqsave_lock_wait(ptr::addr_of_mut!(set.waiters.lock)) };
                    entry.next = set.waiters.head;
                    entry.prev = ptr::null_mut();
                    if !set.waiters.head.is_null() {
                        unsafe { (*set.waiters.head).prev = ptr::addr_of_mut!(entry) };
                    }
                    set.waiters.head = ptr::addr_of_mut!(entry);
                    if !cur.is_null() {
                        unsafe { ffi::sysv_sem_task_set_blocked(cur) };
                    }
                }
                core::mem::drop(_guard);

                if !cur.is_null() && deadline != 0 {
                    unsafe { ffi::sysv_sem_proc_set_wake_time(cur, deadline) };
                }
                unsafe { ffi::sysv_sem_sched() };
                if !cur.is_null() {
                    unsafe { ffi::sysv_sem_proc_set_wake_time(cur, 0) };
                }
                unsafe {
                    ffi::sysv_sem_wait_queue_finish(
                        ptr::addr_of_mut!(set.waiters),
                        ptr::addr_of_mut!(entry),
                    )
                };
                continue;
            }
        }

        if !wake_waiters.is_null() {
            unsafe { ffi::sysv_sem_wait_queue_wake_all(wake_waiters) };
        }
        return 0;
    }
}

#[no_mangle]
pub unsafe extern "C" fn sysv_sem_op(semid: i32, sops: *const c_void, nsops: usize) -> i32 {
    unsafe { sysv_sem_timedop(semid, sops, nsops, 0) }
}
