#![no_std]

//! Linux sendmmsg/recvmmsg transaction controller.
//!
//! The C adapter owns the pinned socket/vfile, usercopy primitives and one
//! sendmsg/recvmsg operation. This module owns batch-level progress, output
//! copyout error precedence, WAITFORONE state and the per-call receive
//! deadline. Callbacks must not retain the temporary message header.

use core::ffi::c_void;
use core::ptr;

#[cfg(test)]
extern crate std;

const EINVAL: i32 = 22;
const EDOM: i32 = 33;
#[cfg(test)]
const EFAULT: i32 = 14;
const MAX_VLEN: usize = 1024;
const HEADER_SCRATCH_WORDS: usize = 8;
const MSG_DONTWAIT: i32 = 0x0040;
const MSG_WAITFORONE: i32 = 0x10000;
const NSEC_PER_SEC: u64 = 1_000_000_000;
const USEC_PER_SEC: u64 = 1_000_000;

type CopyHeader = unsafe extern "C" fn(
    *mut c_void, *mut c_void, u32, *mut c_void, usize,
) -> i32;
type CopyMsgLen = unsafe extern "C" fn(*mut c_void, *mut c_void, u32, u32) -> i32;
type SendOne = unsafe extern "C" fn(*mut c_void, *mut c_void, i32) -> i64;
type RecvOne = unsafe extern "C" fn(*mut c_void, *mut c_void, i32, u64) -> i64;
type ReadTimeout = unsafe extern "C" fn(*mut c_void, *const c_void, *mut i64, *mut i64) -> i32;
type WriteTimeout = unsafe extern "C" fn(*mut c_void, *mut c_void, i64, i64) -> i32;
type ClockValue = unsafe extern "C" fn(*mut c_void) -> u64;

#[repr(C)]
pub struct LinuxMmsgOps {
    copy_header: Option<CopyHeader>,
    copy_msg_len: Option<CopyMsgLen>,
    send_one: Option<SendOne>,
    recv_one: Option<RecvOne>,
    read_timeout: Option<ReadTimeout>,
    write_timeout: Option<WriteTimeout>,
    now_ticks: Option<ClockValue>,
    ticks_per_sec: Option<ClockValue>,
}

fn clamp_vlen(vlen: u32) -> usize {
    (vlen as usize).min(MAX_VLEN)
}

fn prior_or_error(progress: usize, error: i64) -> i64 {
    if progress == 0 { error } else { progress as i64 }
}

unsafe fn callback_header(
    ctx: *mut c_void,
    user_mmsg: *mut c_void,
    index: usize,
    header_size: usize,
    ops: &LinuxMmsgOps,
    scratch: &mut [usize; HEADER_SCRATCH_WORDS],
) -> Result<*mut c_void, i64> {
    if header_size == 0 || header_size > core::mem::size_of_val(scratch) {
        return Err(-(EINVAL as i64));
    }
    let Some(copy_header) = ops.copy_header else { return Err(-(EINVAL as i64)); };
    let header = scratch.as_mut_ptr().cast::<c_void>();
    ptr::write_bytes(header.cast::<u8>(), 0, header_size);
    let rc = copy_header(ctx, user_mmsg, index as u32, header, header_size);
    if rc < 0 { Err(rc as i64) } else { Ok(header) }
}

/// Run a sendmmsg batch. A successful socket send whose `msg_len` copyout
/// fails has had its side effect, but is not reported as completed; earlier
/// entries still take Linux's partial-progress precedence.
#[no_mangle]
pub unsafe extern "C" fn a20_linux_sendmmsg_transaction(
    ctx: *mut c_void,
    user_mmsg: *mut c_void,
    vlen: u32,
    flags: i32,
    header_size: usize,
    ops: *const LinuxMmsgOps,
) -> i64 {
    let Some(ops) = ops.as_ref() else { return -(EINVAL as i64); };
    let total = clamp_vlen(vlen);
    if total == 0 { return 0; }
    let (Some(send_one), Some(copy_msg_len)) = (ops.send_one, ops.copy_msg_len) else {
        return -(EINVAL as i64);
    };

    let mut completed = 0usize;
    while completed < total {
        let mut header_scratch = [0usize; HEADER_SCRATCH_WORDS];
        let header = match callback_header(
            ctx, user_mmsg, completed, header_size, ops, &mut header_scratch,
        ) {
            Ok(header) => header,
            Err(error) => return prior_or_error(completed, error),
        };
        let sent = send_one(ctx, header, flags);
        if sent < 0 { return prior_or_error(completed, sent); }
        let msg_len = sent.min(u32::MAX as i64) as u32;
        let copied = copy_msg_len(ctx, user_mmsg, completed as u32, msg_len);
        if copied < 0 { return prior_or_error(completed, copied as i64); }
        completed += 1;
    }
    completed as i64
}

