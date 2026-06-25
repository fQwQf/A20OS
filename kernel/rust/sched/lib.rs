#![no_std]

mod ffi;

use a20rust_support::lock::{raw_irqsave_lock, IrqSaveSpinLock};
use core::ptr;
use core::sync::atomic::{AtomicU64, Ordering};

const MAX_CPUS: usize = 32;

#[derive(Clone, Copy)]
struct RunQueue {
    head: [*mut ffi::Task; ffi::SCHED_LEVELS],
    tail: [*mut ffi::Task; ffi::SCHED_LEVELS],
    bitmap: u32,
    nr_running: u32,
}

impl RunQueue {
    const fn new() -> Self {
        Self {
            head: [ptr::null_mut(); ffi::SCHED_LEVELS],
            tail: [ptr::null_mut(); ffi::SCHED_LEVELS],
            bitmap: 0,
            nr_running: 0,
        }
    }
}

unsafe impl Send for RunQueue {}

static RUNQS: [IrqSaveSpinLock<RunQueue>; MAX_CPUS] =
    [const { IrqSaveSpinLock::new(RunQueue::new()) }; MAX_CPUS];
static NEXT_WAKE_SCAN: AtomicU64 = AtomicU64::new(ffi::SCHED_NO_DEADLINE);
static NEXT_ALARM_SCAN: AtomicU64 = AtomicU64::new(ffi::SCHED_NO_DEADLINE);

#[inline]
fn nr_cpus() -> usize {
    let n = unsafe { ffi::a20_sched_nr_cpus() as usize };
    if n == 0 { 1 } else { core::cmp::min(n, MAX_CPUS) }
}

#[inline]
fn current_cpu() -> usize {
    let cpu = unsafe { ffi::a20_sched_current_cpu_id() as usize };
    if cpu < nr_cpus() { cpu } else { 0 }
}

#[inline]
fn ticks_per_sec() -> u64 {
    unsafe { ffi::a20_sched_ticks_per_sec() }
}

#[inline]
fn tick_interval() -> u64 {
    ticks_per_sec() / 100
}

#[inline]
fn min_timer_interval() -> u64 {
    let v = ticks_per_sec() / 10000;
    if v == 0 { 1 } else { v }
}

#[inline]
fn aging_threshold() -> u64 {
    let v = ticks_per_sec() / 20;
    if v == 0 { 1 } else { v }
}

#[inline]
fn yield_slice() -> u64 {
    ticks_per_sec() / 100
}

#[inline]
fn sched_level_clamp(level: i32) -> usize {
    if level < 0 {
        0
    } else if level as usize >= ffi::SCHED_LEVELS {
        ffi::SCHED_LEVELS - 1
    } else {
        level as usize
    }
}

#[inline]
fn sched_task_rt(task: *mut ffi::Task) -> bool {
    let policy = unsafe { ffi::a20_sched_task_sched_policy(task) };
    policy == ffi::SCHED_FIFO || policy == ffi::SCHED_RR
}

#[inline]
fn runq_cpu_for_task(task: *mut ffi::Task) -> usize {
    let cpu = unsafe { ffi::a20_sched_task_cpu_id(task) as usize };
    if cpu < nr_cpus() { cpu } else { current_cpu() }
}

fn runq_unlink_at(rq: &mut RunQueue, task: *mut ffi::Task, q: usize) {
    let prev = unsafe { ffi::a20_sched_task_rq_prev(task) };
    let next = unsafe { ffi::a20_sched_task_rq_next(task) };
    if !prev.is_null() {
        unsafe { ffi::a20_sched_task_set_rq_next(prev, next) };
    } else {
        rq.head[q] = next;
    }
    if !next.is_null() {
        unsafe { ffi::a20_sched_task_set_rq_prev(next, prev) };
    } else {
        rq.tail[q] = prev;
    }
    if rq.head[q].is_null() {
        rq.bitmap &= !(1u32 << q);
    }
    unsafe {
        ffi::a20_sched_task_set_rq_next(task, ptr::null_mut());
        ffi::a20_sched_task_set_rq_prev(task, ptr::null_mut());
    }
}

