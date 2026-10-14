#![no_std]

//! State transitions shared by Linux event and timer descriptors.
//!
//! C owns locks, vfile operations, usercopy, wait queues and timer wakeups.
//! Rust owns counter arithmetic and timer deadline/expiration transitions.

use core::ffi::c_void;

const EVENTFD_MAX: u64 = u64::MAX - 1;
const NSEC_PER_SEC: u64 = 1_000_000_000;

#[no_mangle]
pub extern "C" fn a20_eventfd_readable_rust(counter: u64) -> bool {
    counter != 0
}

#[no_mangle]
/// Read and consume an eventfd counter while its C lock is held.
pub unsafe extern "C" fn a20_eventfd_read_rust(counter: *mut u64, semaphore: bool) -> u64 {
    if counter.is_null() {
        return 0;
    }
    // SAFETY: the caller provides a valid counter protected by its lock.
    let current = unsafe { *counter };
    if current == 0 {
        return 0;
    }
    let value = if semaphore { 1 } else { current };
    unsafe { *counter = current - value };
    value
}

#[no_mangle]
pub unsafe extern "C" fn a20_eventfd_can_write_rust(counter: u64, value: u64) -> bool {
    value <= EVENTFD_MAX - counter
}

#[no_mangle]
/// Add a previously validated increment while the C lock is held.
pub unsafe extern "C" fn a20_eventfd_write_rust(counter: *mut u64, value: u64) -> bool {
    if counter.is_null() {
        return false;
    }
    // SAFETY: the caller provides a valid counter protected by its lock.
    let current = unsafe { *counter };
    if !unsafe { a20_eventfd_can_write_rust(current, value) } {
        return false;
    }
    unsafe { *counter = current + value };
    true
}

