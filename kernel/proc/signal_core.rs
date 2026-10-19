#![no_std]

// Signal decision core (kernel/proc/signal_core.rs).
//
// Rust owns the transplantable signal business logic: default-action
// classification (terminate/stop/ignore/core-dump), wait-status and
// fatal-exit encoding, mask transitions and operand stripping, deliverability
// and fatality decisions, pending-queue coalescing policy (the process /
// thread-scope asymmetry), pending-info scope selection and clearing,
// generation-cancel rules, delivery selection, restart-after-handler policy,
// per-signal action lifecycle (exec reset, sigsuspend restore, SIGCHLD
// flags), syscall-argument validation shared by the Linux and Native ABIs,
// the Native checkpoint thread-check transaction, and signalfd readiness /
// batching decisions.
//
// The C adapter (kernel/proc/signal.c, kernel/abi/linux/sys_signal.c,
// kernel/abi/native/sys_task_signal.c, kernel/abi/native/handle_table.c,
// kernel/ipc/signalfd.c) keeps the IRQ-safe locks, task and signal_state
// access, usercopy, wait queues, the hardware signal frame, arch assembly and
// every side effect. The signal business logic has no C fallback.
//
// Kernel-mask convention (see kernel/include/proc/signal.h): bit position ==
// signum, bit 0 unused. The user/kernel ABI conversion (user bit N-1) stays
// at the syscall boundary in C. All masks here are kernel masks.

const SIGQUIT: i32 = 3;
const SIGILL: i32 = 4;
const SIGABRT: i32 = 6;
const SIGBUS: i32 = 7;
const SIGFPE: i32 = 8;
const SIGKILL: i32 = 9;
const SIGSEGV: i32 = 11;
const SIGCHLD: i32 = 17;
const SIGCONT: i32 = 18;
const SIGSTOP: i32 = 19;
const SIGTSTP: i32 = 20;
const SIGTTIN: i32 = 21;
const SIGTTOU: i32 = 22;
const SIGURG: i32 = 23;
const SIGWINCH: i32 = 28;
const SIGSYS: i32 = 31;

const SIG_BLOCK: i32 = 0;
const SIG_UNBLOCK: i32 = 1;
const SIG_SETMASK: i32 = 2;

const SIG_DFL: u64 = 0;
const SIG_IGN: u64 = 1;

/* sigaction sa_flags (kernel/include/proc/signal.h). */
const SA_NOCLDSTOP: i32 = 1;
const SA_NOCLDWAIT: i32 = 2;
const SA_RESTART: i32 = 0x1000_0000;

/* sigaltstack ss_flags. */
const SS_DISABLE: u32 = 2;

/* errno values (kernel/include/core/errno.h), returned through the C ABI. */
const EPERM: i32 = 1;
const ENOENT: i32 = 2;
const ENOMEM: i32 = 12;
const EINVAL: i32 = 22;

/* Native checkpoint selector flags (kernel/include/uapi/a20/task_signal.h). */
const A20_SIGNAL_THREAD: u32 = 1;
const A20_SIGNAL_PEEK: u32 = 2;
const A20_SIGNAL_SELECT: u32 = 4;

/// NSIG is 64 (kernel/include/proc/signal.h); bit 63 is unused but part of
/// the u64 mask domain. Only bits 1..=62 can name a signal.
#[inline]
fn bit(sig: i32) -> u64 {
    if sig > 0 && sig < 64 {
        1u64 << sig
    } else {
        0
    }
}

/* ============================================================
 * Default-action classification and status encodings
 * ============================================================ */

/// Default action dumps core (WCOREDUMP-eligible) for this signal.
#[no_mangle]
pub extern "C" fn a20_signal_core_dump_default(sig: i32) -> i32 {
    match sig {
        SIGQUIT | SIGILL | SIGABRT | SIGBUS | SIGFPE | SIGSEGV | SIGSYS => 1,
        _ => 0,
    }
}

/// Default action terminates the process for this signal. The exceptions are
/// the Linux default-ignored set plus the stop/continue job-control signals.
#[no_mangle]
pub extern "C" fn a20_signal_default_terminate(sig: i32) -> i32 {
    match sig {
        SIGCHLD | SIGURG | SIGWINCH | SIGSTOP | SIGTSTP | SIGTTIN | SIGTTOU | SIGCONT => 0,
        _ => 1,
    }
}

/// Default action stops the process for this signal.
#[no_mangle]
pub extern "C" fn a20_signal_default_stop(sig: i32) -> i32 {
    match sig {
        SIGSTOP | SIGTSTP | SIGTTIN | SIGTTOU => 1,
        _ => 0,
    }
}

/// Default action ignores this signal.
#[no_mangle]
pub extern "C" fn a20_signal_default_ignore(sig: i32) -> i32 {
    match sig {
        SIGCHLD | SIGURG | SIGWINCH | SIGCONT => 1,
        _ => 0,
    }
}

/// wait(2) status encoding: signal number in the low 7 bits, the 0x80
/// WCOREDUMP bit only when a core file was actually produced.
#[no_mangle]
pub extern "C" fn a20_signal_wait_status_dumped(sig: i32, dumped: i32) -> i32 {
    let mut status = sig & 0x7f;
    if dumped != 0 {
        status |= 0x80;
    }
    status
}

/// Negative wait-status exit code for fatal default actions.
#[no_mangle]
pub extern "C" fn a20_signal_fatal_exit_code_dumped(sig: i32, dumped: i32) -> i32 {
    -a20_signal_wait_status_dumped(sig, dumped)
}

/* ============================================================
 * Mask transitions and operand policy
 * ============================================================ */

/// sigprocmask-style transition on kernel masks. The caller pre-strips the
/// SIGKILL/SIGSTOP bits from `set` (the C adapter in kernel/proc/signal.c
/// owns that operand policy, matching the C reference exactly: the blocked
/// mask may therefore legally lack those bits); Rust performs only the
/// how-mode transition. An invalid `how` yields `old` unchanged.
#[no_mangle]
pub extern "C" fn a20_signal_mask_apply(how: i32, old: u64, set: u64) -> u64 {
    match how {
        SIG_BLOCK => old | set,
        SIG_UNBLOCK => old & !set,
        SIG_SETMASK => set,
        _ => old,
    }
}

/// Strip the two signals that can never be blocked (SIGKILL, SIGSTOP) from a
/// kernel mask. Used for the syscall operand, the sigsuspend replacement mask
/// and the temporary-mask seam.
#[no_mangle]
pub extern "C" fn a20_signal_mask_strip_unblockable(mask: u64) -> u64 {
    mask & !(bit(SIGKILL) | bit(SIGSTOP))
}

/// Signals pending delivery to a task: process + thread pending, minus the
/// blocked set.
#[no_mangle]
pub extern "C" fn a20_signal_deliverable_mask(pending: u64, thread_pending: u64, blocked: u64) -> u64 {
    (pending | thread_pending) & !blocked
}

/// Union of the process- and thread-scope pending masks.
#[no_mangle]
pub extern "C" fn a20_signal_pending_union(pending: u64, thread_pending: u64) -> u64 {
    pending | thread_pending
}

/// Pending signals that are also blocked (rt_sigpending / sigwait view).
#[no_mangle]
pub extern "C" fn a20_signal_pending_blocked(pending: u64, thread_pending: u64, blocked: u64) -> u64 {
    (pending | thread_pending) & blocked
}

/// Lowest signal number set in a kernel mask, or 0 when the mask is empty.
/// Bit 0 is never a signal; any bits above it are honoured.
#[no_mangle]
pub extern "C" fn a20_signal_lowest(mask: u64) -> i32 {
    let named = mask & !1;
    if named == 0 {
        0
    } else {
        named.trailing_zeros() as i32
    }
}

/* ============================================================
 * Deliverability and fatality
 * ============================================================ */

/// Per-signal deliverability from a plain view: blocked, explicitly ignored,
/// or default-ignored signals are not deliverable.
#[no_mangle]
pub extern "C" fn a20_signal_action_deliverable(blocked_mask: u64, handler: u64, sig: i32) -> i32 {
    if blocked_mask & bit(sig) != 0 {
        return 0;
    }
    if handler == SIG_IGN {
        return 0;
    }
    if handler == SIG_DFL && a20_signal_default_ignore(sig) != 0 {
        return 0;
    }
    1
}

/// A pending signal is fatal when it is deliverable, has no handler, and its
/// default action terminates.
#[no_mangle]
pub extern "C" fn a20_signal_action_fatal(blocked_mask: u64, handler: u64, sig: i32) -> i32 {
    if a20_signal_action_deliverable(blocked_mask, handler, sig) == 0 {
        return 0;
    }
    if handler != SIG_DFL {
        return 0;
    }
    a20_signal_default_terminate(sig)
}

/// Whether a user signal handler can run for `sig` right now: a real handler,
/// not explicitly ignored, and not blocked.
#[no_mangle]
pub extern "C" fn a20_signal_action_user_handler_available(handler: u64, blocked: i32) -> i32 {
    if handler == SIG_DFL || handler == SIG_IGN || blocked != 0 {
        return 0;
    }
    1
}

/* ============================================================
 * Queue coalescing policy (process / thread-scope asymmetry)
 * ============================================================ */

/// Discard-at-generation policy: an explicitly ignored, or default-ignored,
/// signal is dropped at queue time -- except SIGCONT, which always leaves a
/// pending marker (the job-control wake marker depends on it).
#[no_mangle]
pub extern "C" fn a20_signal_queue_discard(sig: i32, handler: u64) -> i32 {
    if sig == SIGCONT {
        return 0;
    }
    if handler == SIG_IGN {
        return 1;
    }
    if handler == SIG_DFL && a20_signal_default_ignore(sig) != 0 {
        return 1;
    }
    0
}

/// Whether a queued occurrence updates the stored siginfo for its scope.
/// Process scope is last-wins (always updates); thread scope is first-wins
/// and only records info for the first occurrence of the signum.
#[no_mangle]
pub extern "C" fn a20_signal_queue_update_info(thread_directed: i32, thread_bit_set: i32) -> i32 {
    if thread_directed != 0 {
        (thread_bit_set == 0) as i32
    } else {
        1
    }
}

/// Whether the stored siginfo should report "has info" after this queue.
/// Process scope mirrors the input (a later no-info queue clears the flag);
/// thread scope only sets it when the first occurrence carried info.
#[no_mangle]
pub extern "C" fn a20_signal_queue_info_has(
    thread_directed: i32,
    thread_bit_set: i32,
    info_present: i32,
) -> i32 {
    if thread_directed != 0 {
        (info_present != 0 && thread_bit_set == 0) as i32
    } else {
        (info_present != 0) as i32
    }
}

/// Pending-info scope selection for peek/take. Returns 0 for the thread
/// scope, 1 for the process scope, or -ENOENT when the pending occurrence
/// carries no info. Thread occurrences win over process occurrences.
#[no_mangle]
pub extern "C" fn a20_signal_peek_scope(
    thread_bit: i32,
    thread_info_obj: i32,
    thread_has_info: i32,
    process_has_info: i32,
) -> i32 {
    if thread_bit != 0 {
        if thread_info_obj == 0 || thread_has_info == 0 {
            return -ENOENT;
        }
        0
    } else if process_has_info == 0 {
        -ENOENT
    } else {
        1
    }
}

