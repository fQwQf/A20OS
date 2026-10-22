#![no_std]

//! OOM victim selection and the reclaim escalation policy.
//!
//! The one thing this module must never do is free memory that something still
//! owns.  It therefore never touches a frame: reclaim goes through the normal
//! page-cache, slab and swap levers, and killing a victim is a request for the
//! ordinary proc-exit/mm-teardown path.  What Rust owns is the policy around
//! that:
//!
//!   * which task is the victim -- the highest `oom_score_adj`, minus 100 for a
//!     task with no address space, pid 2 and below and the idle task skipped,
//!     ties broken toward the higher pid;
//!   * the escalation order in `oom_try_reclaim`: an in-progress guard, the
//!     free-page watermark, the kill cooldown, then swap-before-kill, then the
//!     kill itself;
//!   * the background reclaimer's ladder -- spare slab pages, clean page cache,
//!     then paced swap -- with the backoff when a pass frees nothing;
//!   * the counters the shutdown audit reads.
//!
//! C keeps the task-list walk and its per-task park_lock sampling, the swap-out
//! page walk, the levers themselves, the kill (log + force-exit) and the
//! scheduler calls.

use core::ffi::c_void;
use core::sync::atomic::{AtomicI32, AtomicUsize, Ordering};

const PAGE_SIZE: usize = 4096;

const EINVAL: i32 = -22;

/// One task sample, taken under that task's park_lock by the C bridge.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct TaskSnap {
    pid: i32,
    state: i32,
    score: i32,
    has_mm: i32,
    is_idle: i32,
}

/// The policy knobs and enum values the module must not hardcode.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Params {
    min_free_pages: usize,
    max_swap_reclaim: i32,
    cooldown_ticks: u64,
    kswapd_interval_ticks: u64,
    proc_unused: i32,
    proc_zombie: i32,
    kswapd_swap_batch: i32,
}

/// `oom_stats_t` from mm/oom.h.  `unsigned long` is pointer-sized; the
/// explicit padding field keeps the layout identical on 64-bit.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Stats {
    kills: usize,
    last_kill_tick: usize,
    last_victim_pid: i32,
    last_victim_score: i32,
    free_pages_at_kill: usize,
    free_pages_now: usize,
    in_progress: i32,
    reserved: i32,
    kswapd_passes: usize,
    kswapd_pages_freed: usize,
    kswapd_last_pass_tick: usize,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 72] = [(); core::mem::size_of::<Stats>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 20] = [(); core::mem::size_of::<TaskSnap>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 48] = [(); core::mem::size_of::<Params>()];

static IN_PROGRESS: AtomicI32 = AtomicI32::new(0);
// Ticks come from a 64-bit counter, but a 32-bit target has no 64-bit atomics.
// Only wrapping tick *differences* are ever computed from these, so storing the
// native word and comparing truncated values is correct as long as the cooldown
// and the kswapd interval stay far below the wrap period.
static LAST_KILL_TICK: AtomicUsize = AtomicUsize::new(0);
static KILLS: AtomicUsize = AtomicUsize::new(0);
static LAST_VICTIM_PID: AtomicI32 = AtomicI32::new(0);
static LAST_VICTIM_SCORE: AtomicI32 = AtomicI32::new(0);
static FREE_AT_KILL: AtomicUsize = AtomicUsize::new(0);
static KSWAPD_PASSES: AtomicUsize = AtomicUsize::new(0);
static KSWAPD_FREED: AtomicUsize = AtomicUsize::new(0);
static KSWAPD_LAST_TICK: AtomicUsize = AtomicUsize::new(0);

unsafe extern "C" {
    fn a20_oom_params(out: *mut Params);
    fn a20_oom_task_first() -> *mut c_void;
    fn a20_oom_task_next(t: *mut c_void) -> *mut c_void;
    fn a20_oom_task_snapshot(t: *mut c_void, out: *mut TaskSnap) -> i32;
    fn a20_oom_free_pages() -> usize;
    fn a20_oom_now() -> u64;
    fn a20_oom_swap_victim_by_pid(pid: i32, target: i32) -> i32;
    fn a20_oom_swap_task(t: *mut c_void, limit: i32, start: usize, end: usize) -> i32;
    fn a20_oom_find_get(pid: i32) -> *mut c_void;
    fn a20_oom_put(t: *mut c_void);
    fn a20_oom_kill(t: *mut c_void);
    fn a20_oom_sleep_until(tick: u64);
    fn a20_oom_yield();
    fn a20_oom_kswapd_started();
    fn page_cache_drop_clean() -> usize;
    fn slab_reclaim_spare() -> usize;
    fn a20_oom_user_va_limit() -> usize;
}

/// The victim: highest adjusted score, then the higher pid.  The idle task,
/// `pid <= 2` and tasks that are not runnable are never candidates.
unsafe fn pick_victim_pid(p: &Params) -> i32 {
    let mut victim_pid: i32 = -1;
    let mut best_score: i32 = -1;
    let mut t = unsafe { a20_oom_task_first() };
    while !t.is_null() {
        let mut s = TaskSnap::default();
        unsafe { a20_oom_task_snapshot(t, &mut s) };
        if s.is_idle != 0
            || s.state == p.proc_unused
            || s.state == p.proc_zombie
            || s.pid <= 2
        {
            t = unsafe { a20_oom_task_next(t) };
            continue;
        }
        // A task with no address space gives back less than one that has one.
        let score = if s.has_mm != 0 { s.score } else { s.score - 100 };
        if score > best_score || (score == best_score && s.pid > victim_pid) {
            best_score = score;
            victim_pid = s.pid;
        }
        t = unsafe { a20_oom_task_next(t) };
    }
    victim_pid
}

