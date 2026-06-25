#![no_std]

mod ffi;

use a20rust_support::lock::IrqSaveSpinLock;
use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use ffi::{vfile_ops_t, vfile_t, wait_queue_t};

struct TimerFdState {
    waiters: wait_queue_t,
    interval_sec: u64,
    interval_nsec: u64,
    value_sec: u64,
    value_nsec: u64,
    expire_tick: u64,
    armed: bool,
    nonblock: bool,
}

struct TimerFd {
    lock: IrqSaveSpinLock<TimerFdState>,
}

extern "C" fn timerfd_close(vf: *mut vfile_t) -> c_int {
    unsafe { ffi::anonfd_free_priv_close(vf) }
}

static TIMERFD_OPS: vfile_ops_t = vfile_ops_t {
    read: Some(timerfd_read),
    write: None,
    lseek: None,
    readdir: None,
    ioctl: None,
    close: Some(timerfd_close),
};

fn ticks_per_sec() -> u64 {
    unsafe { ffi::a20_arch_timer_freq() }
}

fn timespec_to_ticks(sec: u64, nsec: u64) -> u64 {
    let tps = ticks_per_sec();
    sec * tps + (nsec * tps + 999_999_999) / 1_000_000_000
}

impl TimerFd {
    unsafe fn from_vfile(vf: *mut vfile_t) -> Option<*mut TimerFd> {
        let priv_ptr = ffi::a20_timerfd_vfile_priv(vf);
        if priv_ptr.is_null() {
            None
        } else {
            Some(priv_ptr as *mut TimerFd)
        }
    }
}

#[no_mangle]
pub extern "C" fn timerfd_read(vf: *mut vfile_t, buf: *mut c_char, count: usize) -> c_int {
    if buf.is_null() {
        return -ffi::EINVAL;
    }

    let tfd_ptr = match unsafe { TimerFd::from_vfile(vf) } {
        Some(p) => p,
        None => return -ffi::EBADF,
    };
    if count < core::mem::size_of::<u64>() {
        return -ffi::EINVAL;
    }

    let mut guard = unsafe { (*tfd_ptr).lock.lock() };
    while !guard.armed || unsafe { ffi::timer_get_ticks() } < guard.expire_tick {
        if guard.nonblock {
            return -ffi::EAGAIN;
        }
        if guard.armed {
            let now = unsafe { ffi::timer_get_ticks() };
            let remaining = guard.expire_tick.saturating_sub(now);
            if remaining > 0 {
                unsafe { ffi::proc_set_wake_time(ffi::proc_current(), guard.expire_tick) };
            }
        }
        let waiters_ptr = &mut guard.waiters as *mut wait_queue_t;
        drop(guard);
        unsafe { ffi::wait_queue_sleep(waiters_ptr) };
        guard = unsafe { (*tfd_ptr).lock.lock() };
    }

    let mut expirations: u64 = 1;
    let interval = timespec_to_ticks(guard.interval_sec, guard.interval_nsec);
    if interval > 0 {
        let now = unsafe { ffi::timer_get_ticks() };
        if now > guard.expire_tick {
            expirations += (now - guard.expire_tick) / interval;
        }
        guard.expire_tick += expirations * interval;
    } else {
        guard.armed = false;
    }
    drop(guard);

    unsafe {
        ptr::write_unaligned(buf as *mut u64, expirations);
    }
    core::mem::size_of::<u64>() as c_int
}

#[no_mangle]
pub extern "C" fn timerfd_create_file(clockid: c_int, flags: c_int) -> c_int {
    if clockid != 0 && clockid != 1 && clockid != 7 {
        return -ffi::EINVAL;
    }
    let allowed = ffi::O_CLOEXEC | ffi::O_NONBLOCK;
    if flags & !allowed != 0 {
        return -ffi::EINVAL;
    }

    let tfd_size = core::mem::size_of::<TimerFd>();
    let tfd_ptr = unsafe { ffi::kmalloc(tfd_size) as *mut TimerFd };
    if tfd_ptr.is_null() {
        return -ffi::ENOMEM;
    }

    let state = TimerFdState {
        waiters: unsafe { core::mem::zeroed() },
        interval_sec: 0,
        interval_nsec: 0,
        value_sec: 0,
        value_nsec: 0,
        expire_tick: 0,
        armed: false,
        nonblock: (flags & ffi::O_NONBLOCK) != 0,
    };
    unsafe {
        ptr::write(ptr::addr_of_mut!((*tfd_ptr).lock), IrqSaveSpinLock::new(state));
        let mut guard = (*tfd_ptr).lock.lock();
        ffi::wait_queue_init(&mut guard.waiters);
    }

    let vf = unsafe { ffi::a20_timerfd_vfile_alloc() };
    if vf.is_null() {
        unsafe {
            ffi::kfree(tfd_ptr as *mut c_void);
        }
        return -ffi::ENOMEM;
    }

    let vf_flags = ffi::O_RDONLY | (flags & ffi::O_NONBLOCK);
    unsafe {
        ffi::a20_timerfd_vfile_init(vf, ptr::addr_of!(TIMERFD_OPS) as *mut vfile_ops_t, tfd_ptr as *mut c_void, vf_flags);
    }

    let ret = unsafe { ffi::a20_timerfd_install_vfile(vf, flags) };
    if ret < 0 {
        unsafe {
            ffi::kfree(tfd_ptr as *mut c_void);
            ffi::a20_timerfd_vfile_free(vf);
        }
        return ret;
    }
    0
}

