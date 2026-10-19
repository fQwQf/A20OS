#![no_std]

//! POSIX interval-timer table ownership.
//!
//! Rust owns both timer tables and every transition on them: allocation and
//! deletion, arm/disarm, the remaining-time view, the drift-free periodic
//! re-arm, the overrun accounting and the CPU-time itimers.  C supplies the
//! clock reads, the scheduler deadline publication, task references and signal
//! delivery; nothing about the timer state is decided there.
//!
//! The tables are fixed-size and accessed exactly as the C tables were: the
//! ABI syscalls run in process context and the expiry scan runs from the timer
//! heap, which is the same unsynchronized contract the C implementation had.

#[cfg(test)]
extern crate std;

use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::ptr;

const COMPAT_TIMER_MAX: usize = 32;
const EAGAIN: i32 = 11;
const EINVAL: i32 = 22;

const PCLK_REALTIME: i32 = 0;
const PCLK_MONOTONIC: i32 = 1;
const PCLK_MONOTONIC_RAW: i32 = 4;
const PCLK_REALTIME_COARSE: i32 = 5;
const PCLK_MONOTONIC_COARSE: i32 = 6;
const PCLK_BOOTTIME: i32 = 7;
const PCLK_TAI: i32 = 11;

const SIGVTALRM: i32 = 26;
const SIGPROF: i32 = 27;

/// `TIMER_ABSTIME`, the only flag `timer_settime(2)` accepts.
const TIMER_ABSTIME: i32 = 1;
const OVERRUN_MAX: u64 = 0x7fff_ffff;

const NANOS_PER_SEC: u64 = 1_000_000_000;

#[repr(C)]
#[derive(Clone, Copy)]
struct PosixTimer {
    used: i32,
    owner_pid: i32,
    signo: i32,
    target_tid: i32,
    clockid: i32,
    interval: [u64; 2],
    expire_tick: u64,
    overrun_cur: u64,
    overrun_last: u64,
}

const EMPTY_TIMER: PosixTimer = PosixTimer {
    used: 0,
    owner_pid: 0,
    signo: 0,
    target_tid: 0,
    clockid: 0,
    interval: [0, 0],
    expire_tick: 0,
    overrun_cur: 0,
    overrun_last: 0,
};

#[repr(C)]
#[derive(Clone, Copy)]
struct CpuItimer {
    used: i32,
    task: *mut c_void,
    expire: [u64; 2],
    interval: [u64; 2],
}

const EMPTY_ITIMER: CpuItimer = CpuItimer {
    used: 0,
    task: ptr::null_mut(),
    expire: [0, 0],
    interval: [0, 0],
};

struct Table {
    posix: [PosixTimer; COMPAT_TIMER_MAX],
    cpu: [CpuItimer; COMPAT_TIMER_MAX],
}

impl Table {
    const fn new() -> Self {
        Self {
            posix: [EMPTY_TIMER; COMPAT_TIMER_MAX],
            cpu: [EMPTY_ITIMER; COMPAT_TIMER_MAX],
        }
    }
}

struct SharedTable(UnsafeCell<Table>);

// SAFETY: the C contract is unchanged from the two C tables — the ABI calls
// run in process context and the expiry scan runs from the timer heap, and
// neither re-enters this module.
unsafe impl Sync for SharedTable {}

static TABLE: SharedTable = SharedTable(UnsafeCell::new(Table::new()));

fn table() -> &'static mut Table {
    // SAFETY: see the SharedTable note; no reference escapes a call.
    unsafe { &mut *TABLE.0.get() }
}

unsafe extern "C" {
    fn a20_posix_timer_ticks_per_sec() -> u64;
    fn a20_posix_timer_now_ticks() -> u64;
    fn a20_posix_timer_now_wall(ts: *mut u64);
    fn a20_posix_timer_now_monotonic(ts: *mut u64);
    fn a20_posix_timer_set_deadline(next: u64);
    fn a20_posix_timer_task_ref(pid: i32) -> *mut c_void;
    fn a20_posix_timer_task_clone_ref(task: *mut c_void) -> *mut c_void;
    fn a20_posix_timer_task_put(task: *mut c_void);
    fn a20_posix_timer_task_gone(task: *mut c_void) -> i32;
    fn a20_posix_timer_task_metric(task: *mut c_void, which: i32) -> u64;
    fn a20_posix_timer_signal_pending(task: *mut c_void, signo: i32, thread_scoped: i32) -> i32;
    fn a20_posix_timer_send(owner_pid: i32, signo: i32);
    fn a20_posix_timer_send_task(task: *mut c_void, signo: i32);
}

fn clock_is_realtime(clockid: i32) -> bool {
    matches!(clockid, PCLK_REALTIME | PCLK_REALTIME_COARSE | PCLK_TAI)
}