/// Which scope an occurrence of `sig` was queued in and must be cleared from.
/// Returns 0 for the thread scope, 1 for the process scope.
#[no_mangle]
pub extern "C" fn a20_signal_clear_scope(thread_bit: i32) -> i32 {
    if thread_bit != 0 {
        0
    } else {
        1
    }
}

/* ============================================================
 * Generation-cancel rules
 * ============================================================ */

/// Generation-cancel rules: queuing SIGCONT cancels pending stop signals and
/// queuing a stop signal cancels a pending SIGCONT. Returns the kernel-mask
/// of signals to cancel from both pending scopes; 0 when no rule applies.
#[no_mangle]
pub extern "C" fn a20_signal_generation_cancel_mask(sig: i32) -> u64 {
    if sig == SIGCONT {
        bit(SIGSTOP) | bit(SIGTSTP) | bit(SIGTTIN) | bit(SIGTTOU)
    } else if a20_signal_default_stop(sig) != 0 {
        bit(SIGCONT)
    } else {
        0
    }
}

/* ============================================================
 * Restart-after-handler policy
 * ============================================================ */

/// A deliverable signal that is ignored or default-ignored does not affect
/// the restart decision and is skipped.
#[no_mangle]
pub extern "C" fn a20_signal_restart_skips(handler: u64, sig: i32) -> i32 {
    if handler == SIG_IGN {
        return 1;
    }
    if handler == SIG_DFL && a20_signal_default_ignore(sig) != 0 {
        return 1;
    }
    0
}

/// A deliverable user handler without SA_RESTART vetoes restarting the
/// interrupted syscall.
#[no_mangle]
pub extern "C" fn a20_signal_restart_vetoes(handler: u64, flags: i32) -> i32 {
    (handler != SIG_DFL && handler != SIG_IGN && (flags & SA_RESTART) == 0) as i32
}

/* ============================================================
 * Action lifecycle helpers
 * ============================================================ */

/// SIGCHLD auto-reap: the disposition is SIG_IGN or SA_NOCLDWAIT is set.
#[no_mangle]
pub extern "C" fn a20_signal_sigchld_auto_reap(handler: u64, flags: i32) -> i32 {
    (handler == SIG_IGN || (flags & SA_NOCLDWAIT) != 0) as i32
}

/// SIGCHLD stop notification suppression (SA_NOCLDSTOP).
#[no_mangle]
pub extern "C" fn a20_signal_sigchld_no_cldstop(flags: i32) -> i32 {
    (flags & SA_NOCLDSTOP != 0) as i32
}

/// SIGCONT is pending in either scope.
#[no_mangle]
pub extern "C" fn a20_signal_continue_pending(pending_union: u64) -> i32 {
    (pending_union & bit(SIGCONT) != 0) as i32
}

/// Temporary-mask operand: the replacement mask for sigsuspend / the
/// epoll_pwait seam, with the two unblockable signals removed.
#[no_mangle]
pub extern "C" fn a20_signal_temporary_mask(new_mask: u64) -> u64 {
    a20_signal_mask_strip_unblockable(new_mask)
}

/// A deferred sigsuspend mask is restored on the next boundary only when the
/// mask replacement is active and no handler is currently running.
#[no_mangle]
pub extern "C" fn a20_signal_sigsuspend_restore_active(sigsuspend_active: i32, sig_handling: i32) -> i32 {
    (sigsuspend_active != 0 && sig_handling == 0) as i32
}

/// exec resets every disposition to SIG_DFL except an explicit SIG_IGN,
/// which survives across exec.
#[no_mangle]
pub extern "C" fn a20_signal_exec_action_reset(handler: u64) -> u64 {
    if handler != SIG_IGN && handler != SIG_DFL {
        SIG_DFL
    } else {
        handler
    }
}

/* ============================================================
 * Syscall-argument validation (Linux and Native ABI)
 * ============================================================ */

/// rt_sigaction(2) argument validation: a valid, non-KILL/STOP signum and the
/// architecture sigset size. Returns 0 or -EINVAL.
#[no_mangle]
pub extern "C" fn a20_signal_action_valid(sig: i32, sigsetsize: u64, arch_sigset_size: u64) -> i32 {
    if sig <= 0 || sig >= 64 || sig == SIGKILL || sig == SIGSTOP {
        return -EINVAL;
    }
    if sigsetsize != arch_sigset_size {
        return -EINVAL;
    }
    0
}

/// rt_sigprocmask(2) `how` validation; only consulted when a set is present.
/// Returns 0 or -EINVAL.
#[no_mangle]
pub extern "C" fn a20_signal_procmask_how_valid(how: i32) -> i32 {
    if (how as u32) > SIG_SETMASK as u32 {
        -EINVAL
    } else {
        0
    }
}

/// kill(2)/tgkill(2) credential check. CAP_KILL bypasses it; otherwise the
/// sender UID/EUID must match the target real or saved UID. Returns 0 or
/// -EPERM.
#[no_mangle]
pub extern "C" fn a20_signal_kill_permission(
    cap_kill: i32,
    self_uid: u32,
    self_euid: u32,
    target_uid: u32,
    target_suid: u32,
) -> i32 {
    if cap_kill != 0 {
        return 0;
    }
    if self_uid == target_uid
        || self_uid == target_suid
        || self_euid == target_uid
        || self_euid == target_suid
    {
        return 0;
    }
    -EPERM
}

/// sigaltstack(2) set validation: no flag beyond SS_DISABLE, and a non-disabled
/// stack must meet MINSIGSTKSZ. Returns 0, -EINVAL or -ENOMEM.
#[no_mangle]
pub extern "C" fn a20_signal_sigaltstack_valid(flags: u32, size: u64, min_size: u64) -> i32 {
    if flags & !SS_DISABLE != 0 {
        return -EINVAL;
    }
    if flags & SS_DISABLE == 0 && size < min_size {
        return -ENOMEM;
    }
    0
}

/// Native task_kill(2) argument validation. Returns 1 when invalid, else 0.
#[no_mangle]
pub extern "C" fn a20_signal_native_kill_valid(mode: u64, sig: i64, nsig: i32) -> i32 {
    ((mode & !(A20_SIGNAL_THREAD as u64)) != 0 || sig < 0 || sig >= nsig as i64) as i32
}

/// Native signal_check(2) / signal_mask(2) flag validation. Returns 1 when
/// invalid, else 0. `allowed` is the operation-specific flag set; a nonzero
/// mode must at least request the thread scope.
#[no_mangle]
pub extern "C" fn a20_signal_native_mode_valid(mode: u64, allowed: u64) -> i32 {
    ((mode & !allowed) != 0 || (mode != 0 && (mode & A20_SIGNAL_THREAD as u64) == 0)) as i32
}

/// Native structured signal_control validation. `check_mask` / `mask_mask`
/// are the permitted flag sets for the CHECK and MASK operations. Returns 1
/// when invalid, else 0.
#[no_mangle]
pub extern "C" fn a20_signal_native_control_valid(
    operation: u32,
    flags: u32,
    op_check: u32,
    op_mask: u32,
    check_mask: u64,
    mask_mask: u64,
) -> i32 {
    let flags = flags as u64;
    if operation == op_check {
        (flags & !check_mask != 0) as i32
    } else if operation == op_mask {
        (flags & !mask_mask != 0) as i32
    } else {
        1
    }
}

/* ============================================================
 * Native checkpoint thread-signal transaction
 * ============================================================ */

/// Decide the deliverable set for a Native thread checkpoint and report which
/// pending scopes a consuming (non-PEEK) check must clear. Thread occurrences
/// are preferred; a process occurrence of the same signum stays queued.
#[no_mangle]
pub unsafe extern "C" fn a20_signal_native_thread_check(
    pending_ht: u64,
    pending_thread: u64,
    blocked: u64,
    mode: u32,
    selected: u64,
    clear_thread_out: *mut u64,
    clear_ht_out: *mut u64,
) -> u64 {
    let pending = pending_ht | pending_thread;
    let mut deliver = pending & !blocked;
    if mode & A20_SIGNAL_SELECT != 0 {
        deliver = pending & selected;
        /* sigwait consumes one signal, including signals blocked by caller. */
        if mode & A20_SIGNAL_PEEK == 0 {
            deliver &= deliver.wrapping_neg();
        }
    }
    let mut clear_thread = 0u64;
    let mut clear_ht = 0u64;
    if mode & A20_SIGNAL_PEEK == 0 {
        let local = deliver & pending_thread;
        clear_thread = local;
        clear_ht = deliver & !local;
    }
    if !clear_thread_out.is_null() {
        // SAFETY: the caller passes a writable out-parameter.
        unsafe { *clear_thread_out = clear_thread };
    }
    if !clear_ht_out.is_null() {
        // SAFETY: the caller passes a writable out-parameter.
        unsafe { *clear_ht_out = clear_ht };
    }
    deliver
}

/// Native thread signal-mask operand: the two unblockable signals and the
/// unused bit 0 are removed. The handle-table process mask protects only
/// SIGKILL (callers keep that distinct via the C adapter).
#[no_mangle]
pub extern "C" fn a20_signal_native_thread_mask(new_mask: u64) -> u64 {
    new_mask & !(bit(SIGKILL) | bit(SIGSTOP) | 1)
}

/// Native handle-table blocked-mask operand: only SIGKILL is protected here
/// (the documented asymmetry with the thread mask).
#[no_mangle]
pub extern "C" fn a20_signal_native_ht_mask(new_mask: u64) -> u64 {
    new_mask & !bit(SIGKILL)
}

/* ============================================================
 * State transactions
 *
 * The functions below own the real pending/queue state transitions: the
 * caller (C adapter) holds the IRQ-safe signal_state lock and provides raw
 * pointers to the shared process-scope state and the optional per-thread
 * pending-info object; Rust performs the mutation (coalescing, siginfo
 * writes, scope-priority consumption, generation cancel, disposition
 * lifecycle) and reports every side-effect decision back.  The C adapter
 * keeps only locks, allocation, usercopy, wait/wake and scheduling.
 * ============================================================ */

const NSIG: usize = 64;
const SIGNAL_INFO_SIZE: usize = 128;

/* sigaction flag bits used by the delivery transaction
 * (kernel/include/proc/signal.h). */
const SA_NODEFER: u32 = 0x4000_0000;
const SA_RESETHAND: u32 = 0x8000_0000;

/// Borrowed view of the pending/queue state.  All pointers are non-null
/// except the per-thread object pair, which is NULL until the C adapter
/// lazily allocates it.  The caller holds the signal_state lock.
#[repr(C)]
pub struct A20SignalQueueState {
    pub process_pending: *mut u64,
    pub process_has_info: *mut u8,
    pub process_info: *mut u8,
    pub thread_pending: *mut u64,
    pub thread_has_info: *mut u8,
    pub thread_info: *mut u8,
}

impl A20SignalQueueState {
    #[inline]
    unsafe fn has_info_slot(&self, sig: usize, thread_scope: bool) -> *mut u8 {
        if thread_scope {
            self.thread_has_info.add(sig)
        } else {
            self.process_has_info.add(sig)
        }
    }
    #[inline]
    unsafe fn info_slot(&self, sig: usize, thread_scope: bool) -> *mut u8 {
        let base = if thread_scope {
            self.thread_info
        } else {
            self.process_info
        };
        base.add(sig * SIGNAL_INFO_SIZE)
    }
}