fn runq_append_at(rq: &mut RunQueue, task: *mut ffi::Task, q: usize) {
    let tail = rq.tail[q];
    unsafe {
        ffi::a20_sched_task_set_rq_next(task, ptr::null_mut());
        ffi::a20_sched_task_set_rq_prev(task, tail);
    }
    if !tail.is_null() {
        unsafe { ffi::a20_sched_task_set_rq_next(tail, task) };
    } else {
        rq.head[q] = task;
    }
    rq.tail[q] = task;
    rq.bitmap |= 1u32 << q;
}

fn promote_aged_locked(rq: &mut RunQueue, now: u64) {
    for q in 1..ffi::SCHED_LEVELS {
        let mut it = rq.head[q];
        while !it.is_null() {
            let next = unsafe { ffi::a20_sched_task_rq_next(it) };
            let ready_since = unsafe { ffi::a20_sched_task_ready_since(it) };
            let state = unsafe { ffi::a20_sched_task_state(it) };
            if !sched_task_rt(it)
                && state == ffi::PROC_READY
                && ready_since > 0
                && now.wrapping_sub(ready_since) >= aging_threshold()
            {
                runq_unlink_at(rq, it, q);
                unsafe {
                    ffi::a20_sched_task_set_sched_level(it, 0);
                    ffi::a20_sched_task_set_ready_since(it, now);
                }
                runq_append_at(rq, it, 0);
            }
            it = next;
        }
    }
}

fn note_deadline(slot: &AtomicU64, value: u64) -> bool {
    if value == 0 {
        return false;
    }
    let mut old = slot.load(Ordering::Relaxed);
    while value < old {
        match slot.compare_exchange_weak(old, value, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => return true,
            Err(cur) => old = cur,
        }
    }
    false
}

fn rearm_timer() {
    let now = unsafe { ffi::timer_get_ticks() };
    let delta = proc_next_timer_interval(now);
    unsafe { ffi::timer_set_interval(delta) };
}

fn scan_timers(now: u64) {
    let mut next_wake = ffi::SCHED_NO_DEADLINE;
    let mut next_alarm = ffi::SCHED_NO_DEADLINE;
    let mut sigalrm_pids = [0i32; 128];
    let mut sigalrm_count = 0usize;
    let mut wake_list = [ptr::null_mut(); 512];
    let mut wake_count = 0usize;

    unsafe {
        let _guard = raw_irqsave_lock(core::ptr::addr_of_mut!(ffi::proc_lock));
        let mut task = ffi::proc_first_task_locked();
        while !task.is_null() {
            if ffi::a20_sched_task_state(task) != ffi::PROC_UNUSED {
                let mut alarm = ffi::a20_sched_task_alarm_expire(task);
                if alarm > 0 {
                    if now >= alarm {
                        let interval = ffi::a20_sched_task_itimer_real_interval(task);
                        alarm = if interval != 0 { now + interval } else { 0 };
                        ffi::a20_sched_task_set_alarm_expire(task, alarm);
                        if sigalrm_count < sigalrm_pids.len() {
                            sigalrm_pids[sigalrm_count] = ffi::a20_sched_task_pid(task);
                            sigalrm_count += 1;
                        }
                    }
                    if alarm > 0 && alarm < next_alarm {
                        next_alarm = alarm;
                    }
                }

                let wake = ffi::a20_sched_task_wake_time(task);
                if ffi::a20_sched_task_state(task) == ffi::PROC_BLOCKED && wake > 0 {
                    if now >= wake {
                        ffi::a20_sched_task_set_wake_time(task, 0);
                        ffi::a20_sched_task_set_sched_level(task, 0);
                        ffi::a20_sched_task_set_state(task, ffi::PROC_READY);
                        ffi::a20_sched_task_set_exec_start(task, now);
                        if wake_count < wake_list.len() {
                            wake_list[wake_count] = task;
                            wake_count += 1;
                        } else {
                            let retry = now + min_timer_interval();
                            if retry < next_wake {
                                next_wake = retry;
                            }
                        }
                    } else if wake < next_wake {
                        next_wake = wake;
                    }
                }
            }
            task = ffi::proc_next_task_locked(task);
        }
    }

    for task in wake_list[..wake_count].iter().copied() {
        proc_runq_enqueue_locked(task);
    }
    for pid in sigalrm_pids[..sigalrm_count].iter().copied() {
        unsafe { ffi::signal_send(pid, ffi::SIGALRM) };
    }
    unsafe {
        ffi::a20_sched_posix_timer_tick();
        ffi::a20_sched_native_timer_tick();
    }
    NEXT_WAKE_SCAN.store(next_wake, Ordering::Relaxed);
    NEXT_ALARM_SCAN.store(next_alarm, Ordering::Relaxed);
}

