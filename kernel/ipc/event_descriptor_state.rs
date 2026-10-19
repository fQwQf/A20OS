#![no_std]

//! State transitions shared by Linux event and timer descriptors.
//!
//! C owns locks, vfile operations, usercopy, clocks and park/wake primitives.
//! Rust owns eventfd and timerfd wait/read/write/poll transactions, plus
//! timerfd schedule replacement and remaining-time snapshots.

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

const EBADF: i32 = 9;
const EAGAIN: i32 = 11;
const EINVAL: i32 = 22;
#[cfg(test)]
const ERESTARTSYS: i32 = 512;
const POLLIN: i32 = 0x001;
const POLLOUT: i32 = 0x004;

unsafe extern "C" {
    fn a20_eventfd_txn_lock(context: *mut c_void);
    fn a20_eventfd_txn_unlock(context: *mut c_void);
    fn a20_eventfd_txn_wait_prepare(context: *mut c_void) -> *mut c_void;
    fn a20_eventfd_txn_wait_link(
        context: *mut c_void,
        waiter: *mut c_void,
        writers: bool,
    ) -> bool;
    fn a20_eventfd_txn_wait_finish(
        waiter: *mut c_void,
        linked: bool,
        commit: bool,
    ) -> i32;
    fn a20_eventfd_txn_wake(context: *mut c_void, writers: bool);
}

/// Wait for one counter condition. On entry, and on every successful return,
/// the caller holds the eventfd lock. Error returns always drop it. The C
/// primitives perform one park operation each; this loop owns admission,
/// persistent-condition rechecks, cancellation and interrupted-wait outcomes.
unsafe fn eventfd_wait_until(
    counter: *mut u64,
    context: *mut c_void,
    nonblock: bool,
    writers: bool,
    increment: u64,
) -> i32 {
    loop {
        let current = unsafe { *counter };
        let ready = if writers {
            unsafe { a20_eventfd_can_write_rust(current, increment) }
        } else {
            current != 0
        };
        if ready {
            return 0;
        }
        if nonblock {
            unsafe { a20_eventfd_txn_unlock(context) };
            return -EAGAIN;
        }

        unsafe { a20_eventfd_txn_unlock(context) };
        let waiter = unsafe { a20_eventfd_txn_wait_prepare(context) };
        if waiter.is_null() {
            return -EAGAIN;
        }

        unsafe { a20_eventfd_txn_lock(context) };
        let current = unsafe { *counter };
        let ready = if writers {
            unsafe { a20_eventfd_can_write_rust(current, increment) }
        } else {
            current != 0
        };
        if ready {
            unsafe { a20_eventfd_txn_unlock(context) };
            let _ = unsafe { a20_eventfd_txn_wait_finish(waiter, false, false) };
            unsafe { a20_eventfd_txn_lock(context) };
            continue;
        }

        let linked = unsafe { a20_eventfd_txn_wait_link(context, waiter, writers) };
        unsafe { a20_eventfd_txn_unlock(context) };
        let result = unsafe { a20_eventfd_txn_wait_finish(waiter, linked, true) };
        if result < 0 {
            return result;
        }
        unsafe { a20_eventfd_txn_lock(context) };
    }
}

/// Consume a normal or semaphore eventfd read. The counter is changed only
/// after the wait protocol observes a readable state under the object lock;
/// writers are woken after that lock is released.
#[no_mangle]
pub unsafe extern "C" fn a20_eventfd_read_txn(
    counter: *mut u64,
    semaphore: bool,
    context: *mut c_void,
    nonblock: bool,
    value_out: *mut u64,
) -> i32 {
    if counter.is_null() || context.is_null() || value_out.is_null() {
        return -EBADF;
    }
    unsafe { a20_eventfd_txn_lock(context) };
    let waited = unsafe { eventfd_wait_until(counter, context, nonblock, false, 0) };
    if waited < 0 {
        return waited;
    }
    let value = unsafe { a20_eventfd_read_rust(counter, semaphore) };
    unsafe {
        a20_eventfd_txn_unlock(context);
        a20_eventfd_txn_wake(context, true);
        *value_out = value;
    }
    0
}