/// Clear one signal occurrence from exactly one scope (thread scope wins)
/// and drop its has-info flag, mirroring signal_clear_pending_locked().
#[inline]
unsafe fn clear_one_scope(st: &A20SignalQueueState, sig: usize) {
    let b = 1u64 << sig;
    let thread_scope = (*st.thread_pending & b) != 0;
    if thread_scope {
        *st.thread_pending &= !b;
        if !st.thread_has_info.is_null() {
            *st.has_info_slot(sig, true) = 0;
        }
    } else {
        *st.process_pending &= !b;
        *st.has_info_slot(sig, false) = 0;
    }
}

/// Clear a signal from BOTH scopes (generation-cancel semantics): pending
/// bits and has-info flags in each scope that carries an occurrence.
#[inline]
unsafe fn clear_both_scopes(st: &A20SignalQueueState, sig: usize) {
    let b = 1u64 << sig;
    if (*st.thread_pending & b) != 0 {
        *st.thread_pending &= !b;
        if !st.thread_has_info.is_null() {
            *st.has_info_slot(sig, true) = 0;
        }
    }
    if (*st.process_pending & b) != 0 {
        *st.process_pending &= !b;
        *st.has_info_slot(sig, false) = 0;
    }
}

/// Copy a queued siginfo slot into a caller buffer (zero-padded).
#[inline]
unsafe fn copy_info_slot(src: *const u8, out: *mut u8, out_size: usize) {
    let n = if out_size < SIGNAL_INFO_SIZE {
        out_size
    } else {
        SIGNAL_INFO_SIZE
    };
    let mut i = 0;
    while i < n {
        *out.add(i) = *src.add(i);
        i += 1;
    }
    while i < out_size && i < SIGNAL_INFO_SIZE {
        *out.add(i) = 0;
        i += 1;
    }
}

/// Write a new occurrence into an info slot (truncate + zero-pad).
#[inline]
unsafe fn write_info_slot(dst: *mut u8, info: *const u8, info_size: usize) {
    let n = if info_size < SIGNAL_INFO_SIZE {
        info_size
    } else {
        SIGNAL_INFO_SIZE
    };
    let mut i = 0;
    while i < n {
        *dst.add(i) = *info.add(i);
        i += 1;
    }
    while i < SIGNAL_INFO_SIZE {
        *dst.add(i) = 0;
        i += 1;
    }
}

#[inline]
unsafe fn peek_locked_ref(st: &A20SignalQueueState, sig: i32, out: *mut u8, out_size: usize) -> i32 {
    if sig <= 0 || sig >= NSIG as i32 {
        return -EINVAL;
    }
    let s = sig as usize;
    let b = 1u64 << s;
    let thread_scope = (*st.thread_pending & b) != 0;
    let has_info = if thread_scope {
        !st.thread_has_info.is_null() && *st.has_info_slot(s, true) != 0
    } else {
        *st.has_info_slot(s, false) != 0
    };
    if !has_info {
        return -ENOENT;
    }
    if !out.is_null() && out_size > 0 {
        copy_info_slot(st.info_slot(s, thread_scope), out, out_size);
    }
    0
}

/// Peek the queued siginfo for `sig` (thread occurrence preferred) without
/// consuming.  Returns 0 and fills `out` when info exists, -ENOENT when the
/// pending occurrence carries no info, -EINVAL for a bad signum.
#[no_mangle]
pub unsafe extern "C" fn a20_signal_peek_locked(
    st: *mut A20SignalQueueState,
    sig: i32,
    out: *mut u8,
    out_size: usize,
) -> i32 {
    if st.is_null() {
        return -EINVAL;
    }
    peek_locked_ref(&*st, sig, out, out_size)
}

/// Consume one occurrence of `sig` (thread scope preferred): copy the queued
/// siginfo (when present) and clear the pending bit plus has-info flag of
/// the owning scope.  Returns 0 when info was present, -ENOENT when the
/// consumed occurrence carried no info, -EINVAL for a bad signum.
#[no_mangle]
pub unsafe extern "C" fn a20_signal_consume_locked(
    st: *mut A20SignalQueueState,
    sig: i32,
    out: *mut u8,
    out_size: usize,
) -> i32 {
    if st.is_null() {
        return -EINVAL;
    }
    let st = &*st;
    let ret = peek_locked_ref(st, sig, out, out_size);
    if sig > 0 && sig < NSIG as i32 {
        clear_one_scope(st, sig as usize);
    }
    ret
}

/// Result side-band of one queue transaction.
#[repr(C)]
pub struct A20SignalQueueResult {
    pub discard: i32,       /* signal dropped at generation */
    pub fatal: i32,         /* deliverable + SIG_DFL + terminate */
    pub deliverable: i32,   /* deliverable after queueing */
    pub sigwait_match: i32, /* caller's sigwait set matches this signal */
}

/// The complete queue transaction: apply generation-cancel rules to both
/// scopes, decide discard-at-generation, coalesce the occurrence into the
/// requested scope (thread scope first-wins info, process scope last-wins),
/// write the siginfo bytes, set the pending bit, and report the fatal /
/// deliverable / sigwait-match side effects.  The caller (C adapter) holds
/// the signal_state lock and has already validated the signum range.
#[no_mangle]
pub unsafe extern "C" fn a20_signal_queue_locked(
    st: *mut A20SignalQueueState,
    signum: i32,
    info: *const u8,
    info_size: usize,
    handler: u64,
    blocked: u64,
    thread_directed: i32,
    sigwait_active: i32,
    sigwait_mask: u64,
    result: *mut A20SignalQueueResult,
) -> i32 {
    if st.is_null() || result.is_null() || signum <= 0 || signum >= NSIG as i32 {
        return -EINVAL;
    }
    let st = &*st;
    let result = &mut *result;
    result.discard = 0;
    result.fatal = 0;
    result.deliverable = 0;
    result.sigwait_match = 0;

    /* Generation-cancel: SIGCONT cancels pending stop signals and a stop
     * signal cancels a pending SIGCONT, in both scopes. */
    let cancel = a20_signal_generation_cancel_mask(signum);
    let mut s = 1;
    while s < NSIG {
        if cancel & (1u64 << s) != 0 {
            clear_both_scopes(st, s);
        }
        s += 1;
    }

    if a20_signal_queue_discard(signum, handler) != 0 {
        result.discard = 1;
        return 0;
    }

    let info_present = (!info.is_null() && info_size > 0) as i32;
    let b = bit(signum);
    let thread_bit_set = ((*st.thread_pending & b) != 0) as i32;
    if thread_directed != 0 {
        /* Thread scope: coalesce; first occurrence wins the stored info.
         * Without a per-thread info object (lazy allocation lives in the C
         * adapter) the has-info write is skipped, exactly like the C
         * reference's `update_info && pending` guard. */
        let update_info = a20_signal_queue_update_info(1, thread_bit_set);
        let info_has = a20_signal_queue_info_has(1, thread_bit_set, info_present);
        if update_info != 0 && !st.thread_info.is_null() {
            *st.has_info_slot(signum as usize, true) = info_has as u8;
            if info_present != 0 {
                write_info_slot(st.info_slot(signum as usize, true), info, info_size);
            }
        }
        *st.thread_pending |= b;
    } else {
        /* Process scope: last occurrence wins; a no-info queue clears the
         * has-info flag. */
        let update_info = a20_signal_queue_update_info(0, thread_bit_set);
        let info_has = a20_signal_queue_info_has(0, thread_bit_set, info_present);
        if update_info != 0 {
            if info_present != 0 {
                write_info_slot(st.info_slot(signum as usize, false), info, info_size);
            }
            *st.has_info_slot(signum as usize, false) = info_has as u8;
        }
        *st.process_pending |= b;
    }

    result.fatal = a20_signal_action_fatal(blocked, handler, signum);
    result.deliverable = a20_signal_action_deliverable(blocked, handler, signum);
    result.sigwait_match = (sigwait_active != 0 && (sigwait_mask & b) != 0) as i32;
    0
}

/// Per-task scalar slots reset by exec (C adapter passes field addresses).
#[repr(C)]
pub struct A20SignalTaskSlots {
    pub sig_handling: *mut i32,
    pub sigsuspend_active: *mut i32,
    pub sigwait_active: *mut i32,
    pub sigwait_mask: *mut u64,
}

/// Read a sigaction field from the C actions array without mirroring the
/// struct layout: `handler_size` is sizeof(uintptr_t) on the target.
#[inline]
unsafe fn read_action_handler(actions: *const u8, index: usize, stride: usize, off: usize, handler_size: usize) -> u64 {
    let p = actions.add(index * stride + off);
    if handler_size == 4 {
        u64::from(*(p as *const u32))
    } else {
        *(p as *const u64)
    }
}

#[inline]
unsafe fn write_action_handler(actions: *mut u8, index: usize, stride: usize, off: usize, handler_size: usize, value: u64) {
    let p = actions.add(index * stride + off);
    if handler_size == 4 {
        *(p as *mut u32) = value as u32;
    } else {
        *(p as *mut u64) = value;
    }
}

#[inline]
unsafe fn read_action_u32(actions: *const u8, index: usize, stride: usize, off: usize) -> u32 {
    *(actions.add(index * stride + off) as *const u32)
}

#[inline]
unsafe fn read_action_u64(actions: *const u8, index: usize, stride: usize, off: usize) -> u64 {
    *(actions.add(index * stride + off) as *const u64)
}

#[inline]
unsafe fn write_action_zeroes(actions: *mut u8, index: usize, stride: usize, off: usize, size: usize) {
    let p = actions.add(index * stride + off);
    let mut i = 0;
    while i < size {
        *p.add(i) = 0;
        i += 1;
    }
}

/// The exec transaction: reset every disposition to SIG_DFL except an
/// explicit SIG_IGN (clearing flags and mask), drop all pending state from
/// both scopes with their siginfo, and clear the per-task signal scalars.
/// `handler_off`/`mask_off`/`flags_off`/`handler_size` describe the C
/// sigaction_t layout; the adapter passes offsetof/sizeof values.
#[no_mangle]
pub unsafe extern "C" fn a20_signal_exec_reset_locked(
    st: *mut A20SignalQueueState,
    actions: *mut u8,
    actions_count: usize,
    action_stride: usize,
    handler_off: usize,
    handler_size: usize,
    mask_off: usize,
    flags_off: usize,
    slots: *mut A20SignalTaskSlots,
) -> i32 {
    if st.is_null() || actions.is_null() || slots.is_null() || actions_count > NSIG {
        return -EINVAL;
    }
    let st = &*st;
    /* Signum 0 has no disposition; the C reference loop starts at 1. */
    let mut i = 1;
    while i < actions_count {
        let handler = read_action_handler(actions, i, action_stride, handler_off, handler_size);
        write_action_handler(actions, i, action_stride, handler_off, handler_size,
                             a20_signal_exec_action_reset(handler));
        write_action_zeroes(actions, i, action_stride, mask_off, 8);
        write_action_zeroes(actions, i, action_stride, flags_off, 4);
        i += 1;
    }

    *st.process_pending = 0;
    let mut s = 0;
    while s < NSIG {
        *st.process_has_info.add(s) = 0;
        let mut b = 0;
        while b < SIGNAL_INFO_SIZE {
            *st.process_info.add(s * SIGNAL_INFO_SIZE + b) = 0;
            b += 1;
        }
        s += 1;
    }
    *st.thread_pending = 0;
    if !st.thread_has_info.is_null() {
        let mut s = 0;
        while s < NSIG {
            *st.thread_has_info.add(s) = 0;
            let mut b = 0;
            while b < SIGNAL_INFO_SIZE {
                *st.thread_info.add(s * SIGNAL_INFO_SIZE + b) = 0;
                b += 1;
            }
            s += 1;
        }
    }

    let slots = &*slots;
    if !slots.sig_handling.is_null() {
        *slots.sig_handling = 0;
    }
    if !slots.sigsuspend_active.is_null() {
        *slots.sigsuspend_active = 0;
    }
    if !slots.sigwait_active.is_null() {
        *slots.sigwait_active = 0;
    }
    if !slots.sigwait_mask.is_null() {
        *slots.sigwait_mask = 0;
    }
    0
}