#[no_mangle]
pub extern "C" fn proc_sched_runq_init() {
    for rq in &RUNQS[..MAX_CPUS] {
        let mut guard = rq.lock();
        *guard = RunQueue::new();
    }
    NEXT_WAKE_SCAN.store(ffi::SCHED_NO_DEADLINE, Ordering::Relaxed);
    NEXT_ALARM_SCAN.store(ffi::SCHED_NO_DEADLINE, Ordering::Relaxed);
}

#[no_mangle]
pub extern "C" fn proc_next_timer_interval(now: u64) -> u64 {
    let mut next = now + tick_interval();
    let wake = NEXT_WAKE_SCAN.load(Ordering::Relaxed);
    let alarm = NEXT_ALARM_SCAN.load(Ordering::Relaxed);
    if wake < next {
        next = wake;
    }
    if alarm < next {
        next = alarm;
    }
    if next <= now {
        return min_timer_interval();
    }
    let mut delta = next - now;
    let min_delta = min_timer_interval();
    if delta < min_delta {
        delta = min_delta;
    }
    delta
}

#[no_mangle]
pub extern "C" fn proc_sched_select_cpu(task: *mut ffi::Task) -> u32 {
    let current = current_cpu();
    if nr_cpus() <= 1 {
        return current as u32;
    }
    let mask = unsafe { ffi::a20_sched_task_cpu_mask(task) };
    if mask == 0 {
        return current as u32;
    }
    let cur = unsafe { ffi::proc_current() };
    if !task.is_null() && task == cur && current < 32 && (mask & (1u32 << current)) != 0 {
        return current as u32;
    }
    let mut best = current;
    let mut best_load = u32::MAX;
    for cpu in 0..nr_cpus().min(32) {
        if (mask & (1u32 << cpu)) == 0 {
            continue;
        }
        let load = RUNQS[cpu].lock().nr_running;
        if load < best_load {
            best = cpu;
            best_load = load;
        }
    }
    best as u32
}

#[no_mangle]
pub extern "C" fn proc_sched_select_cpu_locked(task: *mut ffi::Task) -> u32 {
    proc_sched_select_cpu(task)
}

#[no_mangle]
pub extern "C" fn proc_sched_kick_cpu(cpu: u32) {
    if (cpu as usize) >= nr_cpus() || cpu as usize == current_cpu() {
        return;
    }
    unsafe { ffi::a20_sched_send_reschedule(cpu) };
}

#[no_mangle]
pub extern "C" fn proc_set_wake_time(task: *mut ffi::Task, wake_time: u64) {
    if task.is_null() {
        return;
    }
    unsafe { ffi::a20_sched_task_set_wake_time(task, wake_time) };
    if note_deadline(&NEXT_WAKE_SCAN, wake_time) {
        rearm_timer();
    }
}

#[no_mangle]
pub extern "C" fn proc_set_alarm_expire(task: *mut ffi::Task, alarm_expire: u64) {
    if task.is_null() {
        return;
    }
    unsafe { ffi::a20_sched_task_set_alarm_expire(task, alarm_expire) };
    if note_deadline(&NEXT_ALARM_SCAN, alarm_expire) {
        rearm_timer();
    }
}

#[no_mangle]
pub extern "C" fn proc_runq_enqueue_locked(task: *mut ffi::Task) {
    if task.is_null() {
        return;
    }
    unsafe {
        if task == ffi::proc_idle_task() || ffi::a20_sched_task_state(task) != ffi::PROC_READY {
            return;
        }
    }
    let mut cpu = runq_cpu_for_task(task);
    if unsafe { ffi::a20_sched_task_on_rq(task) } != 0 {
        return;
    }
    if cpu >= nr_cpus() {
        cpu = current_cpu();
    }
    let mut rq = RUNQS[cpu].lock();
    if unsafe { ffi::a20_sched_task_on_rq(task) } != 0 {
        return;
    }
    let q = if sched_task_rt(task) {
        0
    } else {
        sched_level_clamp(unsafe { ffi::a20_sched_task_sched_level(task) })
    };
    let now = unsafe { ffi::timer_get_ticks() };
    unsafe {
        ffi::a20_sched_task_set_sched_level(task, q as i32);
        ffi::a20_sched_task_set_cpu_id(task, cpu as u32);
        ffi::a20_sched_task_set_ready_since(task, now);
    }
    runq_append_at(&mut rq, task, q);
    unsafe { ffi::a20_sched_task_set_on_rq(task, 1) };
    rq.nr_running = rq.nr_running.saturating_add(1);
}

