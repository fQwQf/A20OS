//! Rust implementation of A20OS timekeeping.
//!
//! Drop-in replacement for kernel/core/timekeeping.c.

#![no_std]
#![allow(static_mut_refs)]
#![deny(unsafe_op_in_unsafe_fn)]
#![warn(rust_2018_idioms)]

mod ffi;

use core::ptr;

use ffi::{
    a20_arch_timer_freq, a20_build_unix_time, a20_irqsave_lock, a20_irqsave_unlock,
    timer_get_ticks, SPINLOCK_INIT,
};

#[repr(C)]
struct TimekeepingState {
    boot_ticks: u64,
    realtime_base_ticks: u64,
    realtime_base_sec: u64,
    realtime_base_nsec: u64,
    lock: ffi::spinlock_t,
}

impl TimekeepingState {
    const fn new() -> Self {
        Self {
            boot_ticks: 0,
            realtime_base_ticks: 0,
            realtime_base_sec: 0,
            realtime_base_nsec: 0,
            lock: SPINLOCK_INIT,
        }
    }
}

static mut G_STATE: TimekeepingState = TimekeepingState::new();

fn ticks_to_timespec(ticks: u64) -> [u64; 2] {
    let freq = unsafe { a20_arch_timer_freq() };
    if freq == 0 {
        return [0, 0];
    }
    [
        ticks / freq,
        (ticks % freq) * 1_000_000_000u64 / freq,
    ]
}

#[no_mangle]
pub unsafe extern "C" fn timekeeping_init() {
    let boot_ticks = unsafe { timer_get_ticks() };
    let build_time = unsafe { a20_build_unix_time() };

    unsafe {
        G_STATE.boot_ticks = boot_ticks;
    }

    // timekeeping_set_realtime with nsec = 0 cannot fail.
    unsafe {
        timekeeping_set_realtime(build_time, 0);
    }
}

#[no_mangle]
pub unsafe extern "C" fn timekeeping_get_monotonic(ts: *mut u64) {
    let boot_ticks = unsafe { G_STATE.boot_ticks };
    let now = unsafe { timer_get_ticks() };
    let tv = ticks_to_timespec(now - boot_ticks);
    unsafe {
        ptr::write(ts.add(0), tv[0]);
        ptr::write(ts.add(1), tv[1]);
    }
}

#[no_mangle]
pub unsafe extern "C" fn timekeeping_get_realtime(ts: *mut u64) {
    let (base_ticks, base_sec, base_nsec) = {
        let flags = unsafe { a20_irqsave_lock(ptr::addr_of_mut!(G_STATE.lock)) };
        let base_ticks = unsafe { G_STATE.realtime_base_ticks };
        let base_sec = unsafe { G_STATE.realtime_base_sec };
        let base_nsec = unsafe { G_STATE.realtime_base_nsec };
        unsafe { a20_irqsave_unlock(ptr::addr_of_mut!(G_STATE.lock), flags) };
        (base_ticks, base_sec, base_nsec)
    };

    let now = unsafe { timer_get_ticks() };
    let delta = ticks_to_timespec(now - base_ticks);
    let mut sec = base_sec + delta[0];
    let mut nsec = base_nsec + delta[1];
    if nsec >= 1_000_000_000u64 {
        sec += nsec / 1_000_000_000u64;
        nsec %= 1_000_000_000u64;
    }
    unsafe {
        ptr::write(ts.add(0), sec);
        ptr::write(ts.add(1), nsec);
    }
}

#[no_mangle]
pub unsafe extern "C" fn timekeeping_set_realtime(sec: u64, mut nsec: u64) -> i32 {
    if nsec >= 1_000_000_000u64 {
        let extra = nsec / 1_000_000_000u64;
        nsec %= 1_000_000_000u64;
        let sec = sec.saturating_add(extra);
        let flags = unsafe { a20_irqsave_lock(ptr::addr_of_mut!(G_STATE.lock)) };
        unsafe {
            G_STATE.realtime_base_ticks = timer_get_ticks();
            G_STATE.realtime_base_sec = sec;
            G_STATE.realtime_base_nsec = nsec;
        }
        unsafe { a20_irqsave_unlock(ptr::addr_of_mut!(G_STATE.lock), flags) };
        return 0;
    }

    let flags = unsafe { a20_irqsave_lock(ptr::addr_of_mut!(G_STATE.lock)) };
    unsafe {
        G_STATE.realtime_base_ticks = timer_get_ticks();
        G_STATE.realtime_base_sec = sec;
        G_STATE.realtime_base_nsec = nsec;
    }
    unsafe { a20_irqsave_unlock(ptr::addr_of_mut!(G_STATE.lock), flags) };
    0
}