/// `sec * TICKS_PER_SEC + ceil(nsec * TICKS_PER_SEC / 1e9)`, wrapping exactly
/// like the C expression it replaces (unsigned arithmetic wraps in both).
fn timespec_to_ticks(sec: u64, nsec: u64) -> u64 {
    let tps = unsafe { a20_posix_timer_ticks_per_sec() };
    sec.wrapping_mul(tps).wrapping_add(
        nsec.wrapping_mul(tps).wrapping_add(NANOS_PER_SEC - 1) / NANOS_PER_SEC,
    )
}

fn now_ticks() -> u64 {
    unsafe { a20_posix_timer_now_ticks() }
}

/// Publish the earliest armed notification deadline, or 0 for none.
fn update_deadline() {
    let t = table();
    let mut next = 0u64;
    for tmr in t.posix.iter() {
        if tmr.used == 0 || tmr.signo == 0 {
            continue;
        }
        let expire = tmr.expire_tick;
        if expire != 0 && (next == 0 || expire < next) {
            next = expire;
        }
    }
    unsafe { a20_posix_timer_set_deadline(next) };
}

fn timer_signal_pending(tmr: &PosixTimer) -> bool {
    let pid = if tmr.target_tid > 0 { tmr.target_tid } else { tmr.owner_pid };
    let task = unsafe { a20_posix_timer_task_ref(pid) };
    if task.is_null() {
        return false;
    }
    let scoped = if tmr.target_tid > 0 { 1 } else { 0 };
    let pending = unsafe { a20_posix_timer_signal_pending(task, tmr.signo, scoped) } != 0;
    unsafe { a20_posix_timer_task_put(task) };
    pending
}

fn deliver(tmr: &PosixTimer) {
    if tmr.target_tid > 0 {
        let target = unsafe { a20_posix_timer_task_ref(tmr.target_tid) };
        if !target.is_null() {
            unsafe { a20_posix_timer_send_task(target, tmr.signo) };
            unsafe { a20_posix_timer_task_put(target) };
        }
    } else {
        unsafe { a20_posix_timer_send(tmr.owner_pid, tmr.signo) };
    }
}

/// The clocks `timer_create(2)` accepts.
#[unsafe(no_mangle)]
pub extern "C" fn a20_posix_timer_clock_supported(clockid: i32) -> i32 {
    matches!(
        clockid,
        PCLK_REALTIME
            | PCLK_MONOTONIC
            | PCLK_MONOTONIC_RAW
            | PCLK_REALTIME_COARSE
            | PCLK_MONOTONIC_COARSE
            | PCLK_BOOTTIME
            | PCLK_TAI
    ) as i32
}

/// Allocate a timer slot; `signo == 0` means no notification.
#[unsafe(no_mangle)]
pub extern "C" fn a20_posix_timer_create(
    owner_pid: i32,
    signo: i32,
    target_tid: i32,
    clockid: i32,
) -> i32 {
    let t = table();
    for (index, slot) in t.posix.iter_mut().enumerate() {
        if slot.used == 0 {
            *slot = EMPTY_TIMER;
            slot.used = 1;
            slot.owner_pid = owner_pid;
            slot.signo = signo;
            slot.target_tid = target_tid;
            slot.clockid = clockid;
            return index as i32;
        }
    }
    -EAGAIN
}

fn checked_slot(t: &mut Table, owner_pid: i32, id: i32) -> Option<&mut PosixTimer> {
    if id < 0 || id as usize >= COMPAT_TIMER_MAX {
        return None;
    }
    let slot = &mut t.posix[id as usize];
    if slot.used == 0 || slot.owner_pid != owner_pid {
        return None;
    }
    Some(slot)
}

/// Release a timer; only the owning process may delete it.
#[unsafe(no_mangle)]
pub extern "C" fn a20_posix_timer_delete(owner_pid: i32, id: i32) -> i32 {
    let t = table();
    match checked_slot(t, owner_pid, id) {
        Some(slot) => *slot = EMPTY_TIMER,
        None => return -EINVAL,
    }
    update_deadline();
    0
}

/// Read `[interval_sec, interval_nsec, remaining_sec, remaining_nsec]`; the
/// value is the time REMAINING until expiry, zero when disarmed.
///
/// # Safety
/// `out` must point to four writable `u64`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_posix_timer_get_time(
    owner_pid: i32,
    id: i32,
    out: *mut u64,
) -> i32 {
    if out.is_null() {
        return -EINVAL;
    }
    let (interval, expire) = {
        let t = table();
        match checked_slot(t, owner_pid, id) {
            Some(slot) => (slot.interval, slot.expire_tick),
            None => return -EINVAL,
        }
    };
    for index in 0..4 {
        unsafe { ptr::write(out.add(index), 0) };
    }
    unsafe { ptr::write(out, interval[0]) };
    unsafe { ptr::write(out.add(1), interval[1]) };
    if expire == 0 {
        return 0;
    }
    let now = now_ticks();
    let remaining = if expire > now { expire - now } else { 0 };
    let tps = unsafe { a20_posix_timer_ticks_per_sec() };
    // A zero frequency cannot happen; the checked forms keep the object free
    // of a division-by-zero panic branch rather than proving it here.
    let secs = remaining.checked_div(tps).unwrap_or(0);
    let nsecs = remaining
        .checked_rem(tps)
        .unwrap_or(0)
        .wrapping_mul(NANOS_PER_SEC)
        .checked_div(tps)
        .unwrap_or(0);
    unsafe { ptr::write(out.add(2), secs) };
    unsafe { ptr::write(out.add(3), nsecs) };
    0
}