#[no_mangle]
pub extern "C" fn proc_runq_remove_locked(task: *mut ffi::Task) {
    if task.is_null() || unsafe { ffi::a20_sched_task_on_rq(task) } == 0 {
        return;
    }
    let cpu = runq_cpu_for_task(task);
    let mut rq = RUNQS[cpu].lock();
    if unsafe { ffi::a20_sched_task_on_rq(task) } == 0 {
        return;
    }
    let q = sched_level_clamp(unsafe { ffi::a20_sched_task_sched_level(task) });
    runq_unlink_at(&mut rq, task, q);
    unsafe {
        ffi::a20_sched_task_set_on_rq(task, 0);
        ffi::a20_sched_task_set_ready_since(task, 0);
    }
    rq.nr_running = rq.nr_running.saturating_sub(1);
}

#[no_mangle]
pub extern "C" fn proc_runq_pick_locked() -> *mut ffi::Task {
    let cpu = current_cpu();
    let mut rq = RUNQS[cpu].lock();
    promote_aged_locked(&mut rq, unsafe { ffi::timer_get_ticks() });
    while rq.bitmap != 0 {
        let mut q = 0usize;
        while q < ffi::SCHED_LEVELS && (rq.bitmap & (1u32 << q)) == 0 {
            q += 1;
        }
        if q >= ffi::SCHED_LEVELS {
            break;
        }
        let mut task = rq.head[q];
        if task.is_null() {
            rq.bitmap &= !(1u32 << q);
            continue;
        }
        if q == 0 {
            let mut best: *mut ffi::Task = ptr::null_mut();
            let mut it = rq.head[q];
            while !it.is_null() {
                if sched_task_rt(it)
                    && (best.is_null()
                        || unsafe { ffi::a20_sched_task_priority(it) > ffi::a20_sched_task_priority(best) })
                {
                    best = it;
                }
                it = unsafe { ffi::a20_sched_task_rq_next(it) };
            }
            if !best.is_null() {
                task = best;
            }
        }
        runq_unlink_at(&mut rq, task, q);
        unsafe {
            ffi::a20_sched_task_set_on_rq(task, 0);
            ffi::a20_sched_task_set_ready_since(task, 0);
        }
        rq.nr_running = rq.nr_running.saturating_sub(1);
        let valid = unsafe {
            task != ffi::proc_idle_task()
                && ffi::a20_sched_task_state(task) == ffi::PROC_READY
                && ffi::a20_sched_task_kstack(task) != 0
                && ffi::a20_sched_task_cg_throttled(task) == 0
        };
        if valid {
            return task;
        }
    }
    ptr::null_mut()
}

#[no_mangle]
pub extern "C" fn sched_reap_zombies() {
    let mut to_reap = [ptr::null_mut(); 32];
    loop {
        let mut count = 0usize;
        unsafe {
            let _guard = raw_irqsave_lock(core::ptr::addr_of_mut!(ffi::proc_lock));
            let current = ffi::proc_current();
            let idle = ffi::proc_idle_task();
            let mut task = ffi::proc_first_task_locked();
            while !task.is_null() {
                if task != idle
                    && task != current
                    && ffi::a20_sched_task_state(task) == ffi::PROC_ZOMBIE
                    && ffi::a20_sched_task_should_reap_zombie(task) != 0
                    && count < to_reap.len()
                {
                    to_reap[count] = task;
                    count += 1;
                }
                task = ffi::proc_next_task_locked(task);
            }
            for task in to_reap[..count].iter().copied() {
                ffi::a20_sched_task_set_state(task, ffi::PROC_UNUSED);
                ffi::proc_unlink_task_locked(task);
            }
        }
        if count == 0 {
            break;
        }
        for task in to_reap[..count].iter().copied() {
            unsafe { ffi::proc_destroy_task(task) };
        }
    }
}

