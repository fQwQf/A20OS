#![no_std]

mod ffi;

use a20rust_support::lock::IrqSaveSpinLock;
use core::ffi::c_int;
use core::{mem, ptr};

use ffi::{mm_struct_t, robust_list, robust_list_head, task_t};

const FUTEX_WAITERS_MAX: usize = 1024;

#[derive(Clone, Copy)]
struct FutexWaiter {
    active: bool,
    woken: bool,
    vkey: usize,
    pkey: usize,
    mm: *mut mm_struct_t,
    bitset: u32,
    task: *mut task_t,
}

impl FutexWaiter {
    const fn empty() -> Self {
        Self {
            active: false,
            woken: false,
            vkey: 0,
            pkey: 0,
            mm: ptr::null_mut(),
            bitset: 0,
            task: ptr::null_mut(),
        }
    }
}

unsafe impl Send for FutexWaiter {}

struct FutexState {
    waiters: [FutexWaiter; FUTEX_WAITERS_MAX],
    wake_generation: u64,
}

unsafe impl Send for FutexState {}

static FUTEX_STATE: IrqSaveSpinLock<FutexState> = IrqSaveSpinLock::new(FutexState {
    waiters: [FutexWaiter::empty(); FUTEX_WAITERS_MAX],
    wake_generation: 0,
});

#[inline]
fn clear_waiter_slot(w: &mut FutexWaiter) {
    *w = FutexWaiter::empty();
}

fn clear_waiter_task(state: &mut FutexState, task: *mut task_t) {
    if task.is_null() {
        return;
    }
    for waiter in &mut state.waiters {
        if waiter.active && waiter.task == task {
            clear_waiter_slot(waiter);
        }
    }
}

fn waiter_matches(w: &FutexWaiter, mm: *mut mm_struct_t, vkey: usize, pkey: usize) -> bool {
    if !w.active || w.woken {
        return false;
    }
    if !w.task.is_null() {
        let state = unsafe { ffi::a20_futex_task_state(w.task) };
        if state == ffi::PROC_UNUSED || state == ffi::PROC_ZOMBIE {
            return false;
        }
    }
    (w.mm == mm && w.vkey == vkey) || (pkey != 0 && w.pkey == pkey)
}

fn alloc_waiter(
    state: &mut FutexState,
    vkey: usize,
    pkey: usize,
    mm: *mut mm_struct_t,
    bitset: u32,
    task: *mut task_t,
) -> Result<usize, c_int> {
    clear_waiter_task(state, task);
    for (idx, waiter) in state.waiters.iter_mut().enumerate() {
        if !waiter.active {
            *waiter = FutexWaiter {
                active: true,
                woken: false,
                vkey,
                pkey,
                mm,
                bitset,
                task,
            };
            return Ok(idx);
        }
    }
    Err(-ffi::ENOMEM)
}

fn timeout_ticks(timeout: *mut core::ffi::c_void, absolute: bool, realtime: bool) -> Result<u64, c_int> {
    let mut ticks = 0u64;
    let rc = unsafe {
        ffi::a20_futex_timeout_ticks(
            timeout,
            if absolute { 1 } else { 0 },
            if realtime { 1 } else { 0 },
            &mut ticks,
        )
    };
    if rc < 0 {
        Err(rc)
    } else {
        Ok(ticks)
    }
}

/* Futex wait/wake participates in BLOCK_WAKE_PROTOCOL: g_futex_lock protects
 * waiter slot publication, then wake paths drop it before proc_make_ready(). */