#[no_mangle]
pub extern "C" fn timerfd_settime_file(
    gfd: c_int,
    flags: c_int,
    new_value: *const u64,
    old_value: *mut u64,
) -> c_int {
    let vf = unsafe { ffi::vfs_get_file_ref(gfd) };
    if vf.is_null() {
        return -ffi::EINVAL;
    }
    let ops_match = unsafe { ffi::a20_timerfd_vfile_ops_match(vf, ptr::addr_of!(TIMERFD_OPS) as *mut vfile_ops_t) };
    if ops_match == 0 {
        unsafe { ffi::vfs_put_file_ref(gfd, vf) };
        return -ffi::EINVAL;
    }

    let tfd_ptr = match unsafe { TimerFd::from_vfile(vf) } {
        Some(p) => p,
        None => {
            unsafe { ffi::vfs_put_file_ref(gfd, vf) };
            return -ffi::EINVAL;
        }
    };

    if new_value.is_null() {
        unsafe { ffi::vfs_put_file_ref(gfd, vf) };
        return -ffi::EINVAL;
    }

    let mut guard = unsafe { (*tfd_ptr).lock.lock() };
    if !old_value.is_null() {
        unsafe { remaining_locked(&guard, old_value) };
    }

    let new = unsafe { core::slice::from_raw_parts(new_value, 4) };
    if new[1] >= 1_000_000_000 || new[3] >= 1_000_000_000 {
        drop(guard);
        unsafe { ffi::vfs_put_file_ref(gfd, vf) };
        return -ffi::EINVAL;
    }

    guard.interval_sec = new[0];
    guard.interval_nsec = new[1];
    guard.value_sec = new[2];
    guard.value_nsec = new[3];

    let ticks = timespec_to_ticks(new[2], new[3]);
    guard.armed = ticks != 0;
    if guard.armed {
        if flags & ffi::TFD_TIMER_ABSTIME != 0 {
            guard.expire_tick = ticks;
        } else {
            guard.expire_tick = unsafe { ffi::timer_get_ticks() } + ticks;
        }
    }

    let armed = guard.armed;
    let waiters_ptr = &mut guard.waiters as *mut wait_queue_t;
    drop(guard);
    unsafe { ffi::vfs_put_file_ref(gfd, vf) };

    if armed {
        unsafe { ffi::wait_queue_wake_all(waiters_ptr) };
    }
    0
}

#[no_mangle]
pub extern "C" fn timerfd_gettime_file(gfd: c_int, curr_value: *mut u64) -> c_int {
    if curr_value.is_null() {
        return -ffi::EINVAL;
    }

    let vf = unsafe { ffi::vfs_get_file_ref(gfd) };
    if vf.is_null() {
        return -ffi::EINVAL;
    }
    let ops_match = unsafe { ffi::a20_timerfd_vfile_ops_match(vf, ptr::addr_of!(TIMERFD_OPS) as *mut vfile_ops_t) };
    if ops_match == 0 {
        unsafe { ffi::vfs_put_file_ref(gfd, vf) };
        return -ffi::EINVAL;
    }

    let tfd_ptr = match unsafe { TimerFd::from_vfile(vf) } {
        Some(p) => p,
        None => {
            unsafe { ffi::vfs_put_file_ref(gfd, vf) };
            return -ffi::EINVAL;
        }
    };

    let guard = unsafe { (*tfd_ptr).lock.lock() };
    unsafe { remaining_locked(&guard, curr_value) };
    drop(guard);
    unsafe { ffi::vfs_put_file_ref(gfd, vf) };
    0
}

unsafe fn remaining_locked(state: &TimerFdState, out: *mut u64) {
    ptr::write_unaligned(out, state.interval_sec);
    ptr::write_unaligned(out.add(1), state.interval_nsec);
    if !state.armed {
        ptr::write_unaligned(out.add(2), 0);
        ptr::write_unaligned(out.add(3), 0);
        return;
    }
    let now = ffi::timer_get_ticks();
    let rem = state.expire_tick.saturating_sub(now);
    let tps = ticks_per_sec();
    ptr::write_unaligned(out.add(2), rem / tps);
    ptr::write_unaligned(out.add(3), (rem % tps) * 1_000_000_000 / tps);
}