#[no_mangle]
pub extern "C" fn context_switch(next: *mut ffi::Task) {
    if next.is_null() || unsafe { ffi::a20_sched_task_kstack(next) } == 0 {
        return;
    }
    let now = unsafe { ffi::timer_get_ticks() };
    let prev = unsafe { ffi::proc_current() };
    if !prev.is_null() {
        let cg = unsafe { ffi::a20_sched_task_cgroup(prev) };
        let cg_start = unsafe { ffi::a20_sched_task_cg_cpu_start(prev) };
        if !cg.is_null() && cg_start > 0 {
            let elapsed_ticks = now.wrapping_sub(cg_start);
            let elapsed_ns = elapsed_ticks.saturating_mul(1_000_000_000u64) / ticks_per_sec();
            let throttled = unsafe { ffi::cg_cpu_account(cg, elapsed_ns, now) };
            if throttled != 0 {
                unsafe { ffi::a20_sched_task_set_cg_throttled(prev, 1) };
            }
            unsafe { ffi::cg_cpu_check_unthrottle(cg, now) };
        }
    }
    unsafe { ffi::a20_sched_task_set_cg_cpu_start(next, now) };
    if next == prev {
        unsafe {
            ffi::a20_sched_task_set_state(next, ffi::PROC_RUNNING);
            ffi::a20_sched_task_set_on_rq(next, 0);
        }
        return;
    }
    let old = unsafe { ffi::proc_set_current(next) };
    unsafe {
        ffi::a20_sched_task_set_state(next, ffi::PROC_RUNNING);
        ffi::a20_sched_task_set_on_rq(next, 0);
    }
    if !prev.is_null() {
        let prev_pid = unsafe { ffi::a20_sched_task_pid(prev) };
        let next_pid = unsafe { ffi::a20_sched_task_pid(next) };
        if prev_pid >= 4 && next_pid >= 4 {
            unsafe { ffi::a20_sched_trace_ctxsw(prev_pid, next_pid) };
        }
    }
    unsafe { ffi::a20_sched_low_level_switch(old, ffi::a20_sched_task_kstack(next)) };
}

#[no_mangle]
pub extern "C" fn sched() {
    let now = unsafe { ffi::timer_get_ticks() };
    unsafe { ffi::kernel_progress_run_bottom_halves() };
    sched_reap_zombies();
    if now >= NEXT_WAKE_SCAN.load(Ordering::Relaxed) || now >= NEXT_ALARM_SCAN.load(Ordering::Relaxed) {
        scan_timers(now);
    }
    let next = proc_runq_pick_locked();
    if !next.is_null() {
        unsafe { ffi::a20_sched_task_set_exec_start(next, now) };
        context_switch(next);
        return;
    }
    let cur = unsafe { ffi::proc_current() };
    if !cur.is_null() {
        let state = unsafe { ffi::a20_sched_task_state(cur) };
        if state == ffi::PROC_READY || state == ffi::PROC_RUNNING {
            unsafe { ffi::a20_sched_task_set_state(cur, ffi::PROC_RUNNING) };
            return;
        }
    }
    let idle = unsafe { ffi::proc_idle_task() };
    if cur != idle {
        if !cur.is_null() {
            let pid = unsafe { ffi::a20_sched_task_pid(cur) };
            if pid >= 4 {
                unsafe { ffi::a20_sched_trace_fall_to_idle(pid, ffi::a20_sched_task_state(cur)) };
            }
        }
        context_switch(idle);
    }
}

#[no_mangle]
pub extern "C" fn proc_yield() {
    let cur = unsafe { ffi::proc_current() };
    let idle = unsafe { ffi::proc_idle_task() };
    if !cur.is_null() && cur != idle && unsafe { ffi::a20_sched_task_state(cur) } == ffi::PROC_RUNNING {
        let now = unsafe { ffi::timer_get_ticks() };
        let elapsed = now.wrapping_sub(unsafe { ffi::a20_sched_task_exec_start(cur) });
        let level = unsafe { ffi::a20_sched_task_sched_level(cur) };
        if !sched_task_rt(cur) && elapsed >= yield_slice() && (level as usize) < ffi::SCHED_LEVELS - 1 {
            unsafe { ffi::a20_sched_task_set_sched_level(cur, level + 1) };
        }
        let pid = unsafe { ffi::a20_sched_task_pid(cur) };
        if pid >= 4 {
            unsafe { ffi::a20_sched_trace_yield(pid) };
        }
        unsafe { ffi::a20_sched_task_set_state(cur, ffi::PROC_READY) };
        unsafe { ffi::proc_make_ready(cur) };
    }
    sched();
}