fn futex_wait_on(
    uaddr: *mut c_int,
    expected: c_int,
    timeout: *mut core::ffi::c_void,
    bitset: u32,
    absolute_timeout: bool,
    realtime_timeout: bool,
) -> i64 {
    if uaddr.is_null() {
        return -(ffi::EFAULT as i64);
    }
    if bitset == 0 {
        return -(ffi::EINVAL as i64);
    }

    let task = unsafe { ffi::proc_current() };
    if task.is_null() {
        return -(ffi::ESRCH as i64);
    }

    let wait_generation = { FUTEX_STATE.lock().wake_generation };

    let mut uval = 0i32;
    if unsafe {
        ffi::copy_from_user(
            &mut uval as *mut _ as *mut _,
            uaddr.cast_const().cast(),
            mem::size_of::<c_int>(),
        )
    } < 0
    {
        return -(ffi::EFAULT as i64);
    }
    if uval != expected {
        return -(ffi::EAGAIN as i64);
    }

    let ticks = match timeout_ticks(timeout, absolute_timeout, realtime_timeout) {
        Ok(v) => v,
        Err(e) => return e as i64,
    };
    let until = if ticks != 0 {
        unsafe { ffi::a20_futex_get_ticks() }.saturating_add(ticks)
    } else {
        0
    };
    let vkey = uaddr as usize;
    let pkey = unsafe { ffi::a20_futex_phys_key(uaddr) };
    let mm = unsafe { ffi::a20_futex_task_mm(task) };

    let slot = {
        let mut state = FUTEX_STATE.lock();
        if wait_generation != state.wake_generation {
            return 0;
        }
        if unsafe { ffi::signal_task_has_unblocked(task) } != 0 {
            return -(ffi::ERESTARTSYS as i64);
        }
        let slot = match alloc_waiter(&mut state, vkey, pkey, mm, bitset, task) {
            Ok(slot) => slot,
            Err(e) => return e as i64,
        };
        unsafe {
            if until != 0 {
                ffi::proc_set_wake_time(task, until);
            }
            ffi::a20_futex_set_blocked(task);
        }
        slot
    };

    unsafe { ffi::sched() };

    let (was_woken, still_waiting) = {
        let mut state = FUTEX_STATE.lock();
        let mut was_woken = false;
        let mut still_waiting = false;
        if slot < FUTEX_WAITERS_MAX {
            let waiter = &mut state.waiters[slot];
            was_woken = waiter.woken;
            if waiter.active && waiter.task == task {
                still_waiting = true;
            }
            clear_waiter_slot(waiter);
        }
        (was_woken, still_waiting)
    };
    unsafe { ffi::proc_set_wake_time(task, 0) };

    if was_woken {
        return 0;
    }
    if unsafe { ffi::signal_task_has_unblocked(task) } != 0 {
        return -(ffi::ERESTARTSYS as i64);
    }
    if !was_woken && still_waiting && until != 0 && unsafe { ffi::a20_futex_get_ticks() } >= until {
        return -(ffi::ETIMEDOUT as i64);
    }
    0
}

fn futex_wake_on(uaddr: *mut c_int, nr: c_int, bitset: u32) -> i64 {
    let cur = unsafe { ffi::proc_current() };
    if uaddr.is_null() {
        return -(ffi::EFAULT as i64);
    }
    if bitset == 0 || nr < 0 {
        return -(ffi::EINVAL as i64);
    }

    let vkey = uaddr as usize;
    let pkey = unsafe { ffi::a20_futex_phys_key(uaddr) };
    let mm = unsafe { ffi::a20_futex_task_mm(cur) };
    let mut wake_list = [ptr::null_mut(); FUTEX_WAITERS_MAX];
    let wake_count = {
        let mut state = FUTEX_STATE.lock();
        let mut woke = 0usize;
        if nr > 0 {
            state.wake_generation = state.wake_generation.wrapping_add(1);
        }
        for waiter in &mut state.waiters {
            if woke >= nr as usize {
                break;
            }
            if !waiter_matches(waiter, mm, vkey, pkey) || (waiter.bitset & bitset) == 0 {
                continue;
            }
            let task = waiter.task;
            waiter.woken = true;
            if !task.is_null() {
                wake_list[woke] = task;
                woke += 1;
            }
        }
        woke
    };

    for task in wake_list[..wake_count].iter().copied() {
        unsafe {
            ffi::proc_set_wake_time(task, 0);
            ffi::proc_make_ready(task);
        }
    }
    wake_count as i64
}

fn futex_requeue(
    uaddr: *mut c_int,
    wake_nr: c_int,
    requeue_nr: c_int,
    uaddr2: *mut c_int,
    check_cmp: bool,
    cmpval: c_int,
) -> i64 {
    if uaddr.is_null() || uaddr2.is_null() {
        return -(ffi::EFAULT as i64);
    }
    if wake_nr < 0 || requeue_nr < 0 {
        return -(ffi::EINVAL as i64);
    }
    if check_cmp {
        let mut uval = 0i32;
        if unsafe {
            ffi::copy_from_user(
                &mut uval as *mut _ as *mut _,
                uaddr.cast_const().cast(),
                mem::size_of::<c_int>(),
            )
        } < 0
        {
            return -(ffi::EFAULT as i64);
        }
        if uval != cmpval {
            return -(ffi::EAGAIN as i64);
        }
    }

    let cur = unsafe { ffi::proc_current() };
    let mm = unsafe { ffi::a20_futex_task_mm(cur) };
    let vkey1 = uaddr as usize;
    let pkey1 = unsafe { ffi::a20_futex_phys_key(uaddr) };
    let vkey2 = uaddr2 as usize;
    let pkey2 = unsafe { ffi::a20_futex_phys_key(uaddr2) };
    let mut wake_list = [ptr::null_mut(); FUTEX_WAITERS_MAX];

    let (done, moved) = {
        let mut state = FUTEX_STATE.lock();
        let mut done = 0usize;
        let mut moved = 0usize;
        if wake_nr > 0 || requeue_nr > 0 {
            state.wake_generation = state.wake_generation.wrapping_add(1);
        }
        for waiter in &mut state.waiters {
            if done >= wake_nr as usize {
                break;
            }
            if !waiter_matches(waiter, mm, vkey1, pkey1) {
                continue;
            }
            let task = waiter.task;
            waiter.woken = true;
            if !task.is_null() {
                wake_list[done] = task;
                done += 1;
            }
        }
        for waiter in &mut state.waiters {
            if moved >= requeue_nr as usize {
                break;
            }
            if !waiter_matches(waiter, mm, vkey1, pkey1) {
                continue;
            }
            waiter.vkey = vkey2;
            waiter.pkey = pkey2;
            waiter.mm = mm;
            moved += 1;
        }
        (done, moved)
    };

    for task in wake_list[..done].iter().copied() {
        unsafe {
            ffi::proc_set_wake_time(task, 0);
            ffi::proc_make_ready(task);
        }
    }
    (done + moved) as i64
}