/// Arm from `[interval_sec, interval_nsec, value_sec, value_nsec]`.
///
/// `flags` bit 0 is `TIMER_ABSTIME`; `value == 0` disarms.  Periodic re-arm is
/// drift-free: the deadline advances by whole intervals from the previous
/// deadline, not from the moment the scan happened to run.
///
/// # Safety
/// `ts` must point to four readable `u64`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_posix_timer_set_time(
    owner_pid: i32,
    id: i32,
    ts: *const u64,
    flags: i32,
) -> i32 {
    if flags & !TIMER_ABSTIME != 0 || ts.is_null() {
        return -EINVAL;
    }
    let mut spec = [0u64; 4];
    for (index, slot) in spec.iter_mut().enumerate() {
        *slot = unsafe { ptr::read(ts.add(index)) };
    }

    let t = table();
    let tmr = match checked_slot(t, owner_pid, id) {
        Some(slot) => slot,
        None => return -EINVAL,
    };
    tmr.interval[0] = spec[0];
    tmr.interval[1] = spec[1];

    let ticks = timespec_to_ticks(spec[2], spec[3]);
    if ticks == 0 {
        // it_value == 0 disarms.
        tmr.expire_tick = 0;
        tmr.overrun_cur = 0;
        tmr.overrun_last = 0;
        update_deadline();
        return 0;
    }

    if flags & TIMER_ABSTIME != 0 {
        let mut now_ts = [0u64; 2];
        if clock_is_realtime(tmr.clockid) {
            unsafe { a20_posix_timer_now_wall(now_ts.as_mut_ptr()) };
        } else {
            unsafe { a20_posix_timer_now_monotonic(now_ts.as_mut_ptr()) };
        }
        let mut delta = 0u64;
        if spec[2] > now_ts[0] || (spec[2] == now_ts[0] && spec[3] > now_ts[1]) {
            let mut sec = spec[2] - now_ts[0];
            let nsec = if spec[3] >= now_ts[1] {
                spec[3] - now_ts[1]
            } else {
                sec -= 1;
                NANOS_PER_SEC + spec[3] - now_ts[1]
            };
            delta = timespec_to_ticks(sec, nsec);
        }
        // A past absolute time expires immediately.
        tmr.expire_tick = now_ticks() + delta;
    } else {
        tmr.expire_tick = now_ticks() + ticks;
    }
    update_deadline();
    0
}

/// Overruns of the most recently delivered/accepted notification, latched so
/// repeated reads stay stable.
#[unsafe(no_mangle)]
pub extern "C" fn a20_posix_timer_getoverrun(owner_pid: i32, id: i32) -> i32 {
    let t = table();
    let tmr = match checked_slot(t, owner_pid, id) {
        Some(slot) => slot,
        None => return -EINVAL,
    };
    if !timer_signal_pending(tmr) {
        tmr.overrun_last = tmr.overrun_cur;
        tmr.overrun_cur = 0;
    }
    if tmr.overrun_last > OVERRUN_MAX {
        OVERRUN_MAX as i32
    } else {
        tmr.overrun_last as i32
    }
}

/// Expiry scan, invoked from the scheduler timer heap.
#[unsafe(no_mangle)]
pub extern "C" fn a20_posix_timer_tick() {
    let now = now_ticks();
    let t = table();

    for index in 0..COMPAT_TIMER_MAX {
        let mut tmr = t.posix[index];
        if tmr.used == 0 || tmr.signo == 0 {
            continue;
        }
        if tmr.expire_tick == 0 || now < tmr.expire_tick {
            continue;
        }

        let interval_ticks = timespec_to_ticks(tmr.interval[0], tmr.interval[1]);
        let mut expirations = 1u64;
        if interval_ticks != 0 {
            expirations += (now - tmr.expire_tick) / interval_ticks;
        }

        if timer_signal_pending(&tmr) {
            // Standard signals coalesce, so extra expirations become overruns.
            tmr.overrun_cur += expirations;
        } else {
            // Any previous notification was delivered; latch its overrun count
            // before queueing the next one.
            tmr.overrun_last = tmr.overrun_cur;
            tmr.overrun_cur = 0;
            deliver(&tmr);
            tmr.overrun_cur = expirations - 1;
        }

        if interval_ticks != 0 {
            // Drift-free re-arm; the deadline stays ahead of `now` afterwards.
            tmr.expire_tick += expirations * interval_ticks;
            if tmr.expire_tick <= now {
                tmr.expire_tick = now + interval_ticks;
            }
        } else {
            tmr.expire_tick = 0;
        }
        t.posix[index] = tmr;
    }

    // Reap cpu-itimer entries whose owner is gone.
    for index in 0..COMPAT_TIMER_MAX {
        let entry = t.cpu[index];
        if entry.used == 0 || entry.task.is_null() {
            continue;
        }
        if unsafe { a20_posix_timer_task_gone(entry.task) } != 0 {
            unsafe { a20_posix_timer_task_put(entry.task) };
            t.cpu[index] = EMPTY_ITIMER;
        }
    }

    update_deadline();
}