/* ---- Delivery selection transaction ---- */

pub const A20_DELIVERY_NONE: i32 = 0;
pub const A20_DELIVERY_DROP_IGNORED: i32 = 1;
pub const A20_DELIVERY_STOP: i32 = 2;
pub const A20_DELIVERY_FATAL: i32 = 3;
pub const A20_DELIVERY_HANDLER: i32 = 4;
pub const A20_DELIVERY_PTRACE_STOP: i32 = 5;

/// In/out record for one delivery-selection iteration.  The C adapter fills
/// the inputs while holding the signal_state lock, calls
/// a20_signal_delivery_select_locked(), then stores the mutated scalars back
/// and acts on `outcome` (stop / fatal / build frame / ptrace stop).
#[repr(C)]
pub struct A20SignalDeliveryTxn {
    pub blocked: u64,                 /* in/out */
    pub sigsuspend_active: i32,       /* in/out */
    pub sigsuspend_old_blocked: u64,  /* in */
    pub user: i32,                    /* in: user-context delivery */
    pub traced: i32,                  /* in: proc_debug_is_traced */
    pub ptrace_deliver_sig: i32,      /* in/out: one-shot resume marker */
    pub sig: i32,                     /* out */
    pub outcome: i32,                 /* out: A20_DELIVERY_* */
    pub handler: u64,                 /* out */
    pub flags: i32,                   /* out: sa_flags */
    pub sa_mask: u64,                 /* out: sa_mask */
    pub old_blocked: u64,             /* out: mask to save in the frame */
    pub sig_handling: i32,            /* out */
    pub reset_handler: i32,           /* out: C applies the RESETHAND store */
    pub has_info: i32,                /* out: queued info was copied */
}

/// Select the next signal to deliver and perform the whole per-iteration
/// state transition: ignored-occurrence consumption (with the sigsuspend
/// mask restore), ptrace interception, default stop / fatal consumption,
/// and the handler path (info copy, consume, RESETHAND decision, mask
/// composition, sig_handling).  Mirrors the C reference loop bodies in
/// signal_deliver() and signal_deliver_user() exactly, including the
/// ordering of the sigsuspend restore versus the `|=` mask composition.
#[no_mangle]
pub unsafe extern "C" fn a20_signal_delivery_select_locked(
    st: *mut A20SignalQueueState,
    actions: *const u8,
    actions_count: usize,
    action_stride: usize,
    handler_off: usize,
    handler_size: usize,
    mask_off: usize,
    flags_off: usize,
    txn: *mut A20SignalDeliveryTxn,
    info_out: *mut u8,
    info_out_size: usize,
) -> i32 {
    if st.is_null() || actions.is_null() || txn.is_null() || actions_count > NSIG {
        return -EINVAL;
    }
    let st = &*st;
    let txn = &mut *txn;
    txn.sig = 0;
    txn.outcome = A20_DELIVERY_NONE;
    txn.handler = 0;
    txn.flags = 0;
    txn.sa_mask = 0;
    txn.old_blocked = 0;
    txn.sig_handling = 0;
    txn.reset_handler = 0;
    txn.has_info = 0;

    let deliverable = (*st.process_pending | *st.thread_pending) & !txn.blocked;
    let sig = a20_signal_lowest(deliverable);
    if sig == 0 {
        return 0;
    }
    let su = sig as usize;
    let b = 1u64 << su;
    let handler = read_action_handler(actions, su, action_stride, handler_off, handler_size);
    let flags = read_action_u32(actions, su, action_stride, flags_off);
    txn.sig = sig;

    /* Kernel-context delivery asks deliverability with an empty mask. */
    let action_blocked = if txn.user != 0 { txn.blocked } else { 0 };
    if a20_signal_action_deliverable(action_blocked, handler, sig) == 0 {
        clear_one_scope(st, su);
        if txn.user != 0 && txn.sigsuspend_active != 0 {
            txn.blocked = txn.sigsuspend_old_blocked;
            txn.sigsuspend_active = 0;
        }
        txn.outcome = A20_DELIVERY_DROP_IGNORED;
        return 0;
    }

    /* PTRACE_DELIVERY_BOUNDARY: a traced task intercepts every deliverable
     * signal except SIGKILL as a ptrace signal-stop. */
    if txn.user != 0 && txn.traced != 0 && sig != SIGKILL {
        if txn.ptrace_deliver_sig != sig {
            clear_one_scope(st, su);
            txn.outcome = A20_DELIVERY_PTRACE_STOP;
            return 0;
        }
        txn.ptrace_deliver_sig = 0;
    }

    if handler == SIG_DFL {
        clear_one_scope(st, su);
        if a20_signal_default_stop(sig) != 0 {
            if txn.user != 0 && txn.sigsuspend_active != 0 {
                txn.blocked = txn.sigsuspend_old_blocked;
                txn.sigsuspend_active = 0;
            }
            txn.outcome = A20_DELIVERY_STOP;
        } else {
            txn.outcome = A20_DELIVERY_FATAL;
        }
        return 0;
    }

    /* Handler path: copy queued info, consume, apply the action lifecycle. */
    let has_info = peek_locked_ref(st, sig, info_out, info_out_size) == 0;
    clear_one_scope(st, su);
    txn.handler = handler;
    txn.flags = flags as i32;
    txn.sa_mask = read_action_u64(actions, su, action_stride, mask_off);
    txn.has_info = has_info as i32;
    if txn.user != 0 {
        if flags & SA_RESETHAND != 0 {
            txn.reset_handler = 1;
        }
        txn.old_blocked = if txn.sigsuspend_active != 0 {
            txn.sigsuspend_old_blocked
        } else {
            txn.blocked
        };
        /* Mask composition keeps the C reference's `|=`: the temporary
         * sigsuspend mask is still installed at this point. */
        txn.blocked |= txn.sa_mask;
        if flags & SA_NODEFER == 0 {
            txn.blocked |= b;
        }
        txn.sigsuspend_active = 0;
        txn.sig_handling = sig;
    }
    txn.outcome = A20_DELIVERY_HANDLER;
    0
}

/* ---- signalfd record + batch consumption ---- */

/// Linux signalfd_siginfo layout (128 bytes, kernel/include-fixed).
#[repr(C)]
struct SignalFdSiginfo {
    ssi_signo: u32,
    ssi_errno: i32,
    ssi_code: i32,
    ssi_pid: u32,
    ssi_uid: u32,
    ssi_fd: i32,
    ssi_tid: u32,
    ssi_band: u32,
    ssi_overrun: u32,
    ssi_trapno: u32,
    ssi_status: i32,
    ssi_int: i32,
    ssi_ptr: u64,
    ssi_utime: u64,
    ssi_stime: u64,
    ssi_addr: u64,
    ssi_addr_lsb: u16,
    _pad2: u16,
    ssi_syscall: i32,
    ssi_call_addr: u64,
    ssi_arch: u32,
    _pad: [u8; 28],
}

const SIGNAL_FD_RECORD_SIZE: usize = core::mem::size_of::<SignalFdSiginfo>();

/// Format one 128-byte signalfd record from a consumed occurrence.  The
/// queued arch_siginfo supplies signo/errno/code and the pid/uid sifields
/// (at `sifields_off`, which is 16 on the 64-bit arches and 12 on the
/// 32-bit ones); a no-info occurrence yields a zeroed record with only
/// ssi_signo set.
#[inline]
unsafe fn format_fd_record(record: *mut u8, sig: i32, has_info: bool, info: *const u8,
                           sifields_off: usize) {
    let mut r = core::mem::MaybeUninit::<SignalFdSiginfo>::zeroed();
    let r = r.as_mut_ptr();
    (*r).ssi_signo = sig as u32;
    if has_info {
        (*r).ssi_errno = *(info.add(4) as *const i32);
        (*r).ssi_code = *(info.add(8) as *const i32);
        (*r).ssi_pid = *(info.add(sifields_off) as *const u32);
        (*r).ssi_uid = *(info.add(sifields_off + 4) as *const u32);
    }
    let bytes = core::slice::from_raw_parts(r as *const u8, SIGNAL_FD_RECORD_SIZE);
    let mut i = 0;
    while i < SIGNAL_FD_RECORD_SIZE {
        *record.add(i) = bytes[i];
        i += 1;
    }
}

/// Dequeue up to `capacity` pending signals matching `mask` in ONE locked
/// transaction, formatting a signalfd record per consumed occurrence
/// (lowest signum first).  Returns the record count, 0 when nothing
/// matches, or -EINVAL when record_size is not the ABI record size.
#[no_mangle]
pub unsafe extern "C" fn a20_signal_fd_read_locked(
    st: *mut A20SignalQueueState,
    mask: u64,
    records: *mut u8,
    capacity: usize,
    record_size: usize,
    sifields_off: usize,
) -> i64 {
    if st.is_null() || (capacity > 0 && records.is_null()) {
        return -EINVAL as i64;
    }
    if record_size != SIGNAL_FD_RECORD_SIZE {
        return -EINVAL as i64;
    }
    let st = &*st;
    let mut produced: i64 = 0;
    while (produced as usize) < capacity {
        let candidates = (*st.process_pending | *st.thread_pending) & mask & !1;
        let sig = a20_signal_lowest(candidates);
        if sig == 0 {
            break;
        }
        let mut info = [0u8; SIGNAL_INFO_SIZE];
        let has_info = a20_signal_consume_locked(st as *const A20SignalQueueState as *mut A20SignalQueueState, sig, info.as_mut_ptr(), info.len()) == 0;
        let record = records.add(produced as usize * record_size);
        format_fd_record(record, sig, has_info, info.as_ptr(), sifields_off);
        produced += 1;
    }
    produced
}

/// Select the lowest pending signal matching `mask` (sigwait semantics:
/// blocked signals match too) and consume it in one transaction.  Returns
/// the signal number or 0; `has_info_out` reports whether queued siginfo
/// was copied into `out`.
#[no_mangle]
pub unsafe extern "C" fn a20_signal_take_matching_locked(
    st: *mut A20SignalQueueState,
    mask: u64,
    out: *mut u8,
    out_size: usize,
    has_info_out: *mut i32,
) -> i32 {
    if st.is_null() {
        return 0;
    }
    if !has_info_out.is_null() {
        *has_info_out = 0;
    }
    let st = &*st;
    let candidates = (*st.process_pending | *st.thread_pending) & mask;
    let sig = a20_signal_lowest(candidates);
    if sig == 0 {
        return 0;
    }
    let has_info = a20_signal_consume_locked(st as *const A20SignalQueueState as *mut A20SignalQueueState, sig, out, out_size) == 0;
    if !has_info_out.is_null() {
        *has_info_out = has_info as i32;
    }
    sig
}

/* ---- Native checkpoint consuming transactions ---- */