/// Commit one eventfd increment, including the full-counter wait protocol and
/// reader notification. UINT64_MAX remains the reserved invalid increment.
#[no_mangle]
pub unsafe extern "C" fn a20_eventfd_write_txn(
    counter: *mut u64,
    context: *mut c_void,
    nonblock: bool,
    value: u64,
) -> i32 {
    if counter.is_null() || context.is_null() {
        return -EBADF;
    }
    if value == u64::MAX {
        return -EINVAL;
    }
    unsafe { a20_eventfd_txn_lock(context) };
    let waited = unsafe { eventfd_wait_until(counter, context, nonblock, true, value) };
    if waited < 0 {
        return waited;
    }
    if !unsafe { a20_eventfd_write_rust(counter, value) } {
        unsafe { a20_eventfd_txn_unlock(context) };
        return -EAGAIN;
    }
    unsafe {
        a20_eventfd_txn_unlock(context);
        a20_eventfd_txn_wake(context, false);
    }
    0
}

/// Snapshot poll readiness under the same lock used by the read/write
/// transactions, so a returned mask always describes one counter state.
#[no_mangle]
pub unsafe extern "C" fn a20_eventfd_poll_txn(
    counter: *mut u64,
    context: *mut c_void,
    events: i32,
) -> i32 {
    if counter.is_null() || context.is_null() {
        return 0x020;
    }
    unsafe { a20_eventfd_txn_lock(context) };
    let current = unsafe { *counter };
    let mut revents = 0;
    if events & POLLIN != 0 && current != 0 {
        revents |= POLLIN;
    }
    if events & POLLOUT != 0 && current < EVENTFD_MAX {
        revents |= POLLOUT;
    }
    unsafe { a20_eventfd_txn_unlock(context) };
    revents
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

const ECANCELED: i32 = 125;
const EFAULT: i32 = 14;

/// Borrowed view of timerfd_t's business-state tail, beginning at
/// interval_sec. C retains the preceding lock and wait_queue fields.
#[repr(C)]
pub struct TimerFdState {
    interval_sec: u64,
    interval_nsec: u64,
    expire_tick: u64,
    armed: i32,
    nonblock: i32,
    clockid: i32,
    cancel_on_set: i32,
    cancel_gen: u64,
}

const _: () = {
    assert!(core::mem::size_of::<TimerFdState>() == 48);
    assert!(core::mem::offset_of!(TimerFdState, expire_tick) == 16);
    assert!(core::mem::offset_of!(TimerFdState, armed) == 24);
    assert!(core::mem::offset_of!(TimerFdState, nonblock) == 28);
    assert!(core::mem::offset_of!(TimerFdState, clockid) == 32);
    assert!(core::mem::offset_of!(TimerFdState, cancel_on_set) == 36);
    assert!(core::mem::offset_of!(TimerFdState, cancel_gen) == 40);
};

unsafe extern "C" {
    fn a20_timerfd_txn_lock(context: *mut c_void);
    fn a20_timerfd_txn_unlock(context: *mut c_void);
    fn a20_timerfd_txn_wait_prepare(context: *mut c_void, deadline: u64) -> *mut c_void;
    fn a20_timerfd_txn_wait_link(context: *mut c_void, waiter: *mut c_void) -> bool;
    fn a20_timerfd_txn_wait_finish(waiter: *mut c_void, linked: bool, commit: bool) -> i32;
    fn a20_timerfd_txn_wake(context: *mut c_void);
    fn a20_timerfd_txn_now_ticks(context: *mut c_void) -> u64;
    fn a20_timerfd_txn_now_timespec(context: *mut c_void, clockid: i32, out: *mut u64);
    fn a20_timerfd_txn_realtime_generation(context: *mut c_void) -> u64;
}

#[no_mangle]
pub unsafe extern "C" fn a20_timerfd_state_init_rust(
    state: *mut TimerFdState,
    clockid: i32,
    nonblock: bool,
) {
    if state.is_null() {
        return;
    }
    unsafe {
        state.write(TimerFdState {
            interval_sec: 0,
            interval_nsec: 0,
            expire_tick: 0,
            armed: 0,
            nonblock: i32::from(nonblock),
            clockid,
            cancel_on_set: 0,
            cancel_gen: 0,
        });
    }
}

unsafe fn timerfd_canceled_locked(state: &TimerFdState, context: *mut c_void) -> bool {
    state.armed != 0
        && state.cancel_on_set != 0
        && unsafe { a20_timerfd_txn_realtime_generation(context) } != state.cancel_gen
}

unsafe fn timerfd_remaining_locked(
    state: &TimerFdState,
    context: *mut c_void,
    ticks_per_sec: u64,
    out: *mut u64,
) {
    unsafe {
        out.add(0).write(0);
        out.add(1).write(0);
        out.add(2).write(0);
        out.add(3).write(0);
        out.add(0).write(state.interval_sec);
        out.add(1).write(state.interval_nsec);
    }
    if state.armed == 0 {
        return;
    }
    let now = unsafe { a20_timerfd_txn_now_ticks(context) };
    unsafe {
        a20_timerfd_remaining_rust(
            state.expire_tick,
            now,
            ticks_per_sec,
            out.add(2),
            out.add(3),
        );
    }
}

/// Wait for expiration while Rust owns disarm/rearm rechecks and the selected
/// park deadline. Successful returns hold the timer lock; errors drop it.
unsafe fn timerfd_wait_until_expired(state: *mut TimerFdState, context: *mut c_void) -> i32 {
    loop {
        let current = unsafe { &*state };
        let now = unsafe { a20_timerfd_txn_now_ticks(context) };
        if current.armed != 0 && now >= current.expire_tick {
            return 0;
        }
        if current.nonblock != 0 {
            unsafe { a20_timerfd_txn_unlock(context) };
            return -EAGAIN;
        }
        let deadline = if current.armed != 0 { current.expire_tick } else { 0 };
        unsafe { a20_timerfd_txn_unlock(context) };
        let waiter = unsafe { a20_timerfd_txn_wait_prepare(context, deadline) };
        if waiter.is_null() {
            return -EAGAIN;
        }

        unsafe { a20_timerfd_txn_lock(context) };
        let current = unsafe { &*state };
        let now = unsafe { a20_timerfd_txn_now_ticks(context) };
        if current.armed != 0 && now >= current.expire_tick {
            unsafe { a20_timerfd_txn_unlock(context) };
            let _ = unsafe { a20_timerfd_txn_wait_finish(waiter, false, false) };
            unsafe { a20_timerfd_txn_lock(context) };
            continue;
        }
        let linked = unsafe { a20_timerfd_txn_wait_link(context, waiter) };
        unsafe { a20_timerfd_txn_unlock(context) };
        let result = unsafe { a20_timerfd_txn_wait_finish(waiter, linked, true) };
        if result < 0 {
            return result;
        }
        unsafe { a20_timerfd_txn_lock(context) };
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_timerfd_read_txn(
    state: *mut TimerFdState,
    context: *mut c_void,
    ticks_per_sec: u64,
    value_out: *mut u64,
) -> i32 {
    if state.is_null() || context.is_null() || value_out.is_null() {
        return -EBADF;
    }
    if ticks_per_sec == 0 {
        return -EINVAL;
    }
    unsafe { a20_timerfd_txn_lock(context) };
    if unsafe { timerfd_canceled_locked(&*state, context) } {
        unsafe { a20_timerfd_txn_unlock(context) };
        return -ECANCELED;
    }
    let waited = unsafe { timerfd_wait_until_expired(state, context) };
    if waited < 0 {
        return waited;
    }
    let current = unsafe { &mut *state };
    let interval = a20_timerfd_timespec_to_ticks_rust(
        current.interval_sec,
        current.interval_nsec,
        ticks_per_sec,
    );
    let now = unsafe { a20_timerfd_txn_now_ticks(context) };
    let expirations = unsafe {
        a20_timerfd_consume_expirations_rust(
            now,
            &mut current.expire_tick,
            interval,
            &mut current.armed,
        )
    };
    unsafe {
        a20_timerfd_txn_unlock(context);
        *value_out = expirations;
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_timerfd_poll_txn(
    state: *mut TimerFdState,
    context: *mut c_void,
    events: i32,
) -> i32 {
    if state.is_null() || context.is_null() {
        return 0x020;
    }
    unsafe { a20_timerfd_txn_lock(context) };
    let mut revents = 0;
    if events & POLLIN != 0 {
        let current = unsafe { &*state };
        if unsafe { timerfd_canceled_locked(current, context) } {
            revents = POLLIN;
        } else {
            let now = unsafe { a20_timerfd_txn_now_ticks(context) };
            if current.armed != 0 && now >= current.expire_tick {
                revents = POLLIN;
            }
        }
    }
    unsafe { a20_timerfd_txn_unlock(context) };
    revents
}

#[no_mangle]
pub unsafe extern "C" fn a20_timerfd_poll_deadline_txn(
    state: *mut TimerFdState,
    context: *mut c_void,
) -> u64 {
    if state.is_null() || context.is_null() {
        return 0;
    }
    unsafe { a20_timerfd_txn_lock(context) };
    let current = unsafe { &*state };
    let now = unsafe { a20_timerfd_txn_now_ticks(context) };
    let deadline = if current.armed != 0 && current.expire_tick > now {
        current.expire_tick
    } else {
        0
    };
    unsafe { a20_timerfd_txn_unlock(context) };
    deadline
}

#[no_mangle]
pub unsafe extern "C" fn a20_timerfd_gettime_txn(
    state: *mut TimerFdState,
    context: *mut c_void,
    ticks_per_sec: u64,
    curr_value: *mut u64,
) -> i32 {
    if state.is_null() || context.is_null() || curr_value.is_null() {
        return -EFAULT;
    }
    if ticks_per_sec == 0 {
        return -EINVAL;
    }
    unsafe { a20_timerfd_txn_lock(context) };
    unsafe { timerfd_remaining_locked(&*state, context, ticks_per_sec, curr_value) };
    unsafe { a20_timerfd_txn_unlock(context) };
    0
}

/// Validate and atomically replace one timer's schedule. No field is committed
/// until all conversions and overflow checks succeed; waiters are woken only
/// after the new state is visible and the timer lock has been released.
#[no_mangle]
pub unsafe extern "C" fn a20_timerfd_settime_txn(
    state: *mut TimerFdState,
    context: *mut c_void,
    flags: i32,
    new_value: *const u64,
    old_value: *mut u64,
    ticks_per_sec: u64,
) -> i32 {
    if state.is_null() || context.is_null() || new_value.is_null() {
        return -EFAULT;
    }
    if ticks_per_sec == 0 || flags & !3 != 0 || (flags & 2 != 0 && flags & 1 == 0) {
        return -EINVAL;
    }
    let interval_sec = unsafe { *new_value.add(0) };
    let interval_nsec = unsafe { *new_value.add(1) };
    let value_sec = unsafe { *new_value.add(2) };
    let value_nsec = unsafe { *new_value.add(3) };
    if (interval_sec as i64) < 0 || (value_sec as i64) < 0 {
        return -EINVAL;
    }

    unsafe { a20_timerfd_txn_lock(context) };
    if !old_value.is_null() {
        unsafe { timerfd_remaining_locked(&*state, context, ticks_per_sec, old_value) };
    }
    if (interval_nsec as i64) < 0
        || interval_nsec >= NSEC_PER_SEC
        || (value_nsec as i64) < 0
        || value_nsec >= NSEC_PER_SEC
    {
        unsafe { a20_timerfd_txn_unlock(context) };
        return -EINVAL;
    }
    let interval_ticks = a20_timerfd_timespec_to_ticks_rust(
        interval_sec,
        interval_nsec,
        ticks_per_sec,
    );
    let ticks = a20_timerfd_timespec_to_ticks_rust(value_sec, value_nsec, ticks_per_sec);
    if interval_ticks == u64::MAX || ticks == u64::MAX {
        unsafe { a20_timerfd_txn_unlock(context) };
        return -EINVAL;
    }

    let mut new_expire_tick = 0u64;
    let new_armed = i32::from(ticks != 0);
    let mut new_cancel_on_set = 0;
    let mut new_cancel_gen = 0u64;
    if new_armed != 0 {
        if flags & 1 != 0 {
            let mut now_ts = [0u64; 2];
            let clockid = unsafe { (*state).clockid };
            unsafe { a20_timerfd_txn_now_timespec(context, clockid, now_ts.as_mut_ptr()) };
            let now_tick = unsafe { a20_timerfd_txn_now_ticks(context) };
            new_expire_tick = a20_timerfd_absolute_deadline_rust(
                now_ts[0],
                now_ts[1],
                value_sec,
                value_nsec,
                now_tick,
                ticks_per_sec,
            );
            if new_expire_tick == u64::MAX {
                unsafe { a20_timerfd_txn_unlock(context) };
                return -EINVAL;
            }
            if flags & 2 != 0 && clockid == 0 {
                new_cancel_on_set = 1;
                new_cancel_gen = unsafe { a20_timerfd_txn_realtime_generation(context) };
            }
        } else {
            let now_tick = unsafe { a20_timerfd_txn_now_ticks(context) };
            let Some(deadline) = now_tick.checked_add(ticks) else {
                unsafe { a20_timerfd_txn_unlock(context) };
                return -EINVAL;
            };
            new_expire_tick = deadline;
        }
    }

    unsafe {
        let current = &mut *state;
        current.interval_sec = interval_sec;
        current.interval_nsec = interval_nsec;
        current.armed = new_armed;
        current.expire_tick = new_expire_tick;
        current.cancel_on_set = new_cancel_on_set;
        current.cancel_gen = new_cancel_gen;
        a20_timerfd_txn_unlock(context);
        a20_timerfd_txn_wake(context);
    }
    0
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::boxed::Box;

    struct MockEventfd {
        counter: *mut u64,
        locked: bool,
        wakes: [usize; 2],
        prepares: usize,
        links: usize,
        finishes: usize,
        fail_prepare: bool,
        link_success: bool,
        ready_on_prepare: Option<u64>,
        ready_on_commit: Option<u64>,
        finish_result: i32,
        last_linked: bool,
        last_commit: bool,
    }

    impl MockEventfd {
        fn new(counter: &mut u64) -> Self {
            Self {
                counter,
                locked: false,
                wakes: [0; 2],
                prepares: 0,
                links: 0,
                finishes: 0,
                fail_prepare: false,
                link_success: true,
                ready_on_prepare: None,
                ready_on_commit: None,
                finish_result: 0,
                last_linked: false,
                last_commit: false,
            }
        }
    }

    struct MockWaiter {
        context: *mut c_void,
    }

    #[no_mangle]
    unsafe extern "C" fn a20_eventfd_txn_lock(context: *mut c_void) {
        let mock = unsafe { &mut *(context as *mut MockEventfd) };
        assert!(!mock.locked);
        mock.locked = true;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_eventfd_txn_unlock(context: *mut c_void) {
        let mock = unsafe { &mut *(context as *mut MockEventfd) };
        assert!(mock.locked);
        mock.locked = false;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_eventfd_txn_wait_prepare(context: *mut c_void) -> *mut c_void {
        let mock = unsafe { &mut *(context as *mut MockEventfd) };
        assert!(!mock.locked);
        mock.prepares += 1;
        if mock.fail_prepare {
            return core::ptr::null_mut();
        }
        if let Some(value) = mock.ready_on_prepare.take() {
            unsafe { *mock.counter = value };
        }
        Box::into_raw(Box::new(MockWaiter { context })).cast()
    }

    #[no_mangle]
    unsafe extern "C" fn a20_eventfd_txn_wait_link(
        context: *mut c_void,
        _waiter: *mut c_void,
        _writers: bool,
    ) -> bool {
        let mock = unsafe { &mut *(context as *mut MockEventfd) };
        assert!(mock.locked);
        mock.links += 1;
        mock.link_success
    }

    #[no_mangle]
    unsafe extern "C" fn a20_eventfd_txn_wait_finish(
        waiter: *mut c_void,
        linked: bool,
        commit: bool,
    ) -> i32 {
        let waiter = unsafe { Box::from_raw(waiter as *mut MockWaiter) };
        let mock = unsafe { &mut *(waiter.context as *mut MockEventfd) };
        assert!(!mock.locked);
        mock.finishes += 1;
        mock.last_linked = linked;
        mock.last_commit = commit;
        if commit && linked {
            if let Some(value) = mock.ready_on_commit.take() {
                unsafe { *mock.counter = value };
            }
        }
        mock.finish_result
    }

    #[no_mangle]
    unsafe extern "C" fn a20_eventfd_txn_wake(context: *mut c_void, writers: bool) {
        let mock = unsafe { &mut *(context as *mut MockEventfd) };
        assert!(!mock.locked);
        mock.wakes[usize::from(writers)] += 1;
    }

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
    fn eventfd_transaction_nonblock_poll_and_wake_order() {
        let mut counter = 0u64;
        let mut mock = MockEventfd::new(&mut counter);
        let context = &mut mock as *mut MockEventfd as *mut c_void;
        let mut out = 0u64;
        unsafe {
            assert_eq!(a20_eventfd_read_txn(&mut counter, false, context, true, &mut out), -EAGAIN);
            assert_eq!(mock.prepares, 0);
            assert_eq!(a20_eventfd_write_txn(&mut counter, context, true, u64::MAX), -EINVAL);
            assert_eq!(a20_eventfd_write_txn(&mut counter, context, true, 3), 0);
            assert_eq!(counter, 3);
            assert_eq!(mock.wakes[0], 1);
            assert_eq!(a20_eventfd_poll_txn(&mut counter, context, POLLIN | POLLOUT), POLLIN | POLLOUT);
            assert_eq!(a20_eventfd_read_txn(&mut counter, true, context, true, &mut out), 0);
            assert_eq!((out, counter), (1, 2));
            assert_eq!(mock.wakes[1], 1);

            counter = EVENTFD_MAX;
            assert_eq!(a20_eventfd_poll_txn(&mut counter, context, POLLIN | POLLOUT), POLLIN);
            assert_eq!(a20_eventfd_write_txn(&mut counter, context, true, 1), -EAGAIN);
            assert_eq!(a20_eventfd_read_txn(&mut counter, false, context, true, &mut out), 0);
            assert_eq!((out, counter), (EVENTFD_MAX, 0));
        }
        assert!(!mock.locked);
    }

    #[test]
    fn eventfd_transaction_blocking_read_and_write_recheck_after_wake() {
        let mut counter = 0u64;
        let mut mock = MockEventfd::new(&mut counter);
        mock.ready_on_commit = Some(9);
        let context = &mut mock as *mut MockEventfd as *mut c_void;
        let mut out = 0u64;
        unsafe {
            assert_eq!(a20_eventfd_read_txn(&mut counter, false, context, false, &mut out), 0);
        }
        assert_eq!((out, counter), (9, 0));
        assert_eq!((mock.prepares, mock.links, mock.finishes), (1, 1, 1));
        assert!(mock.last_linked && mock.last_commit);
        assert!(!mock.locked);

        counter = EVENTFD_MAX;
        mock.ready_on_commit = Some(0);
        unsafe {
            assert_eq!(a20_eventfd_write_txn(&mut counter, context, false, 2), 0);
        }
        assert_eq!(counter, 2);
        assert_eq!((mock.prepares, mock.links, mock.finishes), (2, 2, 2));
        assert!(!mock.locked);
    }

    #[test]
    fn eventfd_transaction_wait_failures_do_not_commit_counter() {
        let mut counter = 0u64;
        let mut mock = MockEventfd::new(&mut counter);
        mock.fail_prepare = true;
        let context = &mut mock as *mut MockEventfd as *mut c_void;
        let mut out = 77u64;
        unsafe {
            assert_eq!(a20_eventfd_read_txn(&mut counter, false, context, false, &mut out), -EAGAIN);
        }
        assert_eq!((out, counter), (77, 0));
        assert!(!mock.locked);

        mock.fail_prepare = false;
        mock.finish_result = -ERESTARTSYS;
        unsafe {
            assert_eq!(a20_eventfd_read_txn(&mut counter, false, context, false, &mut out), -ERESTARTSYS);
        }
        assert_eq!((out, counter), (77, 0));
        assert_eq!(mock.wakes, [0, 0]);
        assert!(!mock.locked);
    }

    #[test]
    fn eventfd_transaction_cancels_prepare_when_state_becomes_ready() {
        let mut counter = 0u64;
        let mut mock = MockEventfd::new(&mut counter);
        mock.ready_on_prepare = Some(4);
        let context = &mut mock as *mut MockEventfd as *mut c_void;
        let mut out = 0u64;
        unsafe {
            assert_eq!(a20_eventfd_read_txn(&mut counter, false, context, false, &mut out), 0);
        }
        assert_eq!((out, counter), (4, 0));
        assert_eq!((mock.prepares, mock.links, mock.finishes), (1, 0, 1));
        assert!(!mock.last_linked && !mock.last_commit);
        assert!(!mock.locked);
    }

    struct MockTimerFd {
        locked: bool,
        now: u64,
        now_ts: [u64; 2],
        generation: u64,
        prepares: usize,
        links: usize,
        finishes: usize,
        wakes: usize,
        fail_prepare: bool,
        ready_on_commit: Option<u64>,
        finish_result: i32,
        last_deadline: u64,
    }

    impl MockTimerFd {
        fn new(_state: &mut TimerFdState) -> Self {
            Self {
                locked: false,
                now: 0,
                now_ts: [100, 0],
                generation: 7,
                prepares: 0,
                links: 0,
                finishes: 0,
                wakes: 0,
                fail_prepare: false,
                ready_on_commit: None,
                finish_result: 0,
                last_deadline: u64::MAX,
            }
        }
    }

    struct MockTimerWaiter {
        context: *mut c_void,
    }

    #[no_mangle]
    unsafe extern "C" fn a20_timerfd_txn_lock(context: *mut c_void) {
        let mock = unsafe { &mut *(context as *mut MockTimerFd) };
        assert!(!mock.locked);
        mock.locked = true;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_timerfd_txn_unlock(context: *mut c_void) {
        let mock = unsafe { &mut *(context as *mut MockTimerFd) };
        assert!(mock.locked);
        mock.locked = false;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_timerfd_txn_wait_prepare(
        context: *mut c_void,
        deadline: u64,
    ) -> *mut c_void {
        let mock = unsafe { &mut *(context as *mut MockTimerFd) };
        assert!(!mock.locked);
        mock.prepares += 1;
        mock.last_deadline = deadline;
        if mock.fail_prepare {
            return core::ptr::null_mut();
        }
        Box::into_raw(Box::new(MockTimerWaiter { context })).cast()
    }

    #[no_mangle]
    unsafe extern "C" fn a20_timerfd_txn_wait_link(
        context: *mut c_void,
        _waiter: *mut c_void,
    ) -> bool {
        let mock = unsafe { &mut *(context as *mut MockTimerFd) };
        assert!(mock.locked);
        mock.links += 1;
        true
    }

    #[no_mangle]
    unsafe extern "C" fn a20_timerfd_txn_wait_finish(
        waiter: *mut c_void,
        linked: bool,
        commit: bool,
    ) -> i32 {
        let waiter = unsafe { Box::from_raw(waiter as *mut MockTimerWaiter) };
        let mock = unsafe { &mut *(waiter.context as *mut MockTimerFd) };
        assert!(!mock.locked);
        mock.finishes += 1;
        if commit && linked {
            if let Some(now) = mock.ready_on_commit.take() {
                mock.now = now;
            }
        }
        mock.finish_result
    }

    #[no_mangle]
    unsafe extern "C" fn a20_timerfd_txn_wake(context: *mut c_void) {
        let mock = unsafe { &mut *(context as *mut MockTimerFd) };
        assert!(!mock.locked);
        mock.wakes += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_timerfd_txn_now_ticks(context: *mut c_void) -> u64 {
        let mock = unsafe { &mut *(context as *mut MockTimerFd) };
        mock.now
    }

    #[no_mangle]
    unsafe extern "C" fn a20_timerfd_txn_now_timespec(
        context: *mut c_void,
        _clockid: i32,
        out: *mut u64,
    ) {
        let mock = unsafe { &mut *(context as *mut MockTimerFd) };
        unsafe {
            out.add(0).write(mock.now_ts[0]);
            out.add(1).write(mock.now_ts[1]);
        }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_timerfd_txn_realtime_generation(context: *mut c_void) -> u64 {
        let mock = unsafe { &mut *(context as *mut MockTimerFd) };
        mock.generation
    }

    #[test]
    fn timerfd_transaction_settime_gettime_and_failed_commit() {
        let mut state = TimerFdState {
            interval_sec: 0,
            interval_nsec: 0,
            expire_tick: 0,
            armed: 0,
            nonblock: 0,
            clockid: 0,
            cancel_on_set: 0,
            cancel_gen: 0,
        };
        unsafe { a20_timerfd_state_init_rust(&mut state, 1, true) };
        let mut mock = MockTimerFd::new(&mut state);
        mock.now = 50;
        let context = &mut mock as *mut MockTimerFd as *mut c_void;
        let new_value = [1, 0, 2, 0];
        let mut old_value = [0u64; 4];
        unsafe {
            assert_eq!(a20_timerfd_settime_txn(&mut state, context, 0, new_value.as_ptr(), old_value.as_mut_ptr(), 100), 0);
        }
        assert_eq!((state.interval_sec, state.expire_tick, state.armed), (1, 250, 1));
        assert_eq!(mock.wakes, 1);

        mock.now = 60;
        let mut current = [0u64; 4];
        unsafe {
            assert_eq!(a20_timerfd_gettime_txn(&mut state, context, 100, current.as_mut_ptr()), 0);
        }
        assert_eq!(current, [1, 0, 1, 900_000_000]);

        let committed = (state.interval_sec, state.expire_tick, state.armed);
        let invalid = [4, 0, 3, NSEC_PER_SEC];
        let overflow = [0, 0, i64::MAX as u64, 0];
        unsafe {
            assert_eq!(a20_timerfd_settime_txn(&mut state, context, 0, invalid.as_ptr(), core::ptr::null_mut(), 100), -EINVAL);
            assert_eq!(a20_timerfd_settime_txn(&mut state, context, 2, new_value.as_ptr(), core::ptr::null_mut(), 100), -EINVAL);
            assert_eq!(a20_timerfd_settime_txn(&mut state, context, 0, overflow.as_ptr(), core::ptr::null_mut(), 100), -EINVAL);
        }
        assert_eq!((state.interval_sec, state.expire_tick, state.armed), committed);
        assert_eq!(mock.wakes, 1);
        assert!(!mock.locked);
    }

    #[test]
    fn timerfd_transaction_blocking_read_uses_deadline_and_consumes() {
        let mut state = TimerFdState {
            interval_sec: 0,
            interval_nsec: 0,
            expire_tick: 100,
            armed: 1,
            nonblock: 0,
            clockid: 1,
            cancel_on_set: 0,
            cancel_gen: 0,
        };
        let mut mock = MockTimerFd::new(&mut state);
        mock.ready_on_commit = Some(100);
        let context = &mut mock as *mut MockTimerFd as *mut c_void;
        let mut out = 0u64;
        unsafe {
            assert_eq!(a20_timerfd_read_txn(&mut state, context, 100, &mut out), 0);
        }
        assert_eq!((out, state.armed), (1, 0));
        assert_eq!(mock.last_deadline, 100);
        assert_eq!((mock.prepares, mock.links, mock.finishes), (1, 1, 1));
        assert!(!mock.locked);

        state.armed = 1;
        state.expire_tick = 200;
        mock.finish_result = -ERESTARTSYS;
        unsafe {
            assert_eq!(a20_timerfd_read_txn(&mut state, context, 100, &mut out), -ERESTARTSYS);
        }
        assert_eq!(state.armed, 1);
        assert!(!mock.locked);
    }

    #[test]
    fn timerfd_transaction_absolute_cancel_poll_and_disarm() {
        let mut state = TimerFdState {
            interval_sec: 0,
            interval_nsec: 0,
            expire_tick: 0,
            armed: 0,
            nonblock: 0,
            clockid: 0,
            cancel_on_set: 0,
            cancel_gen: 0,
        };
        unsafe { a20_timerfd_state_init_rust(&mut state, 0, true) };
        let mut mock = MockTimerFd::new(&mut state);
        mock.now = 10;
        let context = &mut mock as *mut MockTimerFd as *mut c_void;
        let absolute = [0, 0, 110, 0];
        unsafe {
            assert_eq!(a20_timerfd_settime_txn(&mut state, context, 3, absolute.as_ptr(), core::ptr::null_mut(), 100), 0);
            assert_eq!(a20_timerfd_poll_deadline_txn(&mut state, context), 1010);
        }
        assert_eq!((state.cancel_on_set, state.cancel_gen), (1, 7));

        mock.generation = 8;
        let mut out = 99u64;
        unsafe {
            assert_eq!(a20_timerfd_poll_txn(&mut state, context, POLLIN), POLLIN);
            assert_eq!(a20_timerfd_read_txn(&mut state, context, 100, &mut out), -ECANCELED);
        }
        assert_eq!(out, 99);

        let disarm = [0u64; 4];
        unsafe {
            assert_eq!(a20_timerfd_settime_txn(&mut state, context, 0, disarm.as_ptr(), core::ptr::null_mut(), 100), 0);
            assert_eq!(a20_timerfd_poll_txn(&mut state, context, POLLIN), 0);
            assert_eq!(a20_timerfd_poll_deadline_txn(&mut state, context), 0);
            assert_eq!(a20_timerfd_read_txn(&mut state, context, 100, &mut out), -EAGAIN);
        }
        assert_eq!((state.armed, state.cancel_on_set, state.cancel_gen), (0, 0, 0));
        assert!(!mock.locked);
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
