#![no_std]

//! Receive queue state transitions. The C socket layer owns allocation,
//! payload copying, locks, wakeups, and message destruction; this module owns
//! admission and the linked-list/accounting commit performed under the socket
//! lock. The callbacks are deliberately allocation-free and nonblocking.

use core::ffi::c_void;
use core::ptr;

const EAGAIN: i32 = 11;
const EMSGSIZE: i32 = 90;
const ENOTCONN: i32 = 107;
const EINVAL: i32 = 22;
const SOCK_STREAM: i32 = 1;
const SOCK_SEQPACKET: i32 = 5;

#[repr(C)]
struct MsgPrefix {
    next: *mut MsgPrefix,
    len: usize,
    off: usize,
}

unsafe fn sum(head: *mut MsgPrefix, stream: bool) -> u32 {
    let mut total = 0u32;
    let mut cur = head;
    while !cur.is_null() {
        let m = &*cur;
        let bytes = if stream { m.len.saturating_sub(m.off) } else { m.len };
        total = total.wrapping_add(bytes as u32);
        cur = m.next;
    }
    total
}

unsafe fn tally_bytes(tally: *mut u64, count: i32, head: *mut MsgPrefix,
                      stream: bool) -> u32 {
    let packed = ptr::read(tally);
    if (packed >> 32) as u32 == count as u32 {
        packed as u32
    } else {
        let bytes = sum(head, stream);
        ptr::write(tally, ((count as u32 as u64) << 32) | bytes as u64);
        bytes
    }
}

/// Validate queue admission while the destination socket lock is held.
#[no_mangle]
pub unsafe extern "C" fn a20_net_rxq_check(
    socket_type: i32,
    closed: i32,
    rcv_buf: u32,
    default_buf: u32,
    max_count: i32,
    count: i32,
    tally: *mut u64,
    head: *mut c_void,
    len: usize,
) -> i32 {
    if closed != 0 { return -ENOTCONN; }
    if len > u16::MAX as usize { return -EMSGSIZE; }
    if max_count <= 0 || count >= max_count { return -EAGAIN; }
    if tally.is_null() || count < 0 { return -EINVAL; }
    if socket_type != SOCK_STREAM && socket_type != SOCK_SEQPACKET {
        let ceiling = if rcv_buf == 0 { default_buf } else { rcv_buf };
        if ceiling < default_buf {
            let bytes = tally_bytes(tally, count, head.cast(), false);
            if (bytes as usize).saturating_add(len) > ceiling as usize {
                return -EAGAIN;
            }
        }
    }
    0
}

/// Commit a fully initialized message to the queue and update both counters.
#[no_mangle]
pub unsafe extern "C" fn a20_net_rxq_link(
    head: *mut *mut c_void,
    tail: *mut *mut c_void,
    count: *mut i32,
    tally: *mut u64,
    msg: *mut c_void,
    stream: i32,
) -> i32 {
    if head.is_null() || tail.is_null() || count.is_null() || tally.is_null() || msg.is_null() {
        return -EINVAL;
    }
    let old_count = ptr::read(count);
    if old_count < 0 || old_count == i32::MAX { return -EINVAL; }
    let m = msg.cast::<MsgPrefix>();
    let old_head = ptr::read(head).cast::<MsgPrefix>();
    let old_tail = ptr::read(tail).cast::<MsgPrefix>();
    if (*m).next.is_null() == false || (*m).off != 0 || (*m).len > u16::MAX as usize
        || ((old_count == 0) != old_head.is_null())
        || ((old_count == 0) != old_tail.is_null())
        || (!old_tail.is_null() && !(*old_tail).next.is_null()) {
        return -EINVAL;
    }
    let bytes = tally_bytes(tally, old_count, old_head, stream != 0);
    let prev = old_tail;
    if prev.is_null() {
        ptr::write(head, msg);
    } else {
        (*prev).next = m;
    }
    ptr::write(tail, msg);
    ptr::write(count, old_count + 1);
    ptr::write(tally, (((old_count + 1) as u32 as u64) << 32)
        | bytes.wrapping_add((*m).len as u32) as u64);
    0
}