/// Native thread-check transaction with the consumption applied in Rust:
/// returns the deliverable set and, unless A20_SIGNAL_PEEK is set, clears
/// the consumed occurrences (thread occurrence preferred, lowest-signum
/// SELECT consume) from both pointed masks.
#[no_mangle]
pub unsafe extern "C" fn a20_signal_native_thread_check_locked(
    pending_ht: *mut u64,
    pending_thread: *mut u64,
    blocked: u64,
    mode: u32,
    selected: u64,
) -> u64 {
    if pending_ht.is_null() || pending_thread.is_null() {
        return 0;
    }
    let mut clear_thread = 0u64;
    let mut clear_ht = 0u64;
    let deliver = a20_signal_native_thread_check(
        *pending_ht, *pending_thread, blocked, mode, selected,
        &mut clear_thread, &mut clear_ht);
    *pending_thread &= !clear_thread;
    *pending_ht &= !clear_ht;
    deliver
}

/// Native handle-table take with the consumption applied in Rust:
/// deliverable signals are returned and cleared; blocked ones stay pending.
#[no_mangle]
pub unsafe extern "C" fn a20_signal_native_ht_take_locked(
    pending: *mut u64,
    blocked: u64,
) -> u64 {
    if pending.is_null() {
        return 0;
    }
    let deliver = *pending & !blocked;
    *pending &= blocked;
    deliver
}