#[no_mangle]
pub unsafe extern "C" fn oom_try_reclaim() -> i32 {
    let mut p = Params::default();
    unsafe { a20_oom_params(&mut p) };

    if IN_PROGRESS.load(Ordering::Relaxed) != 0 {
        return 0;
    }
    if unsafe { a20_oom_free_pages() } >= p.min_free_pages {
        return 0;
    }
    let now = unsafe { a20_oom_now() };
    if (now as usize).wrapping_sub(LAST_KILL_TICK.load(Ordering::Relaxed))
        < p.cooldown_ticks as usize
    {
        return 0;
    }

    IN_PROGRESS.store(1, Ordering::Relaxed);

    // Swap first: it keeps every task alive.
    if unsafe { a20_oom_swap_victim_by_pid(pick_victim_pid(&p), p.max_swap_reclaim) } > 0
        && unsafe { a20_oom_free_pages() } >= p.min_free_pages
    {
        IN_PROGRESS.store(0, Ordering::Relaxed);
        return 1;
    }

    let victim_pid = unsafe { pick_victim_pid(&p) };
    if victim_pid <= 0 {
        IN_PROGRESS.store(0, Ordering::Relaxed);
        return 0;
    }
    let victim = unsafe { a20_oom_find_get(victim_pid) };
    if victim.is_null() {
        IN_PROGRESS.store(0, Ordering::Relaxed);
        return 0;
    }

    let mut s = TaskSnap::default();
    unsafe { a20_oom_task_snapshot(victim, &mut s) };
    LAST_KILL_TICK.store(now as usize, Ordering::Relaxed);
    KILLS.fetch_add(1, Ordering::Relaxed);
    LAST_VICTIM_PID.store(s.pid, Ordering::Relaxed);
    LAST_VICTIM_SCORE.store(s.score, Ordering::Relaxed);
    FREE_AT_KILL.store(unsafe { a20_oom_free_pages() }, Ordering::Relaxed);

    unsafe { a20_oom_kill(victim) };
    unsafe { a20_oom_put(victim) };

    IN_PROGRESS.store(0, Ordering::Relaxed);
    1
}

#[no_mangle]
pub unsafe extern "C" fn oom_swap_reclaim_pages(target_pages: i32) -> i32 {
    if IN_PROGRESS.load(Ordering::Relaxed) != 0 {
        return 0;
    }
    let mut p = Params::default();
    unsafe { a20_oom_params(&mut p) };
    unsafe { a20_oom_swap_victim_by_pid(pick_victim_pid(&p), target_pages) }
}

#[no_mangle]
pub unsafe extern "C" fn oom_swap_reclaim_range(
    t: *mut c_void,
    start: usize,
    len: usize,
) -> i32 {
    if t.is_null() || len == 0 || start & (PAGE_SIZE - 1) != 0 || len & (PAGE_SIZE - 1) != 0 {
        return EINVAL;
    }
    let limit = unsafe { a20_oom_user_va_limit() };
    if start >= limit || len > limit - start {
        return EINVAL;
    }
    let pages = len / PAGE_SIZE;
    let pages = if pages > i32::MAX as usize {
        i32::MAX
    } else {
        pages as i32
    };
    unsafe { a20_oom_swap_task(t, pages, start, start + len) }
}

#[no_mangle]
pub unsafe extern "C" fn oom_kswapd_thread() -> ! {
    let mut p = Params::default();
    unsafe { a20_oom_params(&mut p) };
    unsafe { a20_oom_kswapd_started() };
    loop {
        let now = unsafe { a20_oom_now() };
        if unsafe { a20_oom_free_pages() } >= p.min_free_pages {
            unsafe { a20_oom_sleep_until(now.wrapping_add(p.kswapd_interval_ticks)) };
            continue;
        }
        KSWAPD_PASSES.fetch_add(1, Ordering::Relaxed);
        KSWAPD_LAST_TICK.store(now as usize, Ordering::Relaxed);
        // Escalating levers: spare slab pages, clean page cache, then paced
        // swap.  Each is cheaper than the next.
        let mut freed = unsafe { slab_reclaim_spare() } / PAGE_SIZE;
        freed += unsafe { page_cache_drop_clean() };
        if unsafe { a20_oom_free_pages() } < p.min_free_pages {
            let n = unsafe { a20_oom_swap_victim_by_pid(pick_victim_pid(&p), p.kswapd_swap_batch) };
            if n > 0 {
                freed += n as usize;
            }
        }
        KSWAPD_FREED.fetch_add(freed, Ordering::Relaxed);
        // Yield so consumers can run, but back off a full interval when the
        // pass freed nothing rather than spinning.
        if freed == 0 {
            unsafe { a20_oom_sleep_until(now.wrapping_add(p.kswapd_interval_ticks)) };
        } else {
            unsafe { a20_oom_yield() };
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn oom_get_stats(out: *mut Stats) {
    if out.is_null() {
        return;
    }
    unsafe {
        *out = Stats {
            kills: KILLS.load(Ordering::Relaxed),
            last_kill_tick: LAST_KILL_TICK.load(Ordering::Relaxed),
            last_victim_pid: LAST_VICTIM_PID.load(Ordering::Relaxed),
            last_victim_score: LAST_VICTIM_SCORE.load(Ordering::Relaxed),
            free_pages_at_kill: FREE_AT_KILL.load(Ordering::Relaxed),
            free_pages_now: a20_oom_free_pages(),
            in_progress: IN_PROGRESS.load(Ordering::Relaxed),
            reserved: 0,
            kswapd_passes: KSWAPD_PASSES.load(Ordering::Relaxed),
            kswapd_pages_freed: KSWAPD_FREED.load(Ordering::Relaxed),
            kswapd_last_pass_tick: KSWAPD_LAST_TICK.load(Ordering::Relaxed),
        }
    };
}