fn futex_wake_op_cmp(oldval: c_int, cmp: c_int, cmparg: c_int) -> bool {
    match cmp {
        ffi::FUTEX_OP_CMP_EQ => oldval == cmparg,
        ffi::FUTEX_OP_CMP_NE => oldval != cmparg,
        ffi::FUTEX_OP_CMP_LT => oldval < cmparg,
        ffi::FUTEX_OP_CMP_LE => oldval <= cmparg,
        ffi::FUTEX_OP_CMP_GT => oldval > cmparg,
        ffi::FUTEX_OP_CMP_GE => oldval >= cmparg,
        _ => false,
    }
}

fn futex_wake_op_new_value(oldval: c_int, op: c_int, oparg: c_int) -> Result<c_int, c_int> {
    match op & 0xf {
        ffi::FUTEX_OP_SET => Ok(oparg),
        ffi::FUTEX_OP_ADD => Ok(oldval.wrapping_add(oparg)),
        ffi::FUTEX_OP_OR => Ok(oldval | oparg),
        ffi::FUTEX_OP_ANDN => Ok(oldval & !oparg),
        ffi::FUTEX_OP_XOR => Ok(oldval ^ oparg),
        _ => Err(-ffi::EINVAL),
    }
}

fn futex_wake_op(
    uaddr: *mut c_int,
    wake_nr: c_int,
    wake2_nr: c_int,
    uaddr2: *mut c_int,
    encoded_op: c_int,
) -> i64 {
    if uaddr.is_null() || uaddr2.is_null() {
        return -(ffi::EFAULT as i64);
    }
    if wake_nr < 0 || wake2_nr < 0 {
        return -(ffi::EINVAL as i64);
    }

    let mut oldval = 0i32;
    if unsafe {
        ffi::copy_from_user(
            &mut oldval as *mut _ as *mut _,
            uaddr2.cast_const().cast(),
            mem::size_of::<c_int>(),
        )
    } < 0
    {
        return -(ffi::EFAULT as i64);
    }

    let mut op = (encoded_op >> 28) & 0xf;
    let cmp = (encoded_op >> 24) & 0xf;
    let mut oparg = (encoded_op >> 12) & 0xfff;
    let cmparg = encoded_op & 0xfff;
    if (op & ffi::FUTEX_OP_OPARG_SHIFT) != 0 {
        op &= !ffi::FUTEX_OP_OPARG_SHIFT;
        if oparg >= 31 {
            return -(ffi::EINVAL as i64);
        }
        oparg = 1 << oparg;
    }

    let newval = match futex_wake_op_new_value(oldval, op, oparg) {
        Ok(v) => v,
        Err(e) => return e as i64,
    };
    if unsafe {
        ffi::copy_to_user(
            uaddr2.cast(),
            &newval as *const _ as *const _,
            mem::size_of::<c_int>(),
        )
    } < 0
    {
        return -(ffi::EFAULT as i64);
    }

    let cur = unsafe { ffi::proc_current() };
    let mm = unsafe { ffi::a20_futex_task_mm(cur) };
    let vkey1 = uaddr as usize;
    let pkey1 = unsafe { ffi::a20_futex_phys_key(uaddr) };
    let vkey2 = uaddr2 as usize;
    let pkey2 = unsafe { ffi::a20_futex_phys_key(uaddr2) };
    let mut wake_list = [ptr::null_mut(); FUTEX_WAITERS_MAX];

    let wake_count = {
        let mut state = FUTEX_STATE.lock();
        let mut woke = 0usize;
        if wake_nr > 0 || wake2_nr > 0 {
            state.wake_generation = state.wake_generation.wrapping_add(1);
        }
        for waiter in &mut state.waiters {
            if woke >= wake_nr as usize {
                break;
            }
            if !waiter_matches(waiter, mm, vkey1, pkey1) {
                continue;
            }
            let task = waiter.task;
            waiter.woken = true;
            if !task.is_null() {
                wake_list[woke] = task;
                woke += 1;
            }
        }

        if futex_wake_op_cmp(oldval, cmp, cmparg) {
            let mut woke2 = 0usize;
            for waiter in &mut state.waiters {
                if woke2 >= wake2_nr as usize {
                    break;
                }
                if !waiter_matches(waiter, mm, vkey2, pkey2) {
                    continue;
                }
                let task = waiter.task;
                waiter.woken = true;
                if !task.is_null() {
                    wake_list[woke] = task;
                    woke += 1;
                    woke2 += 1;
                }
            }
        }
        woke
    };

    for task in wake_list[..wake_count].iter().copied() {
        unsafe {
            ffi::proc_set_wake_time(task, 0);
            ffi::proc_make_ready(task);
        }
    }
    wake_count as i64
}