unsafe fn read_deadline(
    ctx: *mut c_void,
    user_timeout: *mut c_void,
    ops: &LinuxMmsgOps,
) -> Result<Option<(u64, u64)>, i64> {
    if user_timeout.is_null() { return Ok(None); }
    let (Some(read_timeout), Some(now_ticks), Some(ticks_per_sec)) =
        (ops.read_timeout, ops.now_ticks, ops.ticks_per_sec)
    else { return Err(-(EINVAL as i64)); };

    let mut sec = 0i64;
    let mut nsec = 0i64;
    let rc = read_timeout(ctx, user_timeout, &mut sec, &mut nsec);
    if rc < 0 { return Err(rc as i64); }
    if sec < 0 || nsec < 0 || nsec as u64 >= NSEC_PER_SEC {
        return Err(-(EINVAL as i64));
    }
    let tps = ticks_per_sec(ctx);
    if tps == 0 { return Err(-(EINVAL as i64)); }
    let whole = (sec as u64).checked_mul(tps).ok_or(-(EINVAL as i64))?;
    let fraction = (nsec as u64).saturating_mul(tps) / NSEC_PER_SEC;
    let mut ticks = whole.checked_add(fraction).ok_or(-(EINVAL as i64))?;
    if ticks == 0 && (sec != 0 || nsec != 0) { ticks = 1; }
    let deadline = now_ticks(ctx).saturating_add(ticks);
    Ok(Some((deadline, tps)))
}

unsafe fn write_remaining_timeout(
    ctx: *mut c_void,
    user_timeout: *mut c_void,
    deadline: u64,
    tps: u64,
    ops: &LinuxMmsgOps,
) -> i64 {
    let Some(write_timeout) = ops.write_timeout else { return -(EINVAL as i64); };
    let remaining = deadline.saturating_sub((ops.now_ticks.unwrap())(ctx));
    let sec = remaining / tps;
    let nsec = ((remaining % tps) as u128 * NSEC_PER_SEC as u128 / tps as u128) as u64;
    let rc = write_timeout(ctx, user_timeout, sec.min(i64::MAX as u64) as i64,
                           nsec.min(i64::MAX as u64) as i64);
    if rc < 0 { rc as i64 } else { 0 }
}

/// Run a recvmmsg batch on one pinned socket identity. A failed `msg_len`
/// copyout does not claim the consumed current message as progress.
#[no_mangle]
pub unsafe extern "C" fn a20_linux_recvmmsg_transaction(
    ctx: *mut c_void,
    user_mmsg: *mut c_void,
    vlen: u32,
    flags: i32,
    user_timeout: *mut c_void,
    header_size: usize,
    ops: *const LinuxMmsgOps,
) -> i64 {
    let Some(ops) = ops.as_ref() else { return -(EINVAL as i64); };
    let total = clamp_vlen(vlen);
    if total == 0 { return 0; }
    let (Some(recv_one), Some(copy_msg_len)) = (ops.recv_one, ops.copy_msg_len) else {
        return -(EINVAL as i64);
    };
    let timeout = match read_deadline(ctx, user_timeout, ops) {
        Ok(timeout) => timeout,
        Err(error) => return error,
    };
    let (deadline, tps) = timeout.unwrap_or((0, 0));

    let mut completed = 0usize;
    let mut next_flags = flags;
    let mut result = 0i64;
    while completed < total {
        let mut header_scratch = [0usize; HEADER_SCRATCH_WORDS];
        let header = match callback_header(
            ctx, user_mmsg, completed, header_size, ops, &mut header_scratch,
        ) {
            Ok(header) => header,
            Err(error) => {
                result = prior_or_error(completed, error);
                break;
            }
        };
        let one_flags = if completed > 0 && flags & MSG_WAITFORONE != 0 {
            next_flags | MSG_DONTWAIT
        } else {
            next_flags
        };
        let received = recv_one(ctx, header, one_flags, deadline);
        if received < 0 {
            result = prior_or_error(completed, received);
            break;
        }
        let msg_len = received.min(u32::MAX as i64) as u32;
        let copied = copy_msg_len(ctx, user_mmsg, completed as u32, msg_len);
        if copied < 0 {
            result = prior_or_error(completed, copied as i64);
            break;
        }
        completed += 1;
        result = completed as i64;
        if flags & MSG_WAITFORONE != 0 { next_flags |= MSG_DONTWAIT; }
    }

    if !user_timeout.is_null() {
        let timeout_rc = write_remaining_timeout(ctx, user_timeout, deadline, tps, ops);
        if timeout_rc < 0 { return timeout_rc; }
    }
    result
}

