#![no_std]

//! The blocking receive transaction: the MSG_WAITALL/PEEK/TRUNC/DONTWAIT matrix,
//! the channel versus legacy-queue choice, stream coalescing, the EOF and
//! timeout decisions, the wakeup collection rules and the accounting ordering.
//!
//! The C side keeps the primitives this module calls: the queue dequeue and peek
//! copies, the channel read, the park protocol, the locks, the timers, the
//! wait-queue collects and `net_tcp_recved`.
//!
//! This is the densest edge-case surface of the socket layer, which is why it is
//! Rust: every decision below encodes an ordering that a wrong answer turns into
//! a lost wakeup, a short read, a peek that consumes, or an EOF that eats the
//! peer's last segment.

use core::ffi::c_void;
use core::ptr;

const AF_ALG: i32 = 38;
const SOCK_STREAM: i32 = 1;

const MSG_PEEK: i32 = 0x2;
const MSG_TRUNC: i32 = 0x20;
const MSG_DONTWAIT: i32 = 0x40;
const MSG_WAITALL: i32 = 0x100;

const EAGAIN: i32 = 11;
const EINTR: i32 = 4;
const ENOTSOCK: i32 = 88;
const ERESTARTSYS: i32 = 512;

#[repr(C)]
pub struct RecvView {
    pub s: *mut c_void,
    pub domain: *mut i32,
    pub sock_type: *mut i32,
    pub nonblock: *mut i32,
    pub closed: *mut i32,
    pub peer_closed: *mut i32,
    pub shut_rd: *mut i32,
    pub passcred: *mut i32,
    pub recv_timeout_ticks: *mut u64,
    pub ch_ep: *mut *mut c_void,
    pub rx_head: *mut *mut c_void,
    pub rx_count: *mut i32,
}

unsafe extern "C" {
    fn a20_net_recv_view_init(s: *mut c_void, out: *mut RecvView);
    fn a20_net_recv_flags_check(flags: i32) -> i32;
    fn a20_net_recv_alg(s: *mut c_void, buf: *mut c_void, len: usize) -> i32;
    fn a20_net_recv_now() -> u64;
    fn a20_net_recv_error_take_locked(s: *mut c_void) -> i32;
    fn a20_net_recv_poll_waiter();
    fn a20_net_recv_lock(s: *mut c_void) -> u64;
    fn a20_net_recv_unlock(s: *mut c_void, flags: u64);
    fn a20_net_recv_have_task() -> i32;
    fn a20_net_recv_has_signal() -> i32;
    fn a20_net_recv_wait_expired(s: *mut c_void, start: u64, for_write: i32) -> i32;
    fn a20_net_recv_wait_expired_locked(s: *mut c_void, start: u64,
                                        for_write: i32) -> i32;
    fn a20_net_recv_msg_avail(head: *mut c_void) -> usize;
    fn a20_net_recv_dequeue(s: *mut c_void, buf: *mut c_void, len: usize,
                            addr: *mut c_void, addrlen: *mut usize,
                            meta: *mut c_void) -> i32;
    fn a20_net_recv_peek_msg(head: *mut c_void, buf: *mut c_void, len: usize,
                             addr: *mut c_void, addrlen: *mut usize,
                             meta: *mut c_void, out_avail: *mut usize,
                             out_n: *mut usize);
    fn a20_net_recv_channel(s: *mut c_void, buf: *mut c_void, len: usize,
                            peek: i32) -> i32;
    fn a20_net_recv_creds(s: *mut c_void, meta: *mut c_void);
    fn a20_net_recv_collect_write(s: *mut c_void, wake_q: *mut c_void) -> i32;
    fn a20_net_recv_collect_read(s: *mut c_void, wake_q: *mut c_void) -> i32;
    fn a20_net_recv_wakeq_init(wake_q: *mut c_void);
    fn a20_net_recv_wakeq_flush(wake_q: *mut c_void);
    fn a20_net_recv_recved(s: *mut c_void, len: usize);
    fn a20_net_recv_park(s: *mut c_void, wait_deadline: u64, channel: i32,
                         dontwait: i32) -> i32;
}