#[no_mangle]
pub extern "C" fn futex_wake_user(uaddr: *mut c_int, nr: c_int) -> c_int {
    futex_wake_on(uaddr, nr, ffi::FUTEX_BITSET_MATCH_ANY) as c_int
}

#[no_mangle]
pub extern "C" fn sys_futex(
    uaddr: *mut c_int,
    op: c_int,
    val: c_int,
    timeout: *mut core::ffi::c_void,
    uaddr2: *mut c_int,
    val3: c_int,
) -> i64 {
    match op & ffi::FUTEX_CMD_MASK {
        ffi::FUTEX_WAIT => futex_wait_on(uaddr, val, timeout, ffi::FUTEX_BITSET_MATCH_ANY, false, false),
        ffi::FUTEX_WAIT_BITSET => futex_wait_on(
            uaddr,
            val,
            timeout,
            val3 as u32,
            true,
            (op & ffi::FUTEX_CLOCK_REALTIME) != 0,
        ),
        ffi::FUTEX_WAKE => futex_wake_on(uaddr, val, ffi::FUTEX_BITSET_MATCH_ANY),
        ffi::FUTEX_WAKE_BITSET => futex_wake_on(uaddr, val, val3 as u32),
        ffi::FUTEX_REQUEUE => futex_requeue(uaddr, val, timeout as isize as c_int, uaddr2, false, 0),
        ffi::FUTEX_CMP_REQUEUE => {
            futex_requeue(uaddr, val, timeout as isize as c_int, uaddr2, true, val3)
        }
        ffi::FUTEX_WAKE_OP => futex_wake_op(uaddr, val, timeout as isize as c_int, uaddr2, val3),
        _ => -(ffi::ENOSYS as i64),
    }
}

#[no_mangle]
pub extern "C" fn exit_robust_list(task: *mut task_t) {
    if task.is_null() {
        return;
    }
    let head_addr = unsafe { ffi::a20_futex_task_robust_list_head(task) };
    if head_addr == 0 {
        return;
    }

    let mut head = robust_list_head {
        list: robust_list { next: 0 },
        futex_offset: 0,
        list_op_pending: ptr::null_mut(),
    };
    if unsafe {
        ffi::copy_from_user(
            &mut head as *mut _ as *mut _,
            head_addr as *const _,
            mem::size_of::<robust_list_head>(),
        )
    } < 0
    {
        return;
    }

    let tid = unsafe { ffi::a20_futex_task_pid(task) } as u32;
    let mut count = 0usize;
    let mut entry = head.list.next;

    while entry != 0 && entry != head_addr && count < ffi::ROBUST_LIST_LIMIT {
        let futex_addr = entry.wrapping_add(head.futex_offset as usize);
        let mut futex_word = 0u32;
        if unsafe {
            ffi::copy_from_user(
                &mut futex_word as *mut _ as *mut _,
                futex_addr as *const _,
                mem::size_of::<u32>(),
            )
        } >= 0
        {
            if (futex_word & ffi::FUTEX_TID_MASK) == tid {
                let new_val = (futex_word & ffi::FUTEX_WAITERS) | ffi::FUTEX_OWNER_DIED;
                let _ = unsafe {
                    ffi::copy_to_user(
                        futex_addr as *mut _,
                        &new_val as *const _ as *const _,
                        mem::size_of::<u32>(),
                    )
                };
                if (futex_word & ffi::FUTEX_WAITERS) != 0 {
                    futex_wake_user(futex_addr as *mut c_int, 1);
                }
            }
        }

        let mut node = robust_list { next: 0 };
        if unsafe {
            ffi::copy_from_user(
                &mut node as *mut _ as *mut _,
                entry as *const _,
                mem::size_of::<robust_list>(),
            )
        } < 0
        {
            break;
        }
        entry = node.next;
        count += 1;
    }

    unsafe { ffi::a20_futex_task_clear_robust_list_head(task) };
}