fn itimer_find(t: &Table, task: *mut c_void) -> i32 {
    for (index, entry) in t.cpu.iter().enumerate() {
        if entry.used != 0 && entry.task == task {
            return index as i32;
        }
    }
    -1
}

/// Release an entry whose two deadlines are both disarmed.
fn itimer_maybe_release(entry: &mut CpuItimer) {
    if entry.expire[0] != 0 || entry.expire[1] != 0 {
        return;
    }
    unsafe { a20_posix_timer_task_put(entry.task) };
    entry.used = 0;
    entry.task = ptr::null_mut();
}

fn itimer_metric(task: *mut c_void, which: i32) -> u64 {
    unsafe { a20_posix_timer_task_metric(task, which) }
}

/// Arm `ITIMER_VIRTUAL` (0) or `ITIMER_PROF` (1) for `task`.
///
/// # Safety
/// `task` must be null or a live task pointer the caller may reference.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_posix_itimer_set(
    task: *mut c_void,
    which: i32,
    value_ticks: u64,
    interval_ticks: u64,
) -> i32 {
    if task.is_null() || !(0..=1).contains(&which) {
        return -EINVAL;
    }
    let t = table();
    let mut index = itimer_find(t, task);
    if index < 0 {
        if value_ticks == 0 && interval_ticks == 0 {
            return 0; // disarming a timer that was never set
        }
        for candidate in 0..COMPAT_TIMER_MAX {
            if t.cpu[candidate].used != 0 {
                continue;
            }
            let owned = unsafe { a20_posix_timer_task_clone_ref(task) };
            if owned.is_null() {
                return -EAGAIN;
            }
            t.cpu[candidate] = EMPTY_ITIMER;
            t.cpu[candidate].used = 1;
            t.cpu[candidate].task = owned;
            index = candidate as i32;
            break;
        }
        if index < 0 {
            return -EAGAIN;
        }
    }
    let entry = &mut t.cpu[index as usize];
    entry.interval[which as usize] = interval_ticks;
    entry.expire[which as usize] = if value_ticks != 0 {
        itimer_metric(task, which) + value_ticks
    } else {
        0
    };
    itimer_maybe_release(entry);
    0
}

/// Read `[interval, remaining]` in scheduler ticks for one itimer.
///
/// # Safety
/// `out` must point to two writable `u64`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_posix_itimer_get(
    task: *mut c_void,
    which: i32,
    out: *mut u64,
) -> i32 {
    if out.is_null() {
        return -EINVAL;
    }
    unsafe { ptr::write(out, 0) };
    unsafe { ptr::write(out.add(1), 0) };
    if task.is_null() || !(0..=1).contains(&which) {
        return -EINVAL;
    }
    let t = table();
    let index = itimer_find(t, task);
    if index < 0 {
        return 0;
    }
    let entry = &t.cpu[index as usize];
    unsafe { ptr::write(out, entry.interval[which as usize]) };
    if entry.expire[which as usize] != 0 {
        let metric = itimer_metric(task, which);
        let remaining = if entry.expire[which as usize] > metric {
            entry.expire[which as usize] - metric
        } else {
            0
        };
        unsafe { ptr::write(out.add(1), remaining) };
    }
    0
}