#[no_mangle]
pub unsafe extern "C" fn a20_signalfd_set_mask_rust(mask: *mut u64, value: u64) {
    if !mask.is_null() {
        // SAFETY: caller supplies the signalfd mask protected by its vfile ref.
        unsafe { *mask = value };
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_signalfd_mask_rust(mask: *const u64) -> u64 {
    if mask.is_null() {
        return 0;
    }
    // SAFETY: caller holds the signalfd mask lock while reading.
    unsafe { *mask }
}

#[no_mangle]
pub extern "C" fn a20_signalfd_select_rust(pending: u64, mask: u64) -> i32 {
    let matching = pending & mask & !1;
    if matching == 0 {
        0
    } else {
        matching.trailing_zeros() as i32
    }
}

/// Select the lowest pending signal and commit its consumption through a C
/// callback while the caller holds signal_state.lock. Rust owns selection and
/// the consume decision; C owns the protected signal queues and siginfo copy.
#[no_mangle]
pub unsafe extern "C" fn a20_signalfd_consume_rust(
    pending: u64,
    mask: u64,
    take: Option<unsafe extern "C" fn(i32, *mut c_void)>,
    context: *mut c_void,
) -> i32 {
    let selected = a20_signalfd_select_rust(pending, mask);
    if selected == 0 {
        return 0;
    }
    let Some(take) = take else {
        return 0;
    };
    // SAFETY: the C adapter invokes this while holding signal_state.lock and
    // supplies a context valid for the duration of the call.
    unsafe { take(selected, context) };
    selected
}

#[no_mangle]
pub extern "C" fn a20_signalfd_has_pending_rust(pending: u64, mask: u64) -> bool {
    pending & mask & !1 != 0
}

#[no_mangle]
pub unsafe extern "C" fn a20_pidfd_state_init_rust(
    task: *mut *mut c_void,
    value: *mut c_void,
) {
    if !task.is_null() {
        // SAFETY: caller supplies the task identity field in a live pidfd.
        unsafe { *task = value };
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_pidfd_state_task_rust(
    task: *const *mut c_void,
) -> *mut c_void {
    if task.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: caller supplies a live task identity field protected by a vfile ref.
    unsafe { *task }
}

/// Relinquish a pidfd's pinned task exactly once. C supplies the reference
/// release callback because task lifetime accounting remains in proc core.
#[no_mangle]
pub unsafe extern "C" fn a20_pidfd_state_drop_rust(
    task: *mut *mut c_void,
    release: Option<unsafe extern "C" fn(*mut c_void)>,
) -> bool {
    if task.is_null() {
        return false;
    }
    let Some(release) = release else {
        return false;
    };
    // SAFETY: the caller owns the live pidfd state and vfile close is exclusive.
    let value = unsafe { *task };
    if value.is_null() {
        return false;
    }
    // SAFETY: only the successful non-null take owns this reference.
    unsafe { *task = core::ptr::null_mut() };
    // SAFETY: the reference was transferred to this lifecycle callback.
    unsafe { release(value) };
    true
}

#[no_mangle]
pub extern "C" fn a20_pidfd_group_exited_rust(
    task_found: bool,
    state_unused: bool,
    state_zombie: bool,
    thread_group_dead: bool,
) -> bool {
    !task_found || state_unused || (state_zombie && thread_group_dead)
}

#[no_mangle]
pub extern "C" fn a20_pidfd_poll_events_rust(exited: bool, events: i32) -> i32 {
    if !exited {
        return 0;
    }
    0x10 | (events & 0x001)
}

#[no_mangle]
pub extern "C" fn a20_timerfd_timespec_to_ticks_rust(
    sec: u64,
    nsec: u64,
    ticks_per_sec: u64,
) -> u64 {
    let Some(whole) = sec.checked_mul(ticks_per_sec) else {
        return u64::MAX;
    };
    let Some(scaled_nsec) = nsec.checked_mul(ticks_per_sec) else {
        return u64::MAX;
    };
    let fraction = scaled_nsec / NSEC_PER_SEC
        + u64::from(scaled_nsec % NSEC_PER_SEC != 0);
    whole.checked_add(fraction).unwrap_or(u64::MAX)
}

#[no_mangle]
pub unsafe extern "C" fn a20_timerfd_remaining_rust(
    expire_tick: u64,
    now: u64,
    ticks_per_sec: u64,
    sec_out: *mut u64,
    nsec_out: *mut u64,
) {
    if sec_out.is_null() || nsec_out.is_null() || ticks_per_sec == 0 {
        return;
    }
    let remaining = expire_tick.saturating_sub(now);
    // SAFETY: output pointers are writable by caller.
    unsafe {
        *sec_out = remaining / ticks_per_sec;
        *nsec_out = (remaining % ticks_per_sec).wrapping_mul(NSEC_PER_SEC)
            / ticks_per_sec;
    }
}

#[no_mangle]
/// Advance a periodic timer after a read; for a one-shot timer clear `armed`.
pub unsafe extern "C" fn a20_timerfd_consume_expirations_rust(
    now: u64,
    expire_tick: *mut u64,
    interval_ticks: u64,
    armed: *mut i32,
) -> u64 {
    if expire_tick.is_null() || armed.is_null() {
        return 0;
    }
    let mut count: u64 = 1;
    if interval_ticks != 0 {
        // SAFETY: caller provides live fields protected by the timer lock.
        let expiry = unsafe { *expire_tick };
        if now > expiry {
            count = count.saturating_add((now - expiry) / interval_ticks);
        }
        unsafe {
            *expire_tick = expiry.saturating_add(count.saturating_mul(interval_ticks));
        }
    } else {
        unsafe { *armed = 0 };
    }
    count
}

#[no_mangle]
pub extern "C" fn a20_timerfd_absolute_deadline_rust(
    now_sec: u64,
    now_nsec: u64,
    absolute_sec: u64,
    absolute_nsec: u64,
    now_tick: u64,
    ticks_per_sec: u64,
) -> u64 {
    let delta = if absolute_sec > now_sec
        || (absolute_sec == now_sec && absolute_nsec > now_nsec)
    {
        let (sec, nsec) = if absolute_nsec >= now_nsec {
            (absolute_sec - now_sec, absolute_nsec - now_nsec)
        } else {
            (absolute_sec - now_sec - 1, NSEC_PER_SEC + absolute_nsec - now_nsec)
        };
        a20_timerfd_timespec_to_ticks_rust(sec, nsec, ticks_per_sec)
    } else {
        0
    };
    now_tick.checked_add(delta).unwrap_or(u64::MAX)
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;

    #[test]
    fn eventfd_semaphore_and_overflow_transitions() {
        let mut counter = 5;
        unsafe {
            assert!(a20_eventfd_can_write_rust(counter, EVENTFD_MAX - counter));
            assert!(!a20_eventfd_can_write_rust(counter, EVENTFD_MAX - counter + 1));
            assert!(a20_eventfd_write_rust(&mut counter, 7));
            assert_eq!(counter, 12);
            assert_eq!(a20_eventfd_read_rust(&mut counter, true), 1);
            assert_eq!(counter, 11);
            assert_eq!(a20_eventfd_read_rust(&mut counter, false), 11);
            assert_eq!(counter, 0);
            assert_eq!(a20_eventfd_read_rust(&mut counter, false), 0);
        }
    }

    #[test]
    fn timerfd_periodic_read_catches_up_and_oneshot_disarms() {
        let mut expiry = 100;
        let mut armed = 1;
        unsafe {
            assert_eq!(a20_timerfd_consume_expirations_rust(135, &mut expiry, 10, &mut armed), 4);
            assert_eq!(expiry, 140);
            assert_eq!(armed, 1);
            assert_eq!(a20_timerfd_consume_expirations_rust(140, &mut expiry, 0, &mut armed), 1);
            assert_eq!(armed, 0);
        }
    }

    #[test]
    fn absolute_timer_deadline_normalizes_nanosecond_borrow() {
        assert_eq!(a20_timerfd_absolute_deadline_rust(7, 900_000_000, 9, 100_000_000, 50, 100), 170);
        assert_eq!(a20_timerfd_absolute_deadline_rust(7, 900_000_000, 7, 800_000_000, 50, 100), 50);
        assert_eq!(a20_timerfd_timespec_to_ticks_rust(0, 1, 100), 1);
    }

    #[test]
    fn timerfd_rejects_tick_and_absolute_deadline_overflow_sentinel() {
        assert_eq!(a20_timerfd_timespec_to_ticks_rust(u64::MAX, 0, 100), u64::MAX);
        assert_eq!(
            a20_timerfd_absolute_deadline_rust(0, 0, u64::MAX, 0, 10, 100),
            u64::MAX
        );
    }

    #[test]
    fn signalfd_selects_lowest_matching_kernel_signal_number() {
        assert_eq!(a20_signalfd_select_rust((1 << 5) | (1 << 12), (1 << 12) | (1 << 5)), 5);
        assert_eq!(a20_signalfd_select_rust(1 << 5, 1 << 12), 0);
    }

    #[test]
    fn signalfd_consumes_only_selected_signal_through_locked_callback() {
        #[repr(C)]
        struct TakeState {
            selected: i32,
            calls: u32,
        }
        unsafe extern "C" fn record(signal: i32, context: *mut c_void) {
            let state = unsafe { &mut *(context as *mut TakeState) };
            state.selected = signal;
            state.calls += 1;
        }

        let mut state = TakeState { selected: 0, calls: 0 };
        unsafe {
            assert_eq!(
                a20_signalfd_consume_rust(
                    (1 << 5) | (1 << 12),
                    (1 << 5) | (1 << 12),
                    Some(record),
                    &mut state as *mut TakeState as *mut c_void,
                ),
                5
            );
            assert_eq!(state.selected, 5);
            assert_eq!(state.calls, 1);
            assert_eq!(
                a20_signalfd_consume_rust(1 << 5, 1 << 9, Some(record),
                                          &mut state as *mut TakeState as *mut c_void),
                0
            );
            assert_eq!(state.calls, 1);
        }
    }

    #[test]
    fn signalfd_mask_read_and_update_are_identity_preserving_state_ops() {
        let mut mask = 0x40;
        unsafe {
            assert_eq!(a20_signalfd_mask_rust(&mask), 0x40);
            a20_signalfd_set_mask_rust(&mut mask, 0x120);
            assert_eq!(a20_signalfd_mask_rust(&mask), 0x120);
        }
    }

    #[test]
    fn signalfd_readiness_and_pidfd_group_exit_lifecycle() {
        assert!(a20_signalfd_has_pending_rust(1 << 8, 1 << 8));
        assert!(!a20_signalfd_has_pending_rust(1 << 8, 1 << 9));
        assert!(!a20_pidfd_group_exited_rust(true, false, true, false));
        assert!(!a20_pidfd_group_exited_rust(true, false, false, true));
        assert!(a20_pidfd_group_exited_rust(true, false, true, true));
        assert!(a20_pidfd_group_exited_rust(false, false, false, false));
        assert_eq!(a20_pidfd_poll_events_rust(false, 1), 0);
        assert_eq!(a20_pidfd_poll_events_rust(true, 1), 0x11);
        assert_eq!(a20_pidfd_poll_events_rust(true, 0), 0x10);
    }

    #[test]
    fn pidfd_state_retains_opaque_task_identity() {
        let original = 0x1234usize as *mut c_void;
        let later_pid_reuse = 0x5678usize as *mut c_void;
        let mut identity = core::ptr::null_mut();
        unsafe {
            a20_pidfd_state_init_rust(&mut identity, original);
            assert_eq!(a20_pidfd_state_task_rust(&identity), original);
            assert_ne!(a20_pidfd_state_task_rust(&identity), later_pid_reuse);
        }
    }

    #[test]
    fn pidfd_drop_transfers_reference_once() {
        static RELEASED: core::sync::atomic::AtomicUsize =
            core::sync::atomic::AtomicUsize::new(0);
        unsafe extern "C" fn release(value: *mut c_void) {
            assert_eq!(value, 0x1234usize as *mut c_void);
            RELEASED.fetch_add(1, core::sync::atomic::Ordering::SeqCst);
        }
        RELEASED.store(0, core::sync::atomic::Ordering::SeqCst);
        let mut identity = 0x1234usize as *mut c_void;
        unsafe {
            assert!(!a20_pidfd_state_drop_rust(&mut identity, None));
            assert_eq!(identity, 0x1234usize as *mut c_void);
            assert!(a20_pidfd_state_drop_rust(&mut identity, Some(release)));
            assert!(!a20_pidfd_state_drop_rust(&mut identity, Some(release)));
            assert!(identity.is_null());
            assert_eq!(RELEASED.load(core::sync::atomic::Ordering::SeqCst), 1);
        }
    }
}