/// Convert a target-ABI timeval (already copied and decoded by C) to kernel
/// ticks. Used by socket option adapters so timeout scalar policy has one Rust
/// implementation shared with the per-call mmsg deadline controller.
#[no_mangle]
pub extern "C" fn a20_linux_timeval_to_ticks(sec: i64, usec: i64, tps: u64,
                                               out_ticks: *mut u64) -> i32 {
    if usec < 0 || usec as u64 >= USEC_PER_SEC { return -EDOM; }
    if tps == 0 || out_ticks.is_null() { return -EINVAL; }
    if sec < 0 {
        unsafe { ptr::write(out_ticks, 0); }
        return 0;
    }
    let whole = match (sec as u64).checked_mul(tps) {
        Some(v) => v,
        None => return -EINVAL,
    };
    let fraction = (usec as u64).saturating_mul(tps) / USEC_PER_SEC;
    let mut ticks = match whole.checked_add(fraction) {
        Some(v) => v,
        None => return -EINVAL,
    };
    if ticks == 0 && (sec != 0 || usec != 0) { ticks = 1; }
    unsafe { ptr::write(out_ticks, ticks); }
    0
}

#[no_mangle]
pub extern "C" fn a20_linux_ticks_to_timeval(ticks: u64, tps: u64,
                                               sec_out: *mut i64,
                                               usec_out: *mut i64) -> i32 {
    if sec_out.is_null() || usec_out.is_null() || tps == 0 { return -EINVAL; }
    let sec = ticks / tps;
    let usec = ((ticks % tps) as u128 * USEC_PER_SEC as u128 / tps as u128) as u64;
    unsafe {
        ptr::write(sec_out, sec.min(i64::MAX as u64) as i64);
        ptr::write(usec_out, usec as i64);
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    struct State {
        send_results: [i64; 4],
        recv_results: [i64; 4],
        send_calls: usize,
        recv_calls: usize,
        len_calls: usize,
        fail_len_at: i32,
        fail_header_at: i32,
        flags: [i32; 4],
        deadlines: [u64; 4],
        now: u64,
        tps: u64,
        timeout_sec: i64,
        timeout_nsec: i64,
        timeout_read_error: i32,
        timeout_write_error: i32,
        written_sec: i64,
        written_nsec: i64,
    }

    impl State {
        fn new() -> Self {
            Self {
                send_results: [1, 1, 1, 1], recv_results: [1, 1, 1, 1],
                send_calls: 0, recv_calls: 0, len_calls: 0,
                fail_len_at: -1, fail_header_at: -1, flags: [0; 4],
                deadlines: [0; 4], now: 20, tps: 100,
                timeout_sec: 1, timeout_nsec: 0,
                timeout_read_error: 0, timeout_write_error: 0,
                written_sec: -1, written_nsec: -1,
            }
        }
    }

    unsafe fn state(ctx: *mut c_void) -> &'static mut State {
        &mut *ctx.cast::<State>()
    }

    unsafe extern "C" fn copy_header(
        ctx: *mut c_void, _user: *mut c_void, index: u32,
        out: *mut c_void, size: usize,
    ) -> i32 {
        if state(ctx).fail_header_at == index as i32 { return -EFAULT; }
        ptr::write_bytes(out.cast::<u8>(), 0, size);
        0
    }

    unsafe extern "C" fn copy_len(
        ctx: *mut c_void, _user: *mut c_void, index: u32, _len: u32,
    ) -> i32 {
        let s = state(ctx);
        s.len_calls += 1;
        if s.fail_len_at == index as i32 { -EFAULT } else { 0 }
    }

    unsafe extern "C" fn send_one(ctx: *mut c_void, _header: *mut c_void,
                                   _flags: i32) -> i64 {
        let s = state(ctx);
        let index = s.send_calls;
        s.send_calls += 1;
        s.send_results[index]
    }

    unsafe extern "C" fn recv_one(ctx: *mut c_void, _header: *mut c_void,
        flags: i32, deadline: u64) -> i64 {
        let s = state(ctx);
        let index = s.recv_calls;
        s.flags[index] = flags;
        s.deadlines[index] = deadline;
        s.recv_calls += 1;
        s.recv_results[index]
    }

    unsafe extern "C" fn read_timeout(ctx: *mut c_void, _user: *const c_void,
        sec: *mut i64, nsec: *mut i64) -> i32 {
        let s = state(ctx);
        if s.timeout_read_error < 0 { return s.timeout_read_error; }
        ptr::write(sec, s.timeout_sec);
        ptr::write(nsec, s.timeout_nsec);
        0
    }

    unsafe extern "C" fn write_timeout(ctx: *mut c_void, _user: *mut c_void,
        sec: i64, nsec: i64) -> i32 {
        let s = state(ctx);
        if s.timeout_write_error < 0 { return s.timeout_write_error; }
        s.written_sec = sec;
        s.written_nsec = nsec;
        0
    }

    unsafe extern "C" fn now(ctx: *mut c_void) -> u64 { state(ctx).now }
    unsafe extern "C" fn tps(ctx: *mut c_void) -> u64 { state(ctx).tps }

    fn ops() -> LinuxMmsgOps {
        LinuxMmsgOps {
            copy_header: Some(copy_header), copy_msg_len: Some(copy_len),
            send_one: Some(send_one), recv_one: Some(recv_one),
            read_timeout: Some(read_timeout), write_timeout: Some(write_timeout),
            now_ticks: Some(now), ticks_per_sec: Some(tps),
        }
    }

    #[test]
    fn send_partial_copyout_failure_excludes_current_entry() {
        let mut s = State::new();
        s.fail_len_at = 1;
        s.send_results = [7, 9, 11, 13];
        let ops = ops();
        let rc = unsafe {
            a20_linux_sendmmsg_transaction(
                (&mut s as *mut State).cast(), ptr::null_mut(), 3, 0, 8, &ops,
            )
        };
        assert_eq!(rc, 1);
        assert_eq!(s.send_calls, 2); // second send side effect occurred
        assert_eq!(s.len_calls, 2);
    }

    #[test]
    fn recv_waitforone_deadline_and_timeout_writeback() {
        let mut s = State::new();
        s.recv_results = [3, -(11 as i64), 0, 0];
        let ops = ops();
        let rc = unsafe {
            a20_linux_recvmmsg_transaction(
                (&mut s as *mut State).cast(), ptr::null_mut(), 4,
                MSG_WAITFORONE, 1usize as *mut c_void, 8, &ops,
            )
        };
        assert_eq!(rc, 1);
        assert_eq!(s.recv_calls, 2);
        assert_eq!(s.flags[0] & MSG_DONTWAIT, 0);
        assert_ne!(s.flags[1] & MSG_DONTWAIT, 0);
        assert_eq!(s.deadlines[0], 120);
        assert_eq!(s.deadlines[1], 120);
        assert_eq!(s.written_sec, 1);
        assert_eq!(s.written_nsec, 0);
    }

    #[test]
    fn recv_without_waitforone_keeps_blocking_after_first_message() {
        let mut s = State::new();
        s.recv_results = [3, -(11 as i64), 0, 0];
        let ops = ops();
        let rc = unsafe {
            a20_linux_recvmmsg_transaction(
                (&mut s as *mut State).cast(), ptr::null_mut(), 4,
                0, 1usize as *mut c_void, 8, &ops,
            )
        };
        assert_eq!(rc, 1);
        assert_eq!(s.recv_calls, 2);
        assert_eq!(s.flags[0] & MSG_DONTWAIT, 0);
        assert_eq!(s.flags[1] & MSG_DONTWAIT, 0);
    }

    #[test]
    fn recv_zero_length_entry_advances_batch() {
        let mut s = State::new();
        s.recv_results = [0, 1, 0, 0];
        let ops = ops();
        let rc = unsafe {
            a20_linux_recvmmsg_transaction(
                (&mut s as *mut State).cast(), ptr::null_mut(), 2,
                MSG_DONTWAIT, ptr::null_mut(), 8, &ops,
            )
        };
        assert_eq!(rc, 2);
        assert_eq!(s.recv_calls, 2);
        assert_eq!(s.len_calls, 2);
    }

    #[test]
    fn timeout_copyout_error_overrides_completed_count() {
        let mut s = State::new();
        s.timeout_write_error = -EFAULT;
        let ops = ops();
        let rc = unsafe {
            a20_linux_recvmmsg_transaction(
                (&mut s as *mut State).cast(), ptr::null_mut(), 1,
                MSG_DONTWAIT, 1usize as *mut c_void, 8, &ops,
            )
        };
        assert_eq!(rc, -(EFAULT as i64));
        assert_eq!(s.recv_calls, 1);
    }
}