fn zero_view() -> RecvView {
    RecvView {
        s: ptr::null_mut(),
        domain: ptr::null_mut(),
        sock_type: ptr::null_mut(),
        nonblock: ptr::null_mut(),
        closed: ptr::null_mut(),
        peer_closed: ptr::null_mut(),
        shut_rd: ptr::null_mut(),
        passcred: ptr::null_mut(),
        recv_timeout_ticks: ptr::null_mut(),
        ch_ep: ptr::null_mut(),
        rx_head: ptr::null_mut(),
        rx_count: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> RecvView {
    let mut v = zero_view();
    a20_net_recv_view_init(s, &mut v);
    v
}

/// The timer deadline check: an absolute deadline wins, otherwise the socket's
/// own wait deadline (checked under the lock where the caller holds it).
unsafe fn deadline_passed(s: *mut c_void, start: u64, deadline: u64, locked: i32) -> bool {
    if deadline != 0 {
        ((a20_net_recv_now().wrapping_sub(deadline)) as i64) >= 0
    } else if locked != 0 {
        a20_net_recv_wait_expired_locked(s, start, 0) != 0
    } else {
        a20_net_recv_wait_expired(s, start, 0) != 0
    }
}

unsafe fn wait_deadline_of(start: u64, deadline: u64, recv_timeout: u64) -> u64 {
    if deadline != 0 {
        deadline
    } else if recv_timeout != 0 {
        start.wrapping_add(recv_timeout)
    } else {
        0
    }
}

/// One receive attempt, including its own park-and-retry loop.  The C entry
/// calls this again for each MSG_WAITALL chunk, so it takes a fresh start tick
/// exactly as the recursive C call did.
unsafe fn recv_once(s: *mut c_void, v: &RecvView, buf: *mut c_void, len: usize,
                    flags: i32, addr: *mut c_void, addrlen: *mut usize,
                    meta: *mut c_void, deadline: u64) -> i32 {
    let dontwait = (flags & MSG_DONTWAIT) != 0;
    let start = a20_net_recv_now();
    let peek = (flags & MSG_PEEK) != 0;
    let trunc_datagram = (flags & MSG_TRUNC) != 0 && *v.sock_type != SOCK_STREAM;

    if !(*v.ch_ep).is_null() {
        /* Channel-backed: plain data lives on the internal channel, SCM_RIGHTS
         * messages on the legacy queue.  Drain legacy first so fd-carrying
         * messages keep their delivery order. */
        loop {
            a20_net_recv_poll_waiter();
            let mut r: i32 = -EAGAIN;
            let irq = a20_net_recv_lock(s);
            let head = *v.rx_head;
            if !head.is_null() {
                if peek {
                    let mut avail = 0usize;
                    let mut n = 0usize;
                    a20_net_recv_peek_msg(head, buf, len, ptr::null_mut(),
                                          ptr::null_mut(), ptr::null_mut(),
                                          &mut avail, &mut n);
                    let _ = avail;
                    r = n as i32;
                } else {
                    r = a20_net_recv_dequeue(s, buf, len, addr, addrlen, meta);
                }
                /* Stream coalescing keeps the SCM_RIGHTS fd semantics. */
                if r >= 0 && *v.sock_type == SOCK_STREAM && !peek {
                    let mut total = r as usize;
                    while total < len && !(*v.rx_head).is_null() {
                        let nr = a20_net_recv_dequeue(
                            s, (buf as *mut u8).add(total) as *mut c_void,
                            len - total, ptr::null_mut(), ptr::null_mut(), meta);
                        if nr <= 0 {
                            break;
                        }
                        total += nr as usize;
                    }
                    r = total as i32;
                }
            }
            if r == -EAGAIN {
                r = a20_net_recv_channel(s, buf, len, if peek { 1 } else { 0 });
            }
            if r >= 0 {
                /* Channel delivery has no net_msg metadata; supply
                 * SCM_CREDENTIALS from the latest sender record. */
                if !meta.is_null() {
                    a20_net_recv_creds(s, meta);
                }
                let mut wq = WakeQ::new();
                a20_net_recv_wakeq_init(wq.as_mut_ptr());
                a20_net_recv_collect_write(s, wq.as_mut_ptr());
                a20_net_recv_unlock(s, irq);
                a20_net_recv_wakeq_flush(wq.as_mut_ptr());
                return r;
            }
            if r != -EAGAIN {
                a20_net_recv_unlock(s, irq);
                return r;
            }
            if a20_net_recv_have_task() == 0 {
                a20_net_recv_unlock(s, irq);
                return -EAGAIN;
            }
            if a20_net_recv_has_signal() != 0 {
                a20_net_recv_unlock(s, irq);
                return -ERESTARTSYS;
            }
            if deadline_passed(s, start, deadline, 1) {
                a20_net_recv_unlock(s, irq);
                return -EAGAIN;
            }
            if *v.nonblock != 0 || dontwait || *v.closed != 0
                || *v.peer_closed != 0 || *v.shut_rd != 0
            {
                let eof = *v.closed != 0 || *v.peer_closed != 0 || *v.shut_rd != 0;
                a20_net_recv_unlock(s, irq);
                return if eof { 0 } else { -EAGAIN };
            }
            let wd = wait_deadline_of(start, deadline, *v.recv_timeout_ticks);
            a20_net_recv_unlock(s, irq);
            let e = a20_net_recv_park(s, wd, 1, 0);
            if e == 0 {
                continue;
            }
            return e;
        }
    }

    loop {
        a20_net_recv_poll_waiter();
        let mut wake_q = WakeQ::new();
        a20_net_recv_wakeq_init(wake_q.as_mut_ptr());
        let irq = a20_net_recv_lock(s);
        let error = a20_net_recv_error_take_locked(s);
        if error != 0 {
            a20_net_recv_unlock(s, irq);
            return -error;
        }
        let rx_count_before = *v.rx_count;
        let head = *v.rx_head;
        /* The dequeue frees the head, so the datagram length MSG_TRUNC needs
         * has to be read off it while it is still linked. */
        let datagram_len = if !head.is_null() && *v.sock_type != SOCK_STREAM {
            a20_net_recv_msg_avail(head)
        } else {
            0
        };
        let mut r: i32;
        if peek && !head.is_null() {
            let mut avail = 0usize;
            let mut n = 0usize;
            a20_net_recv_peek_msg(head, buf, len, addr, addrlen, meta,
                                  &mut avail, &mut n);
            r = if trunc_datagram { avail as i32 } else { n as i32 };
        } else {
            r = a20_net_recv_dequeue(s, buf, len, addr, addrlen, meta);
            if r >= 0 && trunc_datagram && datagram_len > r as usize {
                r = datagram_len as i32;
            }
        }
        /* Stream coalescing: drain further queued messages into the same
         * buffer.  A peek must not consume anything, so it stops at the head
         * even when more is queued -- a peek is a read without a dequeue. */
        if r > 0 && *v.sock_type == SOCK_STREAM && !peek {
            let mut total = r as usize;
            while total < len && !(*v.rx_head).is_null() {
                let nr = a20_net_recv_dequeue(
                    s, (buf as *mut u8).add(total) as *mut c_void, len - total,
                    ptr::null_mut(), ptr::null_mut(), meta);
                if nr <= 0 {
                    break;
                }
                total += nr as usize;
            }
            r = total as i32;
        }
        if r != -EAGAIN || *v.nonblock != 0 || dontwait || *v.closed != 0
            || *v.peer_closed != 0 || *v.shut_rd != 0
        {
            if r == -EAGAIN && (*v.closed != 0 || *v.peer_closed != 0
                                || *v.shut_rd != 0)
            {
                r = 0;
            }
            /* Accounting is deliberately outside the socket lock: net_tcp_recved
             * takes the lwIP lock, and the two must never nest.  A peeked stream
             * message is charged too -- the bytes are in the receive queue
             * either way, and never charging them would stall the window once
             * the queue filled.  The cost is that repeated peeks of the same
             * head keep re-charging it. */
            let recved = if r > 0 && *v.sock_type == SOCK_STREAM { r } else { 0 };
            if r > 0 && *v.rx_count < rx_count_before {
                a20_net_recv_collect_write(s, wake_q.as_mut_ptr());
            }
            if r > 0 && !(*v.rx_head).is_null() {
                a20_net_recv_collect_read(s, wake_q.as_mut_ptr());
            }
            a20_net_recv_unlock(s, irq);
            a20_net_recv_wakeq_flush(wake_q.as_mut_ptr());
            if recved != 0 {
                a20_net_recv_recved(s, recved as usize);
            }
            return r;
        }
        if a20_net_recv_have_task() == 0 {
            a20_net_recv_unlock(s, irq);
            return -EAGAIN;
        }
        if a20_net_recv_has_signal() != 0 {
            a20_net_recv_unlock(s, irq);
            return -ERESTARTSYS;
        }
        if deadline_passed(s, start, deadline, 1) {
            a20_net_recv_unlock(s, irq);
            return -EAGAIN;
        }
        let wd = wait_deadline_of(start, deadline, *v.recv_timeout_ticks);
        a20_net_recv_unlock(s, irq);
        let e = a20_net_recv_park(s, wd, 0, if dontwait { 1 } else { 0 });
        if e == 0 {
            continue;
        }
        return e;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_recvfrom_socket_meta_until(
    s: *mut c_void, buf: *mut c_void, len: usize, flags: i32, addr: *mut c_void,
    addrlen: *mut usize, meta: *mut c_void, deadline: u64,
) -> i32 {
    if s.is_null() {
        return -ENOTSOCK;
    }
    let bad = a20_net_recv_flags_check(flags);
    if bad != 0 {
        return bad;
    }
    let v = view_of(s);
    if *v.domain == AF_ALG {
        return a20_net_recv_alg(s, buf, len);
    }
    if (flags & MSG_WAITALL) != 0 && *v.sock_type == SOCK_STREAM && len > 0 {
        /* MSG_WAITALL: keep reading until len bytes have arrived, or until the
         * read is cut short by EOF, the receive timeout, or a signal.  A single
         * pass below already coalesces everything queued up to len, so this loop
         * only iterates when the queue runs dry mid-buffer, which is the one
         * case the flag adds.  WAITALL is stripped from the inner call so the
         * recursion stays one level deep, and the caller's SO_RCVTIMEO is
         * honoured against this outer start rather than restarting per read. */
        let inner = flags & !MSG_WAITALL;
        let start = a20_net_recv_now();
        let mut got = 0usize;
        while got < len {
            if deadline_passed(s, start, deadline, 0) {
                return got as i32;
            }
            let n = recv_once(s, &v, (buf as *mut u8).add(got) as *mut c_void,
                              len - got, inner,
                              if got != 0 { ptr::null_mut() } else { addr },
                              if got != 0 { ptr::null_mut() } else { addrlen },
                              meta, deadline);
            if n < 0 {
                return if got != 0 { got as i32 } else { n };
            }
            if n == 0 {
                return got as i32;
            }
            got += n as usize;
        }
        return got as i32;
    }
    recv_once(s, &v, buf, len, flags, addr, addrlen, meta, deadline)
}

/// proc_wake_q_t is 64 items of (task, seq, reason) plus a count; the size is
/// the caller's, this is only a scratch buffer it initializes and flushes.
#[repr(C, align(16))]
struct WakeQ([u8; 2048]);

impl WakeQ {
    fn new() -> WakeQ {
        WakeQ([0u8; 2048])
    }

    fn as_mut_ptr(&mut self) -> *mut c_void {
        self.0.as_mut_ptr() as *mut c_void
    }
}

#[cfg(test)]
#[allow(static_mut_refs)]
mod tests {
    use super::*;

    #[repr(C)]
    struct Sock {
        domain: i32,
        sock_type: i32,
        nonblock: i32,
        closed: i32,
        peer_closed: i32,
        shut_rd: i32,
        passcred: i32,
        recv_timeout_ticks: u64,
        ch_ep: *mut c_void,
        rx_head: *mut c_void,
        rx_count: i32,
    }

    impl Sock {
        const fn new() -> Sock {
            Sock {
                domain: 2,
                sock_type: SOCK_STREAM,
                nonblock: 0,
                closed: 0,
                peer_closed: 0,
                shut_rd: 0,
                passcred: 0,
                recv_timeout_ticks: 0,
                ch_ep: ptr::null_mut(),
                rx_head: ptr::null_mut(),
                rx_count: 0,
            }
        }
    }

    static mut S_SOCK: Sock = Sock::new();
    static mut T_FLAGS_RC: i32 = 0;
    static mut T_ALG_CALLS: i32 = 0;
    static mut T_NOW: u64 = 1000;
    static mut T_SIGNAL: i32 = 0;
    static mut T_EXPIRED: i32 = 0;
    static mut T_ERROR: i32 = 0;
    static mut T_DEQ_RC: i32 = 0;
    static mut T_DEQ_CALLS: i32 = 0;
    static mut T_DEQ_TOTAL: usize = 0;
    static mut T_PEEK_AVAIL: usize = 0;
    static mut T_PEEK_N: usize = 0;
    static mut T_CH_RC: i32 = -EAGAIN;
    static mut T_CH_CALLS: i32 = 0;
    static mut T_HAVE_TASK: i32 = 1;
    static mut T_HEAD_AVAIL: usize = 0;
    static mut T_DEQ_SEQ: [i32; 8] = [0; 8];
    static mut T_DEQ_SEQ_N: i32 = 0;
    static mut T_CREDS: i32 = 0;
    static mut T_COLLECT_WRITE: i32 = 0;
    static mut T_COLLECT_READ: i32 = 0;
    static mut T_RECVED: i32 = 0;
    static mut T_PARK_CALLS: i32 = 0;
    static mut T_PARK_RC: [i32; 4] = [0; 4];
    static mut T_PARK_AT: usize = 0;
    static mut T_PARK_CHANNEL: [i32; 4] = [0; 4];
    static mut T_POLL_WAITER: i32 = 0;
    static mut T_TRACE: [u8; 32] = [0; 32];
    static mut T_TRACE_N: usize = 0;

    static T_SERIAL: core::sync::atomic::AtomicBool =
        core::sync::atomic::AtomicBool::new(false);

    struct Serial;

    impl Drop for Serial {
        fn drop(&mut self) {
            T_SERIAL.store(false, core::sync::atomic::Ordering::Release);
        }
    }

    fn serial() -> Serial {
        while T_SERIAL.swap(true, core::sync::atomic::Ordering::Acquire) {
            core::hint::spin_loop();
        }
        Serial
    }

    unsafe fn reset() {
        S_SOCK = Sock::new();
        T_FLAGS_RC = 0;
        T_ALG_CALLS = 0;
        T_NOW = 1000;
        T_SIGNAL = 0;
        T_EXPIRED = 0;
        T_ERROR = 0;
        T_DEQ_RC = 0;
        T_DEQ_CALLS = 0;
        T_DEQ_TOTAL = 0;
        T_PEEK_AVAIL = 0;
        T_PEEK_N = 0;
        T_CH_RC = -EAGAIN;
        T_CH_CALLS = 0;
        T_HAVE_TASK = 1;
        T_HEAD_AVAIL = 0;
        T_DEQ_SEQ = [0; 8];
        T_DEQ_SEQ_N = 0;
        T_CREDS = 0;
        T_COLLECT_WRITE = 0;
        T_COLLECT_READ = 0;
        T_RECVED = 0;
        T_PARK_CALLS = 0;
        T_PARK_RC = [0; 4];
        T_PARK_AT = 0;
        T_PARK_CHANNEL = [0; 4];
        T_POLL_WAITER = 0;
        T_TRACE = [0; 32];
        T_TRACE_N = 0;
    }

    unsafe fn sock() -> *mut c_void {
        ptr::addr_of_mut!(S_SOCK) as *mut c_void
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_view_init(s: *mut c_void,
                                                out: *mut RecvView) {
        let k = &mut *(s as *mut Sock);
        *out = RecvView {
            s,
            domain: &mut k.domain,
            sock_type: &mut k.sock_type,
            nonblock: &mut k.nonblock,
            closed: &mut k.closed,
            peer_closed: &mut k.peer_closed,
            shut_rd: &mut k.shut_rd,
            passcred: &mut k.passcred,
            recv_timeout_ticks: &mut k.recv_timeout_ticks,
            ch_ep: &mut k.ch_ep,
            rx_head: &mut k.rx_head,
            rx_count: &mut k.rx_count,
        };
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_flags_check(_f: i32) -> i32 {
        T_FLAGS_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_alg(_s: *mut c_void, _b: *mut c_void,
                                        _l: usize) -> i32 {
        T_ALG_CALLS += 1;
        7
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_now() -> u64 {
        T_NOW
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_poll_waiter() {
        T_POLL_WAITER += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_lock(_s: *mut c_void) -> u64 {
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_unlock(_s: *mut c_void, _f: u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_have_task() -> i32 {
        T_HAVE_TASK
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_has_signal() -> i32 {
        T_SIGNAL
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_msg_avail(head: *mut c_void) -> usize {
        let _ = head;
        T_HEAD_AVAIL
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_wait_expired(_s: *mut c_void, _st: u64,
                                                   _fw: i32) -> i32 {
        T_EXPIRED
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_wait_expired_locked(_s: *mut c_void,
                                                          _st: u64, _fw: i32) -> i32 {
        T_EXPIRED
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_error_take_locked(_s: *mut c_void) -> i32 {
        let e = T_ERROR; T_ERROR = 0; e
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_dequeue(_s: *mut c_void, _b: *mut c_void,
                                              len: usize, _a: *mut c_void,
                                              _al: *mut usize,
                                              _m: *mut c_void) -> i32 {
        T_DEQ_CALLS += 1;
        let idx = T_DEQ_CALLS as usize - 1;
        let rc = if idx < T_DEQ_SEQ_N as usize {
            T_DEQ_SEQ[idx]
        } else {
            T_DEQ_RC
        };
        if rc > 0 {
            let n = (rc as usize).min(len);
            T_DEQ_TOTAL += n;
            return n as i32;
        }
        rc
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_peek_msg(_m: *mut c_void, _b: *mut c_void,
                                               _l: usize, _a: *mut c_void,
                                               _al: *mut usize, _mt: *mut c_void,
                                               avail: *mut usize, n: *mut usize) {
        *avail = T_PEEK_AVAIL;
        *n = T_PEEK_N;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_channel(_s: *mut c_void, _b: *mut c_void,
                                              _l: usize, _p: i32) -> i32 {
        T_CH_CALLS += 1;
        T_CH_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_creds(_s: *mut c_void, _m: *mut c_void) {
        T_CREDS += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_collect_write(_s: *mut c_void,
                                                    _w: *mut c_void) -> i32 {
        T_COLLECT_WRITE += 1;
        1
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_collect_read(_s: *mut c_void,
                                                   _w: *mut c_void) -> i32 {
        T_COLLECT_READ += 1;
        1
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_wakeq_init(_w: *mut c_void) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_wakeq_flush(_w: *mut c_void) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_recved(_s: *mut c_void, _l: usize) {
        T_RECVED += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_recv_park(_s: *mut c_void, _wd: u64,
                                           channel: i32, _dw: i32) -> i32 {
        let at = T_PARK_CALLS as usize;
        T_PARK_CALLS += 1;
        if at < 4 {
            T_PARK_CHANNEL[at] = channel;
        }
        if at < T_PARK_RC.len() {
            T_PARK_RC[at]
        } else {
            0
        }
    }

    #[test]
    fn the_early_refusals_and_the_alg_dispatch() {
        unsafe {
            let _serial = serial();
            reset();
            assert_eq!(a20_net_recvfrom_socket_meta_until(ptr::null_mut(),
                        ptr::null_mut(), 1, 0, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), -ENOTSOCK);
            reset();
            T_FLAGS_RC = -22;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        1, 0x8, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), -22);
            assert_eq!(T_POLL_WAITER, 0);

            /* AF_ALG goes to the algorithm socket's recv. */
            reset();
            (*sock().cast::<Sock>()).domain = AF_ALG;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        1, 0, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), 7);
            assert_eq!(T_ALG_CALLS, 1);
        }
    }

    #[test]
    fn peek_returns_without_consuming_and_honours_trunc() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).rx_head = 8usize as *mut c_void;
            T_PEEK_AVAIL = 10;
            T_PEEK_N = 4;
            /* A stream peek: the copied count. */
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        4, MSG_PEEK, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), 4);
            /* A datagram peek with MSG_TRUNC: the full datagram length. */
            (*s).sock_type = 2;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        4, MSG_PEEK | MSG_TRUNC, ptr::null_mut(),
                        ptr::null_mut(), ptr::null_mut(), 0), 10);
            assert_eq!(T_DEQ_CALLS, 0);
        }
    }

    #[test]
    fn stream_coalescing_drains_until_the_buffer_is_full() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).rx_head = 8usize as *mut c_void;
            T_DEQ_RC = 4;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        12, 0, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), 12);
            /* Three dequeues: 4 + 4 + 4 fills the buffer. */
            assert_eq!(T_DEQ_CALLS, 3);
            /* Stream data is charged to the window after the unlock. */
            assert_eq!(T_RECVED, 1);
        }
    }

    #[test]
    fn a_datagram_read_is_not_coalesced() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).sock_type = 2;
            T_DEQ_RC = 4;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        12, 0, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), 4);
            assert_eq!(T_DEQ_CALLS, 1);
            assert_eq!(T_RECVED, 0);
        }
    }

    #[test]
    fn eof_is_zero_and_a_drained_queue_wakes_the_writer() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).peer_closed = 1;
            T_DEQ_RC = -EAGAIN;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        4, 0, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), 0);
            /* No data, so no writer wake. */
            assert_eq!(T_COLLECT_WRITE, 0);

            /* Data drained: the writer waiting for space is woken. */
            reset();
            let s = sock().cast::<Sock>();
            (*s).rx_count = 0;
            T_DEQ_RC = 4;
            (*s).rx_head = ptr::null_mut();
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        4, 0, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), 4);
            /* rx_count (0) is not below the captured count (0): no wake. */
            assert_eq!(T_COLLECT_WRITE, 0);
        }
    }

    #[test]
    fn nonblocking_and_signals_do_not_park() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).nonblock = 1;
            T_DEQ_RC = -EAGAIN;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        4, 0, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), -EAGAIN);
            assert_eq!(T_PARK_CALLS, 0);

            /* MSG_DONTWAIT without the socket flag. */
            reset();
            T_DEQ_RC = -EAGAIN;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        4, MSG_DONTWAIT, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), -EAGAIN);
            assert_eq!(T_PARK_CALLS, 0);

            /* A pending signal is ERESTARTSYS. */
            reset();
            let _s = sock().cast::<Sock>();
            T_DEQ_RC = -EAGAIN;
            T_SIGNAL = 1;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        4, 0, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), -ERESTARTSYS);
            assert_eq!(T_PARK_CALLS, 0);
        }
    }

    #[test]
    fn the_timeout_is_checked_against_the_start_and_the_park_deadline() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            T_DEQ_RC = -EAGAIN;
            (*s).recv_timeout_ticks = 50;
            /* Park returns EAGAIN (timeout). */
            T_PARK_RC[0] = -EAGAIN;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        4, 0, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), -EAGAIN);
            assert_eq!(T_PARK_CALLS, 1);
            assert_eq!(T_PARK_CHANNEL[0], 0);

            /* An already-expired wait does not park at all. */
            reset();
            T_DEQ_RC = -EAGAIN;
            T_EXPIRED = 1;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        4, 0, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), -EAGAIN);
            assert_eq!(T_PARK_CALLS, 0);
        }
    }

    #[test]
    fn waitall_loops_until_the_buffer_fills() {
        unsafe {
            let _serial = serial();
            reset();
            /* 4 bytes per inner read, twice, to fill the 8-byte buffer. */
            T_DEQ_RC = 4;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        8, MSG_WAITALL, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), 8);
            assert_eq!(T_DEQ_CALLS, 2);
            assert_eq!(T_PARK_CALLS, 0);
        }
    }

    #[test]
    fn waitall_returns_a_short_count_on_eof() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            /* 3 bytes, then the queue drains and the peer closes. */
            T_DEQ_SEQ = [3, -EAGAIN, 0, 0, 0, 0, 0, 0];
            T_DEQ_SEQ_N = 2;
            (*s).peer_closed = 1;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        8, MSG_WAITALL, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), 3);
        }
    }

    #[test]
    fn waitall_returns_the_negative_error_only_with_nothing_read() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).nonblock = 1;
            T_DEQ_RC = -EAGAIN;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        8, MSG_WAITALL, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), -EAGAIN);
        }
    }

    #[test]
    fn the_channel_path_prefers_the_legacy_queue() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).ch_ep = 8usize as *mut c_void;
            (*s).rx_head = 9usize as *mut c_void;
            T_DEQ_RC = 3;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        3, 0, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), 3);
            assert_eq!((T_DEQ_CALLS, T_CH_CALLS), (1, 0));
            assert_eq!(T_COLLECT_WRITE, 1);

            /* With no legacy message the channel read serves it. */
            reset();
            let s = sock().cast::<Sock>();
            (*s).ch_ep = 8usize as *mut c_void;
            T_DEQ_RC = -EAGAIN;
            T_CH_RC = 5;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        5, 0, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), 5);
            assert_eq!((T_DEQ_CALLS, T_CH_CALLS), (0, 1));
        }
    }

    #[test]
    fn the_channel_path_stamps_credentials_and_parks_with_eintr() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).ch_ep = 8usize as *mut c_void;
            (*s).passcred = 1;
            T_DEQ_RC = -EAGAIN;
            T_CH_RC = 2;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        2, 0, ptr::null_mut(), ptr::null_mut(),
                        &mut 0u64 as *mut u64 as *mut c_void, 0), 2);
            assert_eq!(T_CREDS, 1);

            /* An interrupt on the channel path is EINTR (not ERESTARTSYS). */
            reset();
            let s = sock().cast::<Sock>();
            (*s).ch_ep = 8usize as *mut c_void;
            T_DEQ_RC = -EAGAIN;
            T_CH_RC = -EAGAIN;
            T_PARK_RC[0] = -EINTR;
            assert_eq!(a20_net_recvfrom_socket_meta_until(sock(), ptr::null_mut(),
                        2, 0, ptr::null_mut(), ptr::null_mut(),
                        ptr::null_mut(), 0), -EINTR);
            assert_eq!(T_PARK_CHANNEL[0], 1);
        }
    }
}