/// Native checkpoint pend transactions (single-bit queue mutation).
#[no_mangle]
pub unsafe extern "C" fn a20_signal_native_thread_pend_locked(
    pending_thread: *mut u64,
    sig: i32,
) {
    if !pending_thread.is_null() && sig > 0 && sig < NSIG as i32 {
        *pending_thread |= bit(sig);
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_signal_native_ht_pend_locked(
    pending_ht: *mut u64,
    sig: i32,
) {
    if !pending_ht.is_null() && sig > 0 && sig < NSIG as i32 {
        *pending_ht |= bit(sig);
    }
}

/* ============================================================
 * signalfd readiness / batching
 * ============================================================ */

/// A signalfd read is ready when a matching signal is pending.
#[no_mangle]
pub extern "C" fn a20_signal_fd_ready(pending_union: u64, mask: u64) -> i32 {
    (pending_union & mask & !1 != 0) as i32
}

/// A signalfd read length must be at least one record.
#[no_mangle]
pub extern "C" fn a20_signal_fd_read_valid(count: u64, record_size: u64) -> i32 {
    (record_size != 0 && count >= record_size) as i32
}

/// Number of full records a signalfd read of `count` bytes can return.
#[no_mangle]
pub extern "C" fn a20_signal_fd_read_records(count: u64, record_size: u64) -> u64 {
    if record_size == 0 {
        0
    } else {
        count / record_size
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /* ---- C-oracle encodings copied verbatim from kernel/proc/signal.c at
     * the frozen baseline so a table drift cannot pass silently. ---- */
    fn c_oracle_core_dump(sig: i32) -> i32 {
        matches!(sig, 3 | 4 | 6 | 7 | 8 | 11 | 31) as i32
    }
    fn c_oracle_terminate(sig: i32) -> i32 {
        !matches!(sig, 17 | 23 | 28 | 19 | 20 | 21 | 22 | 18) as i32
    }
    fn c_oracle_stop(sig: i32) -> i32 {
        matches!(sig, 19 | 20 | 21 | 22) as i32
    }
    fn c_oracle_ignore(sig: i32) -> i32 {
        matches!(sig, 17 | 23 | 28 | 18) as i32
    }

    #[test]
    fn default_action_tables_match_c_reference_for_every_signo() {
        for sig in 1..64 {
            assert_eq!(a20_signal_core_dump_default(sig), c_oracle_core_dump(sig), "core {sig}");
            assert_eq!(a20_signal_default_terminate(sig), c_oracle_terminate(sig), "term {sig}");
            assert_eq!(a20_signal_default_stop(sig), c_oracle_stop(sig), "stop {sig}");
            assert_eq!(a20_signal_default_ignore(sig), c_oracle_ignore(sig), "ign {sig}");
        }
    }

    #[test]
    fn core_dump_set_is_exactly_the_linux_default() {
        let dumpers: [i32; 7] = [3, 4, 6, 7, 8, 11, 31];
        for sig in 1..64 {
            assert_eq!(a20_signal_core_dump_default(sig), dumpers.contains(&sig) as i32);
        }
    }

    #[test]
    fn wait_status_sets_wcoredump_only_when_dumped() {
        assert_eq!(a20_signal_wait_status_dumped(11, 0), 11);
        assert_eq!(a20_signal_wait_status_dumped(11, 1), 11 | 0x80);
        assert_eq!(a20_signal_wait_status_dumped(15, 1), 15 | 0x80);
        assert_eq!(a20_signal_fatal_exit_code_dumped(11, 1), -(11 | 0x80));
        assert_eq!(a20_signal_fatal_exit_code_dumped(9, 0), -9);
    }

    #[test]
    fn mask_apply_blocks_unblocks_and_sets() {
        let m10 = bit(10);
        let m12 = bit(12);
        assert_eq!(a20_signal_mask_apply(SIG_BLOCK, 0, m10), m10);
        assert_eq!(a20_signal_mask_apply(SIG_BLOCK, m12, m10), m10 | m12);
        assert_eq!(a20_signal_mask_apply(SIG_UNBLOCK, m10 | m12, m10), m12);
        assert_eq!(a20_signal_mask_apply(SIG_SETMASK, m10 | m12, m12), m12);
    }

    #[test]
    fn mask_apply_passes_operand_bits_through_unchanged() {
        let kill = bit(SIGKILL);
        let stop = bit(SIGSTOP);
        assert_eq!(a20_signal_mask_apply(SIG_BLOCK, 0, kill), kill);
        assert_eq!(a20_signal_mask_apply(SIG_SETMASK, kill | stop, 0), 0);
        assert_eq!(a20_signal_mask_apply(SIG_UNBLOCK, kill, kill), 0);
    }

    #[test]
    fn mask_apply_invalid_how_is_identity() {
        assert_eq!(a20_signal_mask_apply(3, bit(5), bit(7)), bit(5));
        assert_eq!(a20_signal_mask_apply(-1, bit(5), bit(7)), bit(5));
    }

    #[test]
    fn mask_strip_removes_only_kill_and_stop() {
        let m = bit(9) | bit(19) | bit(10) | 1;
        assert_eq!(a20_signal_mask_strip_unblockable(m), bit(10) | 1);
        assert_eq!(a20_signal_temporary_mask(m), bit(10) | 1);
    }

    #[test]
    fn deliverable_mask_unions_scopes_and_removes_blocked() {
        assert_eq!(a20_signal_deliverable_mask(bit(2), bit(10), bit(2)), bit(10));
        assert_eq!(a20_signal_deliverable_mask(bit(2) | bit(15), 0, bit(15)), bit(2));
        assert_eq!(a20_signal_deliverable_mask(u64::MAX, u64::MAX, 0), u64::MAX);
        assert_eq!(a20_signal_deliverable_mask(0, 0, u64::MAX), 0);
        assert_eq!(a20_signal_pending_union(bit(2), bit(10)), bit(2) | bit(10));
        assert_eq!(a20_signal_pending_blocked(bit(2) | bit(10), 0, bit(10)), bit(10));
    }

    #[test]
    fn lowest_ignores_bit_zero_and_reports_first_signum() {
        assert_eq!(a20_signal_lowest(0), 0);
        assert_eq!(a20_signal_lowest(1), 0);
        assert_eq!(a20_signal_lowest(bit(31) | bit(5) | bit(9)), 5);
        assert_eq!(a20_signal_lowest(1u64 << 63), 63);
    }

    #[test]
    fn action_deliverable_respects_block_ignore_and_default_ignore() {
        let s = 10;
        assert_eq!(a20_signal_action_deliverable(0, SIG_DFL, s), 1);
        assert_eq!(a20_signal_action_deliverable(bit(s), SIG_DFL, s), 0);
        assert_eq!(a20_signal_action_deliverable(0, SIG_IGN, s), 0);
        assert_eq!(a20_signal_action_deliverable(0, 0x4000, s), 1);
        assert_eq!(a20_signal_action_deliverable(0, SIG_DFL, SIGCHLD), 0);
        assert_eq!(a20_signal_action_deliverable(0, 0x4000, SIGCHLD), 1);
        assert_eq!(a20_signal_action_deliverable(0, SIG_DFL, SIGCONT), 0);
        assert_eq!(a20_signal_action_deliverable(bit(SIGCONT), 0x4000, SIGCONT), 0);
    }

    #[test]
    fn action_fatal_only_for_deliverable_default_terminate() {
        let segv = SIGSEGV;
        assert_eq!(a20_signal_action_fatal(0, SIG_DFL, segv), 1);
        assert_eq!(a20_signal_action_fatal(bit(segv), SIG_DFL, segv), 0);
        assert_eq!(a20_signal_action_fatal(0, SIG_IGN, segv), 0);
        assert_eq!(a20_signal_action_fatal(0, 0x4000, segv), 0);
        assert_eq!(a20_signal_action_fatal(0, SIG_DFL, SIGCHLD), 0);
        assert_eq!(a20_signal_action_fatal(0, SIG_DFL, SIGSTOP), 0);
        assert_eq!(a20_signal_action_fatal(0, SIG_DFL, SIGCONT), 0);
        assert_eq!(a20_signal_action_fatal(0, SIG_DFL, SIGKILL), 1);
    }

    #[test]
    fn user_handler_available_requires_real_unblocked_handler() {
        assert_eq!(a20_signal_action_user_handler_available(0x4000, 0), 1);
        assert_eq!(a20_signal_action_user_handler_available(0x4000, 1), 0);
        assert_eq!(a20_signal_action_user_handler_available(SIG_DFL, 0), 0);
        assert_eq!(a20_signal_action_user_handler_available(SIG_IGN, 0), 0);
    }

    #[test]
    fn generation_cancel_rules_are_symmetric_and_scoped() {
        let stops = bit(SIGSTOP) | bit(SIGTSTP) | bit(SIGTTIN) | bit(SIGTTOU);
        assert_eq!(a20_signal_generation_cancel_mask(SIGCONT), stops);
        for s in [SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU] {
            assert_eq!(a20_signal_generation_cancel_mask(s), bit(SIGCONT));
        }
        assert_eq!(a20_signal_generation_cancel_mask(SIGSEGV), 0);
        assert_eq!(a20_signal_generation_cancel_mask(SIGCHLD), 0);
        assert_eq!(a20_signal_generation_cancel_mask(0), 0);
    }

    /* ---- Queue coalescing policy (D4 asymmetry) ---- */

    #[test]
    fn queue_discard_drops_ignored_except_cont() {
        assert_eq!(a20_signal_queue_discard(SIGCHLD, SIG_DFL), 1);
        assert_eq!(a20_signal_queue_discard(SIGURG, SIG_DFL), 1);
        assert_eq!(a20_signal_queue_discard(SIGCONT, SIG_DFL), 0);
        assert_eq!(a20_signal_queue_discard(10, SIG_IGN), 1);
        assert_eq!(a20_signal_queue_discard(SIGCONT, SIG_IGN), 0);
        assert_eq!(a20_signal_queue_discard(10, SIG_DFL), 0);
        assert_eq!(a20_signal_queue_discard(10, 0x4000), 0);
    }

    #[test]
    fn queue_info_policy_is_first_wins_thread_last_wins_process() {
        /* Thread scope: only the first occurrence records info. */
        assert_eq!(a20_signal_queue_update_info(1, 0), 1);
        assert_eq!(a20_signal_queue_update_info(1, 1), 0);
        assert_eq!(a20_signal_queue_info_has(1, 0, 1), 1);
        assert_eq!(a20_signal_queue_info_has(1, 1, 1), 0);
        assert_eq!(a20_signal_queue_info_has(1, 0, 0), 0);
        /* Process scope: last-wins, a no-info queue clears the flag. */
        assert_eq!(a20_signal_queue_update_info(0, 0), 1);
        assert_eq!(a20_signal_queue_update_info(0, 1), 1);
        assert_eq!(a20_signal_queue_info_has(0, 1, 1), 1);
        assert_eq!(a20_signal_queue_info_has(0, 1, 0), 0);
    }

    #[test]
    fn peek_scope_prefers_thread_and_reports_missing_info() {
        assert_eq!(a20_signal_peek_scope(1, 1, 1, 1), 0);
        assert_eq!(a20_signal_peek_scope(1, 1, 1, 0), 0);
        assert_eq!(a20_signal_peek_scope(1, 0, 1, 1), -ENOENT);
        assert_eq!(a20_signal_peek_scope(1, 1, 0, 1), -ENOENT);
        assert_eq!(a20_signal_peek_scope(0, 0, 0, 1), 1);
        assert_eq!(a20_signal_peek_scope(0, 0, 0, 0), -ENOENT);
        assert_eq!(a20_signal_clear_scope(1), 0);
        assert_eq!(a20_signal_clear_scope(0), 1);
    }

    /* ---- Restart decision (R group) ---- */

    #[test]
    fn restart_skip_and_veto_policy() {
        assert_eq!(a20_signal_restart_skips(SIG_IGN, 10), 1);
        assert_eq!(a20_signal_restart_skips(SIG_DFL, SIGCHLD), 1);
        assert_eq!(a20_signal_restart_skips(SIG_DFL, SIGSEGV), 0);
        assert_eq!(a20_signal_restart_skips(0x4000, 10), 0);
        assert_eq!(a20_signal_restart_vetoes(0x4000, 0), 1);
        assert_eq!(a20_signal_restart_vetoes(0x4000, SA_RESTART), 0);
        assert_eq!(a20_signal_restart_vetoes(SIG_DFL, 0), 0);
        assert_eq!(a20_signal_restart_vetoes(SIG_IGN, 0), 0);
    }

    /* ---- Action lifecycle ---- */

    #[test]
    fn sigchld_and_continue_lifecycle() {
        assert_eq!(a20_signal_sigchld_auto_reap(SIG_IGN, 0), 1);
        assert_eq!(a20_signal_sigchld_auto_reap(0x4000, SA_NOCLDWAIT), 1);
        assert_eq!(a20_signal_sigchld_auto_reap(0x4000, 0), 0);
        assert_eq!(a20_signal_sigchld_no_cldstop(SA_NOCLDSTOP), 1);
        assert_eq!(a20_signal_sigchld_no_cldstop(0), 0);
        assert_eq!(a20_signal_continue_pending(bit(SIGCONT)), 1);
        assert_eq!(a20_signal_continue_pending(bit(SIGCONT) | bit(10)), 1);
        assert_eq!(a20_signal_continue_pending(bit(10)), 0);
    }

    #[test]
    fn sigsuspend_restore_and_exec_reset() {
        assert_eq!(a20_signal_sigsuspend_restore_active(1, 0), 1);
        assert_eq!(a20_signal_sigsuspend_restore_active(1, 1), 0);
        assert_eq!(a20_signal_sigsuspend_restore_active(0, 0), 0);
        assert_eq!(a20_signal_exec_action_reset(0x4000), SIG_DFL);
        assert_eq!(a20_signal_exec_action_reset(SIG_IGN), SIG_IGN);
        assert_eq!(a20_signal_exec_action_reset(SIG_DFL), SIG_DFL);
    }

    /* ---- Syscall validation ---- */

    #[test]
    fn action_and_procmask_validation() {
        assert_eq!(a20_signal_action_valid(10, 8, 8), 0);
        assert_eq!(a20_signal_action_valid(0, 8, 8), -EINVAL);
        assert_eq!(a20_signal_action_valid(64, 8, 8), -EINVAL);
        assert_eq!(a20_signal_action_valid(SIGKILL, 8, 8), -EINVAL);
        assert_eq!(a20_signal_action_valid(SIGSTOP, 8, 8), -EINVAL);
        assert_eq!(a20_signal_action_valid(10, 4, 8), -EINVAL);
        assert_eq!(a20_signal_procmask_how_valid(0), 0);
        assert_eq!(a20_signal_procmask_how_valid(2), 0);
        assert_eq!(a20_signal_procmask_how_valid(3), -EINVAL);
        assert_eq!(a20_signal_procmask_how_valid(-1), -EINVAL);
    }

    #[test]
    fn kill_permission_matches_linux_uid_rule() {
        assert_eq!(a20_signal_kill_permission(1, 1, 1, 2, 2), 0);
        assert_eq!(a20_signal_kill_permission(0, 7, 7, 7, 9), 0);
        assert_eq!(a20_signal_kill_permission(0, 7, 7, 9, 7), 0);
        assert_eq!(a20_signal_kill_permission(0, 7, 7, 9, 9), -EPERM);
        assert_eq!(a20_signal_kill_permission(0, 7, 8, 9, 8), 0);
    }

    #[test]
    fn sigaltstack_validation() {
        assert_eq!(a20_signal_sigaltstack_valid(0, 4096, 2048), 0);
        assert_eq!(a20_signal_sigaltstack_valid(SS_DISABLE, 0, 2048), 0);
        assert_eq!(a20_signal_sigaltstack_valid(4, 4096, 2048), -EINVAL);
        assert_eq!(a20_signal_sigaltstack_valid(0, 1024, 2048), -ENOMEM);
    }

    #[test]
    fn native_validation_helpers() {
        assert_eq!(a20_signal_native_kill_valid(0, 10, 64), 0);
        assert_eq!(a20_signal_native_kill_valid(A20_SIGNAL_THREAD as u64, 10, 64), 0);
        assert_eq!(a20_signal_native_kill_valid(8, 10, 64), 1);
        assert_eq!(a20_signal_native_kill_valid(0, 64, 64), 1);
        assert_eq!(a20_signal_native_kill_valid(0, -1, 64), 1);
        /* signal_check allows THREAD|PEEK|SELECT, signal_mask THREAD|PEEK. */
        assert_eq!(a20_signal_native_mode_valid(0, 7), 0);
        assert_eq!(a20_signal_native_mode_valid(1, 7), 0);
        assert_eq!(a20_signal_native_mode_valid(7, 7), 0);
        assert_eq!(a20_signal_native_mode_valid(8, 7), 1);
        assert_eq!(a20_signal_native_mode_valid(2, 7), 1);
        assert_eq!(a20_signal_native_control_valid(0, 1, 0, 1, 7, 3), 0);
        assert_eq!(a20_signal_native_control_valid(1, 2, 0, 1, 7, 3), 0);
        assert_eq!(a20_signal_native_control_valid(0, 8, 0, 1, 7, 3), 1);
        assert_eq!(a20_signal_native_control_valid(1, 4, 0, 1, 7, 3), 1);
        assert_eq!(a20_signal_native_control_valid(2, 0, 0, 1, 7, 3), 1);
    }

    /* ---- Native thread checkpoint transaction (N group) ---- */

    #[test]
    fn native_thread_check_prefers_thread_and_consumes_lowest() {
        let mut ct = 0u64;
        let mut ch = 0u64;
        unsafe {
            /* Thread occurrence preferred; process occurrence stays queued. */
            let deliver = a20_signal_native_thread_check(
                bit(10), bit(10), 0, 0, 0, &mut ct, &mut ch);
            assert_eq!(deliver, bit(10));
            assert_eq!(ct, bit(10));
            assert_eq!(ch, 0);
            /* SELECT consumes one lowest signal even if blocked. */
            let deliver = a20_signal_native_thread_check(
                bit(12), bit(10), bit(10) | bit(12), A20_SIGNAL_SELECT, bit(10) | bit(12),
                &mut ct, &mut ch);
            assert_eq!(deliver, bit(10));
            assert_eq!(ct, bit(10));
            assert_eq!(ch, 0);
            /* PEEK does not consume. */
            let deliver = a20_signal_native_thread_check(
                bit(12), 0, 0, A20_SIGNAL_PEEK, 0, &mut ct, &mut ch);
            assert_eq!(deliver, bit(12));
            assert_eq!(ct, 0);
            assert_eq!(ch, 0);
            /* Non-select consume clears process occurrences when no thread
             * occurrence exists. */
            let deliver = a20_signal_native_thread_check(
                bit(12), 0, 0, 0, 0, &mut ct, &mut ch);
            assert_eq!(deliver, bit(12));
            assert_eq!(ct, 0);
            assert_eq!(ch, bit(12));
            /* Blocked union without SELECT is not deliverable. */
            let deliver = a20_signal_native_thread_check(
                bit(12), 0, bit(12), 0, 0, &mut ct, &mut ch);
            assert_eq!(deliver, 0);
        }
    }

    #[test]
    fn native_masks_keep_the_two_documented_asymmetries() {
        let m = bit(SIGKILL) | bit(SIGSTOP) | 1 | bit(10);
        assert_eq!(a20_signal_native_thread_mask(m), bit(10));
        assert_eq!(a20_signal_native_ht_mask(m), bit(SIGSTOP) | 1 | bit(10));
    }

    /* ---- signalfd ---- */

    #[test]
    fn signalfd_readiness_validity_and_batch() {
        assert_eq!(a20_signal_fd_ready(bit(10), bit(10)), 1);
        assert_eq!(a20_signal_fd_ready(bit(10), bit(12)), 0);
        assert_eq!(a20_signal_fd_ready(1, u64::MAX), 0);
        assert_eq!(a20_signal_fd_read_valid(128, 128), 1);
        assert_eq!(a20_signal_fd_read_valid(127, 128), 0);
        assert_eq!(a20_signal_fd_read_records(384, 128), 3);
        assert_eq!(a20_signal_fd_read_records(383, 128), 2);
        assert_eq!(a20_signal_fd_read_records(128, 0), 0);
    }

    /* ---- State transactions ---- */

    struct TxnState {
        process_pending: u64,
        process_has_info: [u8; 64],
        process_info: [[u8; 128]; 64],
        thread_pending: u64,
        thread_has_info: [u8; 64],
        thread_info: [[u8; 128]; 64],
    }

    impl TxnState {
        fn new() -> Self {
            TxnState {
                process_pending: 0,
                process_has_info: [0; 64],
                process_info: [[0; 128]; 64],
                thread_pending: 0,
                thread_has_info: [0; 64],
                thread_info: [[0; 128]; 64],
            }
        }
        fn st(&mut self) -> A20SignalQueueState {
            A20SignalQueueState {
                process_pending: &mut self.process_pending,
                process_has_info: self.process_has_info.as_mut_ptr(),
                process_info: self.process_info.as_mut_ptr() as *mut u8,
                thread_pending: &mut self.thread_pending,
                thread_has_info: self.thread_has_info.as_mut_ptr(),
                thread_info: self.thread_info.as_mut_ptr() as *mut u8,
            }
        }
    }

    fn info_with(sig: i32, tag: u8) -> [u8; 128] {
        let mut info = [0u8; 128];
        info[0..4].copy_from_slice(&sig.to_ne_bytes());
        info[4] = tag;
        info[8] = tag.wrapping_add(1);
        info[12..16].copy_from_slice(&0x1122_3344u32.to_ne_bytes());
        info[16..20].copy_from_slice(&0x5566_7788u32.to_ne_bytes());
        info
    }

    #[test]
    fn queue_coalesces_thread_first_wins_process_last_wins() {
        let mut s = TxnState::new();
        let a = info_with(10, 1);
        let b = info_with(10, 2);
        let mut r = A20SignalQueueResult { discard: 0, fatal: 0, deliverable: 0, sigwait_match: 0 };
        unsafe {
            let mut st = s.st();
            a20_signal_queue_locked(&mut st, 10, a.as_ptr(), a.len(), SIG_DFL, 0, 1, 0, 0, &mut r);
            assert_eq!(r.discard, 0);
            a20_signal_queue_locked(&mut st, 10, b.as_ptr(), b.len(), SIG_DFL, 0, 1, 0, 0, &mut r);
            assert_eq!(s.thread_pending, bit(10));
            let mut out = [0u8; 128];
            assert_eq!(a20_signal_consume_locked(&mut st, 10, out.as_mut_ptr(), out.len()), 0);
            assert_eq!(out[4], 1, "thread scope keeps the first info");
        }
        let mut s = TxnState::new();
        let mut r = A20SignalQueueResult { discard: 0, fatal: 0, deliverable: 0, sigwait_match: 0 };
        unsafe {
            let mut st = s.st();
            a20_signal_queue_locked(&mut st, 10, a.as_ptr(), a.len(), SIG_DFL, 0, 0, 0, 0, &mut r);
            a20_signal_queue_locked(&mut st, 10, b.as_ptr(), b.len(), SIG_DFL, 0, 0, 0, 0, &mut r);
            let mut out = [0u8; 128];
            assert_eq!(a20_signal_consume_locked(&mut st, 10, out.as_mut_ptr(), out.len()), 0);
            assert_eq!(out[4], 2, "process scope is last-wins");
            /* A no-info queue clears the has-info flag. */
            a20_signal_queue_locked(&mut st, 10, core::ptr::null(), 0, SIG_DFL, 0, 0, 0, 0, &mut r);
            assert_eq!(s.process_has_info[10], 0);
        }
    }

    #[test]
    fn queue_discard_and_generation_cancel_both_scopes() {
        let mut s = TxnState::new();
        let mut r = A20SignalQueueResult { discard: 0, fatal: 0, deliverable: 0, sigwait_match: 0 };
        let i20 = info_with(20, 7);
        let i18 = info_with(18, 8);
        unsafe {
            let mut st = s.st();
            /* Explicitly ignored is discarded at generation. */
            a20_signal_queue_locked(&mut st, 10, core::ptr::null(), 0, SIG_IGN, 0, 0, 0, 0, &mut r);
            assert_eq!(r.discard, 1);
            assert_eq!(s.process_pending, 0);
            /* Default-ignored (SIGCHLD) discarded; SIGCONT never is. */
            a20_signal_queue_locked(&mut st, SIGCHLD, core::ptr::null(), 0, SIG_DFL, 0, 0, 0, 0, &mut r);
            assert_eq!(r.discard, 1);
            a20_signal_queue_locked(&mut st, SIGCONT, core::ptr::null(), 0, SIG_DFL, 0, 0, 0, 0, &mut r);
            assert_eq!(r.discard, 0);
            assert_eq!(s.process_pending, bit(SIGCONT));
            /* Pending stop signal in BOTH scopes; queuing SIGTSTP cancels the
             * pending SIGCONT (generation-cancel is symmetric). */
            a20_signal_queue_locked(&mut st, SIGTSTP, i20.as_ptr(), i20.len(), SIG_DFL, 0, 0, 0, 0, &mut r);
            assert_eq!(s.process_pending, bit(SIGTSTP), "SIGCONT cancelled by the stop signal");
            a20_signal_queue_locked(&mut st, SIGTSTP, i20.as_ptr(), i20.len(), SIG_DFL, 0, 1, 0, 0, &mut r);
            assert_eq!(s.process_pending, bit(SIGTSTP));
            assert_eq!(s.thread_pending, bit(SIGTSTP));
            a20_signal_queue_locked(&mut st, SIGCONT, i18.as_ptr(), i18.len(), SIG_DFL, 0, 0, 0, 0, &mut r);
            assert_eq!(s.process_pending & bit(SIGTSTP), 0);
            assert_eq!(s.thread_pending & bit(SIGTSTP), 0);
            assert_eq!(s.process_pending, bit(SIGCONT));
            assert_eq!(s.process_has_info[SIGTSTP as usize], 0);
            assert_eq!(s.thread_has_info[SIGTSTP as usize], 0);
            /* A stop signal cancels a pending SIGCONT. */
            a20_signal_queue_locked(&mut st, SIGSTOP, core::ptr::null(), 0, SIG_DFL, 0, 0, 0, 0, &mut r);
            assert_eq!(s.process_pending & bit(SIGCONT), 0);
        }
    }

    #[test]
    fn consume_prefers_thread_scope_and_reports_missing_info() {
        let mut s = TxnState::new();
        let pt = info_with(10, 3);
        let tt = info_with(10, 4);
        let mut r = A20SignalQueueResult { discard: 0, fatal: 0, deliverable: 0, sigwait_match: 0 };
        unsafe {
            let mut st = s.st();
            a20_signal_queue_locked(&mut st, 10, pt.as_ptr(), pt.len(), SIG_DFL, 0, 0, 0, 0, &mut r);
            a20_signal_queue_locked(&mut st, 10, tt.as_ptr(), tt.len(), SIG_DFL, 0, 1, 0, 0, &mut r);
            let mut out = [0u8; 128];
            assert_eq!(a20_signal_consume_locked(&mut st, 10, out.as_mut_ptr(), out.len()), 0);
            assert_eq!(out[4], 4, "thread occurrence wins");
            assert_eq!(s.thread_pending, 0);
            assert_eq!(s.process_pending, bit(10), "process occurrence stays queued");
            assert_eq!(a20_signal_consume_locked(&mut st, 10, out.as_mut_ptr(), out.len()), 0);
            assert_eq!(out[4], 3);
            /* No-info occurrence reports -ENOENT but is still consumed. */
            a20_signal_queue_locked(&mut st, 12, core::ptr::null(), 0, SIG_DFL, 0, 0, 0, 0, &mut r);
            assert_eq!(a20_signal_consume_locked(&mut st, 12, out.as_mut_ptr(), out.len()), -ENOENT);
            assert_eq!(s.process_pending & bit(12), 0);
            /* Peek does not consume. */
            a20_signal_queue_locked(&mut st, 13, tt.as_ptr(), tt.len(), SIG_DFL, 0, 0, 0, 0, &mut r);
            assert_eq!(a20_signal_peek_locked(&mut st, 13, out.as_mut_ptr(), out.len()), 0);
            assert_eq!(s.process_pending & bit(13), bit(13));
        }
    }

    #[test]
    fn delivery_select_runs_the_full_action_lifecycle() {
        let mut s = TxnState::new();
        let mut r = A20SignalQueueResult { discard: 0, fatal: 0, deliverable: 0, sigwait_match: 0 };
        let ih = info_with(10, 9);
        unsafe {
            let mut st = s.st();
            a20_signal_queue_locked(&mut st, 10, ih.as_ptr(), ih.len(), 0x4000, 0, 0, 0, 0, &mut r);
            let mut actions = [0u8; 64 * 24];
            let base = actions.as_mut_ptr();
            core::ptr::write_unaligned(base.add(10 * 24) as *mut u64, 0x4000u64);          /* handler */
            core::ptr::write_unaligned(base.add(10 * 24 + 8) as *mut u64, bit(21));         /* sa_mask */
            core::ptr::write_unaligned(base.add(10 * 24 + 16) as *mut u32, 0);              /* flags */
            let mut txn = A20SignalDeliveryTxn {
                blocked: 0, sigsuspend_active: 0, sigsuspend_old_blocked: 0,
                user: 1, traced: 0, ptrace_deliver_sig: 0, sig: 0, outcome: 0,
                handler: 0, flags: 0, sa_mask: 0, old_blocked: 0,
                sig_handling: 0, reset_handler: 0, has_info: 0,
            };
            let mut info = [0u8; 128];
            assert_eq!(a20_signal_delivery_select_locked(&mut st, actions.as_ptr(), 64, 24,
                0, 8, 8, 16, &mut txn, info.as_mut_ptr(), info.len()), 0);
            assert_eq!(txn.outcome, A20_DELIVERY_HANDLER);
            assert_eq!(txn.sig, 10);
            assert_eq!(txn.handler, 0x4000);
            assert_eq!(txn.sig_handling, 10);
            assert_eq!(txn.blocked, bit(10) | bit(21), "sa_mask + self NODEFER-off bit");
            assert_eq!(txn.has_info, 1);
            assert_eq!(info[4], 9);
            assert_eq!(s.process_pending, 0);

            /* Ignored occurrence: queued while handled, disposition changed to
             * SIG_IGN afterwards (the queue-time discard only applies at
             * generation); delivery consumes it and restores the sigsuspend
             * mask. */
            let mut actions2 = [0u8; 64 * 24];
            a20_signal_queue_locked(&mut st, 11, core::ptr::null(), 0, 0x4000, 0, 0, 0, 0, &mut r);
            core::ptr::write_unaligned(actions2.as_mut_ptr().add(11 * 24) as *mut u64, SIG_IGN);
            txn = A20SignalDeliveryTxn {
                blocked: bit(5), sigsuspend_active: 1, sigsuspend_old_blocked: bit(7),
                user: 1, traced: 0, ptrace_deliver_sig: 0, sig: 0, outcome: 0,
                handler: 0, flags: 0, sa_mask: 0, old_blocked: 0,
                sig_handling: 0, reset_handler: 0, has_info: 0,
            };
            assert_eq!(a20_signal_delivery_select_locked(&mut st, actions2.as_ptr(), 64, 24,
                0, 8, 8, 16, &mut txn, info.as_mut_ptr(), info.len()), 0);
            assert_eq!(txn.outcome, A20_DELIVERY_DROP_IGNORED);
            assert_eq!(txn.blocked, bit(7), "sigsuspend mask restored");
            assert_eq!(txn.sigsuspend_active, 0);

            /* Default stop: consumed, stop outcome, sigsuspend restored. */
            a20_signal_queue_locked(&mut st, SIGSTOP, core::ptr::null(), 0, SIG_DFL, 0, 0, 0, 0, &mut r);
            txn = A20SignalDeliveryTxn {
                blocked: 0, sigsuspend_active: 1, sigsuspend_old_blocked: bit(3),
                user: 1, traced: 0, ptrace_deliver_sig: 0, sig: 0, outcome: 0,
                handler: 0, flags: 0, sa_mask: 0, old_blocked: 0,
                sig_handling: 0, reset_handler: 0, has_info: 0,
            };
            assert_eq!(a20_signal_delivery_select_locked(&mut st, actions2.as_ptr(), 64, 24,
                0, 8, 8, 16, &mut txn, info.as_mut_ptr(), info.len()), 0);
            assert_eq!(txn.outcome, A20_DELIVERY_STOP);
            assert_eq!(txn.sig, SIGSTOP);
            assert_eq!(txn.sigsuspend_active, 0);

            /* Default fatal. */
            a20_signal_queue_locked(&mut st, 15, core::ptr::null(), 0, SIG_DFL, 0, 0, 0, 0, &mut r);
            assert_eq!(a20_signal_delivery_select_locked(&mut st, actions2.as_ptr(), 64, 24,
                0, 8, 8, 16, &mut txn, info.as_mut_ptr(), info.len()), 0);
            assert_eq!(txn.outcome, A20_DELIVERY_FATAL);
            assert_eq!(txn.sig, 15);

            /* PTRACE interception of a deliverable signal. */
            a20_signal_queue_locked(&mut st, 10, core::ptr::null(), 0, 0x4000, 0, 0, 0, 0, &mut r);
            let mut actions3 = [0u8; 64 * 24];
            core::ptr::write_unaligned(actions3.as_mut_ptr().add(10 * 24) as *mut u64, 0x4000u64);
            txn = A20SignalDeliveryTxn {
                blocked: 0, sigsuspend_active: 0, sigsuspend_old_blocked: 0,
                user: 1, traced: 1, ptrace_deliver_sig: 0, sig: 0, outcome: 0,
                handler: 0, flags: 0, sa_mask: 0, old_blocked: 0,
                sig_handling: 0, reset_handler: 0, has_info: 0,
            };
            assert_eq!(a20_signal_delivery_select_locked(&mut st, actions3.as_ptr(), 64, 24,
                0, 8, 8, 16, &mut txn, info.as_mut_ptr(), info.len()), 0);
            assert_eq!(txn.outcome, A20_DELIVERY_PTRACE_STOP);
            assert_eq!(s.process_pending & bit(10), 0, "ptrace stop consumed the occurrence");
            /* One-shot resume marker lets the same signal deliver. */
            a20_signal_queue_locked(&mut st, 10, core::ptr::null(), 0, 0x4000, 0, 0, 0, 0, &mut r);
            txn.ptrace_deliver_sig = 10;
            assert_eq!(a20_signal_delivery_select_locked(&mut st, actions3.as_ptr(), 64, 24,
                0, 8, 8, 16, &mut txn, info.as_mut_ptr(), info.len()), 0);
            assert_eq!(txn.outcome, A20_DELIVERY_HANDLER);
            assert_eq!(txn.ptrace_deliver_sig, 0);
        }
    }

    #[test]
    fn delivery_select_resethand_and_sigsuspend_mask_composition() {
        let mut s = TxnState::new();
        let mut r = A20SignalQueueResult { discard: 0, fatal: 0, deliverable: 0, sigwait_match: 0 };
        unsafe {
            let mut st = s.st();
            /* Handler with SA_RESETHAND|SA_NODEFER and a sigsuspend in flight:
             * the temporary mask keeps the `|=` composition and old_blocked
             * is the pre-sigsuspend mask. */
            a20_signal_queue_locked(&mut st, 10, core::ptr::null(), 0, 0x5000, 0, 0, 0, 0, &mut r);
            let mut actions = [0u8; 64 * 24];
            let base = actions.as_mut_ptr();
            core::ptr::write_unaligned(base.add(10 * 24) as *mut u64, 0x5000u64);
            core::ptr::write_unaligned(base.add(10 * 24 + 8) as *mut u64, bit(4));
            core::ptr::write_unaligned(base.add(10 * 24 + 16) as *mut u32, 0xc0000000);
            let mut txn = A20SignalDeliveryTxn {
                blocked: bit(9) | bit(2), sigsuspend_active: 1,
                sigsuspend_old_blocked: bit(7) | bit(9),
                user: 1, traced: 0, ptrace_deliver_sig: 0, sig: 0, outcome: 0,
                handler: 0, flags: 0, sa_mask: 0, old_blocked: 0,
                sig_handling: 0, reset_handler: 0, has_info: 0,
            };
            let mut info = [0u8; 128];
            assert_eq!(a20_signal_delivery_select_locked(&mut st, actions.as_ptr(), 64, 24,
                0, 8, 8, 16, &mut txn, info.as_mut_ptr(), info.len()), 0);
            assert_eq!(txn.outcome, A20_DELIVERY_HANDLER);
            assert_eq!(txn.reset_handler, 1, "SA_RESETHAND requests the disposition store");
            assert_eq!(txn.old_blocked, bit(7) | bit(9));
            assert_eq!(txn.blocked, bit(9) | bit(2) | bit(4),
                       "NODEFER: no self bit; |= composition on the temp mask");
            assert_eq!(txn.sigsuspend_active, 0);
        }
    }

    #[test]
    fn exec_reset_clears_dispositions_pending_and_task_slots() {
        let mut s = TxnState::new();
        let mut r = A20SignalQueueResult { discard: 0, fatal: 0, deliverable: 0, sigwait_match: 0 };
        let i = info_with(10, 5);
        unsafe {
            let mut st = s.st();
            a20_signal_queue_locked(&mut st, 10, i.as_ptr(), i.len(), 0x4000, 0, 0, 0, 0, &mut r);
            a20_signal_queue_locked(&mut st, 12, i.as_ptr(), i.len(), 0x4000, 0, 1, 0, 0, &mut r);
            let mut actions = [0u8; 64 * 24];
            let base = actions.as_mut_ptr();
            for sig in [5usize, 6, 7] {
                core::ptr::write_unaligned(base.add(sig * 24) as *mut u64, 0x4000u64 + sig as u64);
                core::ptr::write_unaligned(base.add(sig * 24 + 8) as *mut u64, bit(3));
                core::ptr::write_unaligned(base.add(sig * 24 + 16) as *mut u32, 0x10000000);
            }
            /* SIG_IGN on sig 6 survives; handler + flags + mask reset. */
            core::ptr::write_unaligned(base.add(6 * 24) as *mut u64, SIG_IGN);
            let mut sig_handling = 9;
            let mut sigsuspend_active = 1;
            let mut sigwait_active = 1;
            let mut sigwait_mask = 0xffffu64;
            let mut slots = A20SignalTaskSlots {
                sig_handling: &mut sig_handling,
                sigsuspend_active: &mut sigsuspend_active,
                sigwait_active: &mut sigwait_active,
                sigwait_mask: &mut sigwait_mask,
            };
            assert_eq!(a20_signal_exec_reset_locked(&mut st, actions.as_mut_ptr(), 64, 24,
                0, 8, 8, 16, &mut slots), 0);
            assert_eq!(s.process_pending, 0);
            assert_eq!(s.thread_pending, 0);
            assert_eq!(s.process_has_info[10], 0);
            assert_eq!(s.thread_has_info[12], 0);
            assert_eq!(core::ptr::read_unaligned(base.add(5 * 24) as *const u64), SIG_DFL);
            assert_eq!(core::ptr::read_unaligned(base.add(6 * 24) as *const u64), SIG_IGN);
            assert_eq!(core::ptr::read_unaligned(base.add(6 * 24 + 8) as *const u64), 0);
            assert_eq!(core::ptr::read_unaligned(base.add(6 * 24 + 16) as *const u32), 0);
            assert_eq!(sig_handling, 0);
            assert_eq!(sigsuspend_active, 0);
            assert_eq!(sigwait_active, 0);
            assert_eq!(sigwait_mask, 0);
        }
    }

    #[test]
    fn fd_batch_consumes_lowest_matching_and_formats_records() {
        let mut s = TxnState::new();
        let mut r = A20SignalQueueResult { discard: 0, fatal: 0, deliverable: 0, sigwait_match: 0 };
        let i10 = info_with(10, 1);
        let i20 = info_with(20, 2);
        unsafe {
            let mut st = s.st();
            a20_signal_queue_locked(&mut st, 20, i20.as_ptr(), i20.len(), SIG_DFL, 0, 0, 0, 0, &mut r);
            a20_signal_queue_locked(&mut st, 10, i10.as_ptr(), i10.len(), SIG_DFL, 0, 0, 0, 0, &mut r);
            let mut recs = [0u8; 3 * 128];
            let n = a20_signal_fd_read_locked(&mut st, bit(10) | bit(20) | bit(30),
                                              recs.as_mut_ptr(), 3, 128, 16);
            assert_eq!(n, 2);
            assert_eq!(u32::from_ne_bytes(recs[0..4].try_into().unwrap()), 10);
            assert_eq!(i32::from_ne_bytes(recs[4..8].try_into().unwrap()), 1, "ssi_errno mirrors si_errno");
            assert_eq!(i32::from_ne_bytes(recs[8..12].try_into().unwrap()), 2, "si_code at offset 8");
            assert_eq!(u32::from_ne_bytes(recs[12..16].try_into().unwrap()), 0x55667788, "ssi_pid from _sifields[0] (arch offset 16)");
            assert_eq!(u32::from_ne_bytes(recs[16..20].try_into().unwrap()), 0, "ssi_uid from _sifields[1]");
            assert_eq!(u32::from_ne_bytes(recs[128..132].try_into().unwrap()), 20);
            assert_eq!(s.process_pending, 0);
            /* No match -> 0 records, state untouched. */
            a20_signal_queue_locked(&mut st, 30, core::ptr::null(), 0, SIG_DFL, 0, 0, 0, 0, &mut r);
            let n = a20_signal_fd_read_locked(&mut st, bit(10), recs.as_mut_ptr(), 3, 128, 16);
            assert_eq!(n, 0);
            assert_eq!(s.process_pending, bit(30));
            /* Bad record size is rejected. */
            assert_eq!(a20_signal_fd_read_locked(&mut st, bit(30), recs.as_mut_ptr(), 3, 120, 16), -EINVAL as i64);
        }
    }

    #[test]
    fn take_matching_consumes_blocked_signals_too() {
        let mut s = TxnState::new();
        let mut r = A20SignalQueueResult { discard: 0, fatal: 0, deliverable: 0, sigwait_match: 0 };
        let i = info_with(33, 6);
        unsafe {
            let mut st = s.st();
            a20_signal_queue_locked(&mut st, 33, i.as_ptr(), i.len(), SIG_DFL, bit(33), 0, 0, 0, &mut r);
            let mut out = [0u8; 128];
            let mut has_info = 0;
            let sig = a20_signal_take_matching_locked(&mut st, bit(33), out.as_mut_ptr(), out.len(), &mut has_info);
            assert_eq!(sig, 33, "sigwait consumes even when the signal is blocked");
            assert_eq!(has_info, 1);
            assert_eq!(s.process_pending, 0);
        }
    }

    #[test]
    fn native_consuming_transactions_match_the_reference_rules() {
        unsafe {
            let mut ph = bit(10) | bit(12);
            let mut pt = bit(10);
            let deliver = a20_signal_native_thread_check_locked(&mut ph, &mut pt, bit(12), 0, 0);
            assert_eq!(deliver, bit(10));
            assert_eq!(pt, 0, "thread occurrence consumed first");
            assert_eq!(ph, bit(10) | bit(12), "the second (process) occurrence stays queued");
            let mut ph2 = bit(7);
            let deliver = a20_signal_native_thread_check_locked(&mut ph2, &mut pt, 0, A20_SIGNAL_SELECT, bit(7));
            assert_eq!(deliver, bit(7));
            assert_eq!(ph2, 0);
            let mut p = bit(9) | bit(20);
            let deliver = a20_signal_native_ht_take_locked(&mut p, bit(9));
            assert_eq!(deliver, bit(20));
            assert_eq!(p, bit(9));
        }
    }
}

#[cfg(test)]
#[path = "signal_core_oracle_tests.rs"]
mod oracle_tests;