/// Remove bytes from the head. `detached` is non-null only when the complete
/// message was removed; C frees it after copying payload and ancillary fields.
#[no_mangle]
pub unsafe extern "C" fn a20_net_rxq_take(
    head: *mut *mut c_void,
    tail: *mut *mut c_void,
    count: *mut i32,
    tally: *mut u64,
    stream: i32,
    requested: usize,
    out_msg: *mut *mut c_void,
    out_len: *mut usize,
    out_off: *mut usize,
    detached: *mut *mut c_void,
) -> i32 {
    if head.is_null() || tail.is_null() || count.is_null() || tally.is_null()
        || out_msg.is_null() || out_len.is_null() || out_off.is_null()
        || detached.is_null() {
        return -EINVAL;
    }
    let m = ptr::read(head).cast::<MsgPrefix>();
    let old_count = ptr::read(count);
    let old_tail = ptr::read(tail).cast::<MsgPrefix>();
    if m.is_null() || old_count <= 0 || old_tail.is_null()
        || (*m).off > (*m).len || (*m).len > u16::MAX as usize
        || ((old_count == 1) != ((*m).next.is_null()))
        || ((old_count == 1) != (old_tail == m)) {
        return -EINVAL;
    }
    let is_stream = stream != 0;
    let bytes = tally_bytes(tally, old_count, ptr::read(head).cast(), is_stream);
    let avail = (*m).len.saturating_sub((*m).off);
    let n = avail.min(requested);
    ptr::write(out_msg, m.cast());
    ptr::write(out_len, n);
    ptr::write(out_off, (*m).off);
    ptr::write(detached, ptr::null_mut());
    if is_stream && n < avail {
        (*m).off += n;
        ptr::write(tally, ((old_count as u32 as u64) << 32)
            | bytes.saturating_sub(n as u32) as u64);
        return 0;
    }
    let next = (*m).next;
    ptr::write(head, next.cast());
    if next.is_null() { ptr::write(tail, ptr::null_mut()); }
    ptr::write(count, old_count - 1);
    ptr::write(tally, (((old_count - 1) as u32 as u64) << 32)
        | bytes.saturating_sub(avail as u32) as u64);
    (*m).next = ptr::null_mut();
    ptr::write(detached, m.cast());
    0
}

/// Detach the entire queue and reset its tally atomically under the socket lock.
#[no_mangle]
pub unsafe extern "C" fn a20_net_rxq_detach(
    head: *mut *mut c_void,
    tail: *mut *mut c_void,
    count: *mut i32,
    tally: *mut u64,
) -> *mut c_void {
    if head.is_null() || tail.is_null() || count.is_null() || tally.is_null() {
        return ptr::null_mut();
    }
    let old = ptr::read(head);
    ptr::write(head, ptr::null_mut());
    ptr::write(tail, ptr::null_mut());
    ptr::write(count, 0);
    ptr::write(tally, 0);
    old
}