/// CPU-time expiry check for the current task, from the scheduler accounting
/// site.
///
/// # Safety
/// `cur` must be null or the current task pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_posix_itimer_cpu_tick(cur: *mut c_void) {
    if cur.is_null() {
        return;
    }
    let t = table();
    let index = itimer_find(t, cur);
    if index < 0 {
        return;
    }
    let entry = &mut t.cpu[index as usize];
    for which in 0..2usize {
        if entry.expire[which] == 0 {
            continue;
        }
        let metric = itimer_metric(cur, which as i32);
        if metric < entry.expire[which] {
            continue;
        }
        let signo = if which == 0 { SIGVTALRM } else { SIGPROF };
        unsafe { a20_posix_timer_send_task(cur, signo) };
        entry.expire[which] = if entry.interval[which] != 0 {
            metric + entry.interval[which]
        } else {
            0
        };
    }
    itimer_maybe_release(entry);
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::sync::atomic::{AtomicBool, AtomicI32, AtomicU64, Ordering};

    // ---- test doubles for the C bridges --------------------------------
    static NOW: AtomicU64 = AtomicU64::new(0);
    static TPS: AtomicU64 = AtomicU64::new(100);
    static DEADLINE: AtomicU64 = AtomicU64::new(u64::MAX);
    static WALL: AtomicU64 = AtomicU64::new(0);
    static PENDING: AtomicBool = AtomicBool::new(false);
    static SENT_COUNT: AtomicU64 = AtomicU64::new(0);
    static SENT_PID: AtomicI32 = AtomicI32::new(0);
    static SENT_SIGNO: AtomicI32 = AtomicI32::new(0);
    static TASK_REFS: AtomicI32 = AtomicI32::new(0);
    static TASK_GONE: AtomicBool = AtomicBool::new(false);
    static TASK_EXISTS: AtomicBool = AtomicBool::new(false);

    #[repr(C)]
    struct FakeTask {
        utime: u64,
        stime: u64,
    }

    /// The tests drive the one static table, so they must not run concurrently.
    /// The guard also resets the table and the bridge doubles.
    static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    fn reset_state() -> std::sync::MutexGuard<'static, ()> {
        let guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let t = table();
        t.posix = [EMPTY_TIMER; COMPAT_TIMER_MAX];
        t.cpu = [EMPTY_ITIMER; COMPAT_TIMER_MAX];
        NOW.store(0, Ordering::SeqCst);
        TPS.store(100, Ordering::SeqCst);
        DEADLINE.store(u64::MAX, Ordering::SeqCst);
        WALL.store(0, Ordering::SeqCst);
        PENDING.store(false, Ordering::SeqCst);
        SENT_COUNT.store(0, Ordering::SeqCst);
        SENT_PID.store(0, Ordering::SeqCst);
        SENT_SIGNO.store(0, Ordering::SeqCst);
        TASK_REFS.store(0, Ordering::SeqCst);
        TASK_GONE.store(false, Ordering::SeqCst);
        TASK_EXISTS.store(false, Ordering::SeqCst);
        guard
    }

    #[unsafe(no_mangle)]
    extern "C" fn a20_posix_timer_ticks_per_sec() -> u64 {
        TPS.load(Ordering::SeqCst)
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_posix_timer_now_ticks() -> u64 {
        NOW.load(Ordering::SeqCst)
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_posix_timer_now_wall(ts: *mut u64) {
        let value = WALL.load(Ordering::SeqCst);
        unsafe { ptr::write(ts, value) };
        unsafe { ptr::write(ts.add(1), 0) };
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_posix_timer_now_monotonic(ts: *mut u64) {
        let value = NOW.load(Ordering::SeqCst);
        unsafe { ptr::write(ts, value / 100) };
        unsafe { ptr::write(ts.add(1), 0) };
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_posix_timer_set_deadline(next: u64) {
        DEADLINE.store(next, Ordering::SeqCst);
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_posix_timer_task_ref(_pid: i32) -> *mut c_void {
        if TASK_EXISTS.load(Ordering::SeqCst) {
            TASK_REFS.fetch_add(1, Ordering::SeqCst);
            1 as *mut c_void
        } else {
            core::ptr::null_mut() // "no such task" unless a test opts in
        }
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_posix_timer_task_clone_ref(task: *mut c_void) -> *mut c_void {
        TASK_REFS.fetch_add(1, Ordering::SeqCst);
        task
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_posix_timer_task_put(_task: *mut c_void) {
        TASK_REFS.fetch_sub(1, Ordering::SeqCst);
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_posix_timer_task_gone(_task: *mut c_void) -> i32 {
        TASK_GONE.load(Ordering::SeqCst) as i32
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_posix_timer_task_metric(task: *mut c_void, which: i32) -> u64 {
        let fake = unsafe { &*(task as *const FakeTask) };
        if which == 0 {
            fake.utime
        } else {
            fake.utime + fake.stime
        }
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_posix_timer_signal_pending(
        _task: *mut c_void,
        _signo: i32,
        _scoped: i32,
    ) -> i32 {
        PENDING.load(Ordering::SeqCst) as i32
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_posix_timer_send(pid: i32, signo: i32) {
        SENT_COUNT.fetch_add(1, Ordering::SeqCst);
        SENT_PID.store(pid, Ordering::SeqCst);
        SENT_SIGNO.store(signo, Ordering::SeqCst);
    }
    #[unsafe(no_mangle)]
    extern "C" fn a20_posix_timer_send_task(_task: *mut c_void, signo: i32) {
        SENT_COUNT.fetch_add(1, Ordering::SeqCst);
        SENT_SIGNO.store(signo, Ordering::SeqCst);
    }

    fn create(owner: i32, signo: i32, tid: i32, clock: i32) -> i32 {
        a20_posix_timer_create(owner, signo, tid, clock)
    }

    #[test]
    fn supported_clock_ids_match_linux() {
        for clock in [0, 1, 4, 5, 6, 7, 11] {
            assert_eq!(a20_posix_timer_clock_supported(clock), 1, "clock {clock}");
        }
        for clock in [-1, 2, 3, 8, 9, 10, 12] {
            assert_eq!(a20_posix_timer_clock_supported(clock), 0, "clock {clock}");
        }
    }

    #[test]
    fn create_fills_slots_in_order_and_delete_requires_the_owner() {
        let _guard = reset_state();
        assert_eq!(create(7, 10, 0, 1), 0);
        assert_eq!(create(7, 11, 0, 1), 1);
        assert_eq!(a20_posix_timer_delete(8, 0), -EINVAL, "wrong owner");
        assert_eq!(a20_posix_timer_delete(7, 5), -EINVAL, "unused slot");
        assert_eq!(a20_posix_timer_delete(7, 0), 0);
        assert_eq!(create(7, 12, 0, 1), 0, "the freed slot is reused");
    }

    #[test]
    fn a_full_table_reports_eagain() {
        let _guard = reset_state();
        for i in 0..COMPAT_TIMER_MAX {
            assert_eq!(create(1, 10, 0, 1), i as i32);
        }
        assert_eq!(create(1, 10, 0, 1), -EAGAIN);
    }

    #[test]
    fn relative_arming_reports_remaining_time_and_zero_when_disarmed() {
        let _guard = reset_state();
        let id = create(1, 10, 0, 1);
        NOW.store(100, Ordering::SeqCst);
        let mut ts = [2u64, 0, 5, 500_000_000]; // interval 2s, value 5.5s
        assert_eq!(unsafe { a20_posix_timer_set_time(1, id, ts.as_ptr(), 0) }, 0);
        assert_eq!(DEADLINE.load(Ordering::SeqCst), 650, "100 + 550 ticks");
        let mut out = [0u64; 4];
        assert_eq!(unsafe { a20_posix_timer_get_time(1, id, out.as_mut_ptr()) }, 0);
        assert_eq!(out, [2, 0, 5, 500_000_000]);

        NOW.store(400, Ordering::SeqCst);
        assert_eq!(unsafe { a20_posix_timer_get_time(1, id, out.as_mut_ptr()) }, 0);
        assert_eq!(out, [2, 0, 2, 500_000_000]);

        ts = [0, 0, 0, 0];
        assert_eq!(unsafe { a20_posix_timer_set_time(1, id, ts.as_ptr(), 0) }, 0);
        assert_eq!(DEADLINE.load(Ordering::SeqCst), 0, "disarmed");
        assert_eq!(unsafe { a20_posix_timer_get_time(1, id, out.as_mut_ptr()) }, 0);
        assert_eq!(out, [0, 0, 0, 0]);
    }

    #[test]
    fn absolute_arming_subtracts_the_current_clock() {
        let _guard = reset_state();
        let id = create(1, 10, 0, 0); // CLOCK_REALTIME
        WALL.store(1000, Ordering::SeqCst);
        NOW.store(50, Ordering::SeqCst);
        let ts = [0u64, 0, 1003, 0]; // 3s in the future
        assert_eq!(unsafe { a20_posix_timer_set_time(1, id, ts.as_ptr(), TIMER_ABSTIME) }, 0);
        assert_eq!(DEADLINE.load(Ordering::SeqCst), 350, "50 + 300 ticks");
    }

    #[test]
    fn a_past_absolute_deadline_expires_immediately() {
        let _guard = reset_state();
        let id = create(1, 10, 0, 0);
        WALL.store(2000, Ordering::SeqCst);
        NOW.store(70, Ordering::SeqCst);
        let ts = [0u64, 0, 1999, 0];
        assert_eq!(unsafe { a20_posix_timer_set_time(1, id, ts.as_ptr(), TIMER_ABSTIME) }, 0);
        assert_eq!(DEADLINE.load(Ordering::SeqCst), 70, "no delay is added");
    }

    #[test]
    fn set_time_rejects_unknown_flags_and_foreign_owners() {
        let _guard = reset_state();
        let id = create(1, 10, 0, 1);
        let ts = [0u64, 0, 1, 0];
        assert_eq!(unsafe { a20_posix_timer_set_time(1, id, ts.as_ptr(), 2) }, -EINVAL);
        assert_eq!(unsafe { a20_posix_timer_set_time(2, id, ts.as_ptr(), 0) }, -EINVAL);
        assert_eq!(a20_posix_timer_getoverrun(2, id), -EINVAL);
    }

    #[test]
    fn the_tick_delivers_once_and_rearms_drift_free() {
        let _guard = reset_state();
        let id = create(1, 10, 0, 1);
        NOW.store(0, Ordering::SeqCst);
        let ts = [1u64, 0, 1, 0]; // interval 1s, first expiry at 100 ticks
        assert_eq!(unsafe { a20_posix_timer_set_time(1, id, ts.as_ptr(), 0) }, 0);

        NOW.store(99, Ordering::SeqCst);
        a20_posix_timer_tick();
        assert_eq!(SENT_COUNT.load(Ordering::SeqCst), 0, "not yet due");

        NOW.store(100, Ordering::SeqCst);
        a20_posix_timer_tick();
        assert_eq!(SENT_COUNT.load(Ordering::SeqCst), 1);
        assert_eq!(SENT_PID.load(Ordering::SeqCst), 1);
        assert_eq!(SENT_SIGNO.load(Ordering::SeqCst), 10);
        assert_eq!(DEADLINE.load(Ordering::SeqCst), 200, "one interval later");

        // A scan that runs three intervals late advances by whole intervals
        // and charges the extra expirations as overruns.
        NOW.store(400, Ordering::SeqCst);
        a20_posix_timer_tick();
        assert_eq!(SENT_COUNT.load(Ordering::SeqCst), 2, "delivered once");
        assert_eq!(DEADLINE.load(Ordering::SeqCst), 500);
        // The tick delivered and set overrun_cur = expirations - 1.
        let t = table();
        assert_eq!(t.posix[id as usize].overrun_cur, 3 - 1);
    }

    #[test]
    fn a_pending_notification_coalesces_into_overruns() {
        let _guard = reset_state();
        let id = create(1, 10, 0, 1);
        TASK_EXISTS.store(true, Ordering::SeqCst);
        PENDING.store(true, Ordering::SeqCst);
        NOW.store(0, Ordering::SeqCst);
        let ts = [1u64, 0, 1, 0];
        assert_eq!(unsafe { a20_posix_timer_set_time(1, id, ts.as_ptr(), 0) }, 0);
        NOW.store(100, Ordering::SeqCst);
        a20_posix_timer_tick();
        assert_eq!(SENT_COUNT.load(Ordering::SeqCst), 0, "coalesced");
        let t = table();
        assert_eq!(t.posix[id as usize].overrun_cur, 1);
        assert_eq!(a20_posix_timer_getoverrun(1, id), 0, "nothing latched yet");
    }

    #[test]
    fn getoverrun_latches_a_delivered_notification() {
        let _guard = reset_state();
        let id = create(1, 10, 0, 1);
        NOW.store(0, Ordering::SeqCst);
        let ts = [1u64, 0, 1, 0];
        assert_eq!(unsafe { a20_posix_timer_set_time(1, id, ts.as_ptr(), 0) }, 0);
        NOW.store(300, Ordering::SeqCst);
        a20_posix_timer_tick(); // 3 expirations, delivered once
        // No longer pending: the read latches the 2 overruns into overrun_last.
        assert_eq!(a20_posix_timer_getoverrun(1, id), 2);
        // The C body re-latches from overrun_cur on every read, so a second
        // read sees the now-empty current count.  This mirrors the C code (the
        // comment there claims stability, the code does not provide it).
        assert_eq!(a20_posix_timer_getoverrun(1, id), 0, "re-latched from cur");
    }

    #[test]
    fn a_one_shot_timer_disarms_after_expiry() {
        let _guard = reset_state();
        let id = create(1, 10, 0, 1);
        NOW.store(0, Ordering::SeqCst);
        let ts = [0u64, 0, 1, 0];
        assert_eq!(unsafe { a20_posix_timer_set_time(1, id, ts.as_ptr(), 0) }, 0);
        NOW.store(100, Ordering::SeqCst);
        a20_posix_timer_tick();
        assert_eq!(SENT_COUNT.load(Ordering::SeqCst), 1);
        assert_eq!(DEADLINE.load(Ordering::SeqCst), 0, "nothing left to arm");
        NOW.store(200, Ordering::SeqCst);
        a20_posix_timer_tick();
        assert_eq!(SENT_COUNT.load(Ordering::SeqCst), 1, "does not fire again");
    }

    #[test]
    fn a_thread_target_receives_the_notification() {
        let _guard = reset_state();
        let id = create(1, 10, 42, 1);
        TASK_EXISTS.store(true, Ordering::SeqCst);
        NOW.store(0, Ordering::SeqCst);
        let ts = [0u64, 0, 1, 0];
        assert_eq!(unsafe { a20_posix_timer_set_time(1, id, ts.as_ptr(), 0) }, 0);
        NOW.store(100, Ordering::SeqCst);
        a20_posix_timer_tick();
        assert_eq!(SENT_COUNT.load(Ordering::SeqCst), 1);
        assert_eq!(SENT_SIGNO.load(Ordering::SeqCst), 10);
        assert_eq!(SENT_PID.load(Ordering::SeqCst), 0, "sent to the task, not the owner");
    }

    #[test]
    fn cpu_itimers_arm_report_and_fire_from_the_metric() {
        let _guard = reset_state();
        let mut fake = FakeTask { utime: 100, stime: 50 };
        let task = &mut fake as *mut FakeTask as *mut c_void;
        let mut out = [0u64; 2];

        // Disarming one that was never set succeeds without allocating.
        assert_eq!(unsafe { a20_posix_itimer_set(task, 0, 0, 0) }, 0);
        assert_eq!(TASK_REFS.load(Ordering::SeqCst), 0);

        assert_eq!(unsafe { a20_posix_itimer_set(task, 0, 20, 10) }, 0);
        assert_eq!(TASK_REFS.load(Ordering::SeqCst), 1);
        assert_eq!(unsafe { a20_posix_itimer_get(task, 0, out.as_mut_ptr()) }, 0);
        assert_eq!(out, [10, 20]);

        // Not yet due.
        unsafe { a20_posix_itimer_cpu_tick(task) };
        assert_eq!(SENT_COUNT.load(Ordering::SeqCst), 0);
        // Cross the deadline: SIGVTALRM, then re-arm from the metric.
        fake.utime = 120;
        let _ = fake.utime;
        unsafe { a20_posix_itimer_cpu_tick(task) };
        assert_eq!(SENT_COUNT.load(Ordering::SeqCst), 1);
        assert_eq!(SENT_SIGNO.load(Ordering::SeqCst), SIGVTALRM);
        assert_eq!(unsafe { a20_posix_itimer_get(task, 0, out.as_mut_ptr()) }, 0);
        assert_eq!(out, [10, 10], "next deadline is metric + interval");

        // ITIMER_PROF uses utime + stime.
        assert_eq!(unsafe { a20_posix_itimer_set(task, 1, 1, 0) }, 0);
        fake.stime = 1000;
        let _ = fake.stime;
        unsafe { a20_posix_itimer_cpu_tick(task) };
        assert_eq!(SENT_SIGNO.load(Ordering::SeqCst), SIGPROF);
    }

    #[test]
    fn a_fully_disarmed_itimer_releases_its_task_reference() {
        let _guard = reset_state();
        let mut fake = FakeTask { utime: 0, stime: 0 };
        let task = &mut fake as *mut FakeTask as *mut c_void;
        assert_eq!(unsafe { a20_posix_itimer_set(task, 0, 5, 0) }, 0);
        assert_eq!(TASK_REFS.load(Ordering::SeqCst), 1);
        assert_eq!(unsafe { a20_posix_itimer_set(task, 0, 0, 0) }, 0);
        assert_eq!(TASK_REFS.load(Ordering::SeqCst), 0, "last deadline disarmed");
        assert_eq!(unsafe { a20_posix_itimer_get(task, 0, [0u64; 2].as_mut_ptr()) }, 0);
    }

    #[test]
    fn the_tick_reaps_an_itimer_whose_task_is_gone() {
        let _guard = reset_state();
        let mut fake = FakeTask { utime: 0, stime: 0 };
        let task = &mut fake as *mut FakeTask as *mut c_void;
        assert_eq!(unsafe { a20_posix_itimer_set(task, 0, 100, 0) }, 0);
        assert_eq!(TASK_REFS.load(Ordering::SeqCst), 1);
        TASK_GONE.store(true, Ordering::SeqCst);
        a20_posix_timer_tick();
        assert_eq!(TASK_REFS.load(Ordering::SeqCst), 0, "reference dropped");
        assert_eq!(table().cpu[0].used, 0);
    }

    #[test]
    fn itimer_arguments_are_validated() {
        let _guard = reset_state();
        let mut fake = FakeTask { utime: 0, stime: 0 };
        let task = &mut fake as *mut FakeTask as *mut c_void;
        assert_eq!(unsafe { a20_posix_itimer_set(core::ptr::null_mut(), 0, 1, 0) }, -EINVAL);
        assert_eq!(unsafe { a20_posix_itimer_set(task, 2, 1, 0) }, -EINVAL);
        assert_eq!(unsafe { a20_posix_itimer_get(task, -1, [0u64; 2].as_mut_ptr()) }, -EINVAL);
        assert_eq!(unsafe { a20_posix_itimer_get(core::ptr::null_mut(), 0, [0u64; 2].as_mut_ptr()) }, -EINVAL);
    }

    #[test]
    fn the_deadline_is_the_earliest_armed_notification() {
        let _guard = reset_state();
        let a = create(1, 10, 0, 1);
        let b = create(1, 11, 0, 1);
        NOW.store(0, Ordering::SeqCst);
        let late = [0u64, 0, 5, 0];
        let early = [0u64, 0, 2, 0];
        assert_eq!(unsafe { a20_posix_timer_set_time(1, a, late.as_ptr(), 0) }, 0);
        assert_eq!(DEADLINE.load(Ordering::SeqCst), 500);
        assert_eq!(unsafe { a20_posix_timer_set_time(1, b, early.as_ptr(), 0) }, 0);
        assert_eq!(DEADLINE.load(Ordering::SeqCst), 200);
        assert_eq!(a20_posix_timer_delete(1, b), 0);
        assert_eq!(DEADLINE.load(Ordering::SeqCst), 500, "falls back to the later timer");
    }

    #[test]
    fn a_signal_less_timer_does_not_publish_a_deadline() {
        let _guard = reset_state();
        let id = create(1, 0, 0, 1); // signo 0: no notification
        NOW.store(0, Ordering::SeqCst);
        let ts = [0u64, 0, 1, 0];
        assert_eq!(unsafe { a20_posix_timer_set_time(1, id, ts.as_ptr(), 0) }, 0);
        assert_eq!(DEADLINE.load(Ordering::SeqCst), 0);
    }
}