/// Reconcile accounting after direct reset paths that cannot detach here.
#[no_mangle]
pub unsafe extern "C" fn a20_net_rxq_reset(tally: *mut u64) {
    if !tally.is_null() { ptr::write(tally, 0); }
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_rxq_bytes(
    tally: *mut u64,
    count: i32,
    head: *mut c_void,
    stream: i32,
) -> usize {
    if tally.is_null() || count < 0 { return 0; }
    tally_bytes(tally, count, head.cast(), stream != 0) as usize
}

#[cfg(test)]
mod tests {
    use super::*;

    #[repr(C)]
    struct Msg { next: *mut MsgPrefix, len: usize, off: usize }

    #[test]
    fn admission_respects_socket_kind_limits_and_datagram_bytes() {
        let mut tally = 0u64;
        unsafe {
            assert_eq!(a20_net_rxq_check(2, 0, 8, 100, 10, 0,
                         &mut tally, ptr::null_mut(), 9), -EAGAIN);
            assert_eq!(a20_net_rxq_check(2, 0, 0, 100, 10, 0,
                         &mut tally, ptr::null_mut(), 9), 0);
            assert_eq!(a20_net_rxq_check(SOCK_STREAM, 0, 8, 100, 10, 10,
                         &mut tally, ptr::null_mut(), 9), -EAGAIN);
            assert_eq!(a20_net_rxq_check(SOCK_SEQPACKET, 1, 0, 100, 10, 0,
                         &mut tally, ptr::null_mut(), 1), -ENOTCONN);
        }
    }

    #[test]
    fn stream_partial_and_full_dequeue_keep_head_and_packed_tally_consistent() {
        let mut msg = Msg { next: ptr::null_mut(), len: 10, off: 0 };
        let m = (&mut msg as *mut Msg).cast::<c_void>();
        let mut head = ptr::null_mut();
        let mut tail = ptr::null_mut();
        let mut count = 0;
        let mut tally = 0u64;
        unsafe {
            assert_eq!(a20_net_rxq_link(&mut head, &mut tail, &mut count,
                        &mut tally, m, 1), 0);
            assert_eq!(count, 1);
            assert_eq!(tally, (1u64 << 32) | 10);
            let mut out = ptr::null_mut();
            let mut n = 0;
            let mut off = usize::MAX;
            let mut detached = ptr::null_mut();
            assert_eq!(a20_net_rxq_take(&mut head, &mut tail, &mut count,
                        &mut tally, 1, 4, &mut out, &mut n, &mut off,
                        &mut detached), 0);
            assert_eq!((n, off, detached.is_null()), (4, 0, true));
            assert_eq!(msg.off, 4);
            assert_eq!(tally, (1u64 << 32) | 6);
            assert_eq!(a20_net_rxq_take(&mut head, &mut tail, &mut count,
                        &mut tally, 1, 9, &mut out, &mut n, &mut off,
                        &mut detached), 0);
            assert_eq!((n, off, detached), (6, 4, m));
            assert!(head.is_null() && tail.is_null());
            assert_eq!(count, 0);
            assert_eq!(tally, 0);
        }
    }

    #[test]
    fn malformed_take_and_link_leave_queue_unchanged() {
        let mut msg = Msg { next: ptr::null_mut(), len: 4, off: 5 };
        let m = (&mut msg as *mut Msg).cast::<c_void>();
        let mut head = m;
        let mut tail = m;
        let mut count = 1;
        let mut tally = (1u64 << 32) | 4;
        unsafe {
            let mut out = ptr::null_mut();
            let mut n = usize::MAX;
            let mut off = usize::MAX;
            let mut detached = ptr::null_mut();
            assert_eq!(a20_net_rxq_take(&mut head, &mut tail, &mut count,
                        &mut tally, 1, 2, &mut out, &mut n, &mut off,
                        &mut detached), -EINVAL);
            assert_eq!((head, tail, count, tally, n, off),
                       (m, m, 1, (1u64 << 32) | 4, usize::MAX, usize::MAX));

            let mut candidate = Msg { next: ptr::null_mut(), len: 3, off: 0 };
            let candidate_ptr = (&mut candidate as *mut Msg).cast::<c_void>();
            let mut bad_tail = ptr::null_mut();
            let before = tally;
            assert_eq!(a20_net_rxq_link(&mut head, &mut bad_tail, &mut count,
                        &mut tally, candidate_ptr, 1), -EINVAL);
            assert_eq!((head, bad_tail, count, tally),
                       (m, ptr::null_mut(), 1, before));
        }
    }

    #[test]
    fn detach_resets_all_accounting_and_returns_list_once() {
        let mut msg = Msg { next: ptr::null_mut(), len: 7, off: 0 };
        let m = (&mut msg as *mut Msg).cast::<c_void>();
        let mut head = m;
        let mut tail = m;
        let mut count = 1;
        let mut tally = (1u64 << 32) | 7;
        unsafe {
            assert_eq!(a20_net_rxq_detach(&mut head, &mut tail, &mut count,
                         &mut tally), m);
            assert!(head.is_null() && tail.is_null());
            assert_eq!((count, tally), (0, 0));
            assert!(a20_net_rxq_detach(&mut head, &mut tail, &mut count,
                         &mut tally).is_null());
        }
    }
}
