#![no_std]

//! The TCP stream send transaction: the early refusals, the local
//! socket-to-socket path, and the stack-path retry loop with its room
//! computation, halving write retry and allocation-blocked deadline.
//!
//! The C side keeps the primitives this module calls: the socket lock, the
//! address-ordered pair, the reference and free, the blocking enqueue, the lwIP
//! lane lock and option snapshot, the poll, the pcb accessors, `tcp_write` and
//! `tcp_output`, the notconn counter, the trace, the blocking-wait protocol and
//! the two bottom-half drains.
//!
//! Scheduling is not this module's: the wait itself (link the waiter, re-check
//! room, try-wake, park commit) stays one C primitive.

use core::ffi::c_void;
use core::ptr;

const ERR_OK: i32 = 0;
const ERR_MEM: i32 = -1;
const ERR_CONN: i32 = -11;
const ERR_CLSD: i32 = -15;

const EAGAIN: i32 = 11;
const EIO: i32 = 5;
const EPIPE: i32 = 32;
const ENOTSOCK: i32 = 88;

const NOTCONN_NO_PEER: i32 = 0;
const NOTCONN_PEER_GONE: i32 = 1;
const NOTCONN_NOT_CONNECTED: i32 = 3;
const NOTCONN_SHUT_WR: i32 = 4;

/// `net_inet_ip_options_snapshot_t`, opaque here: taken and reapplied by C.
#[repr(C, align(8))]
struct OptsBuf([u8; 32]);

impl OptsBuf {
    fn new() -> OptsBuf {
        OptsBuf([0u8; 32])
    }

    fn as_mut_ptr(&mut self) -> *mut c_void {
        self.0.as_mut_ptr() as *mut c_void
    }
}

#[repr(C)]
pub struct SendTcpView {
    pub s: *mut c_void,
    pub connected: *mut i32,
    pub closed: *mut i32,
    pub ever_connected: *mut i32,
    pub shut_wr: *mut i32,
    pub local_tcp: *mut i32,
    pub nonblock: *mut i32,
    pub send_timeout_ticks: *mut u64,
    pub local: *mut u8,
    pub local_len: *mut usize,
    pub peer: *mut *mut c_void,
    pub tcp: *mut *mut c_void,
}

#[repr(C)]
pub struct SendTcpPair {
    pub lo: *mut c_void,
    pub hi: *mut c_void,
    pub flags: u64,
}

impl SendTcpPair {
    fn zero() -> SendTcpPair {
        SendTcpPair { lo: ptr::null_mut(), hi: ptr::null_mut(), flags: 0 }
    }
}

unsafe extern "C" {
    fn a20_net_inet_send_tcp_view_init(s: *mut c_void, out: *mut SendTcpView);
    fn a20_net_inet_send_tcp_lock(s: *mut c_void, flags: *mut u64);
    fn a20_net_inet_send_tcp_unlock(s: *mut c_void, flags: u64);
    fn a20_net_inet_send_tcp_lock2(a: *mut c_void, b: *mut c_void,
                                   pair: *mut SendTcpPair);
    fn a20_net_inet_send_tcp_unlock2(pair: *mut SendTcpPair);
    fn a20_net_inet_send_tcp_is_live(s: *mut c_void) -> i32;
    fn a20_net_inet_send_tcp_ref(s: *mut c_void);
    fn a20_net_inet_send_tcp_free(s: *mut c_void);
    fn a20_net_inet_send_tcp_notconn(why: i32) -> i32;
    fn a20_net_inet_send_tcp_enqueue_blocking(s: *mut c_void, dst: *mut c_void,
                                              buf: *const c_void, len: usize,
                                              src_local: *const u8, src_len: usize,
                                              nonblock: i32, timeout: u64) -> i32;
    fn a20_net_inet_send_tcp_now() -> u64;
    fn a20_net_inet_send_tcp_ip_lane_lock(s: *mut c_void, opts: *mut c_void) -> u64;
    fn a20_net_inet_send_tcp_lane_unlock(flags: u64);
    fn a20_net_inet_send_tcp_ip_opts_apply(s: *mut c_void, opts: *const c_void);
    fn a20_net_inet_send_tcp_poll_locked();
    fn a20_net_inet_send_tcp_snd_buf_max() -> u32;
    fn a20_net_inet_send_tcp_snd_queuelen_max() -> u32;
    fn a20_net_inet_send_tcp_autotune_grow(s: *mut c_void, tcp: *mut c_void) -> u32;
    fn a20_net_inet_send_tcp_autotune_store(s: *mut c_void, value: u32);
    fn a20_net_inet_send_tcp_pcb_snd_buf(tcp: *mut c_void) -> u32;
    fn a20_net_inet_send_tcp_pcb_snd_queuelen(tcp: *mut c_void) -> u32;
    fn a20_net_inet_send_tcp_write(tcp: *mut c_void, buf: *const u8, len: usize) -> i32;
    fn a20_net_inet_send_tcp_output(tcp: *mut c_void) -> i32;
    fn a20_net_inet_send_tcp_trace_deferred(err: i32);
    fn a20_net_inet_send_tcp_wait(s: *mut c_void, start: u64,
                                  allocation_blocked: i32) -> i32;
    fn a20_net_inet_send_tcp_bottom_half_all();
    fn a20_net_inet_send_tcp_packet_bottom_half();
}

fn zero_view() -> SendTcpView {
    SendTcpView {
        s: ptr::null_mut(),
        connected: ptr::null_mut(),
        closed: ptr::null_mut(),
        ever_connected: ptr::null_mut(),
        shut_wr: ptr::null_mut(),
        local_tcp: ptr::null_mut(),
        nonblock: ptr::null_mut(),
        send_timeout_ticks: ptr::null_mut(),
        local: ptr::null_mut(),
        local_len: ptr::null_mut(),
        peer: ptr::null_mut(),
        tcp: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> SendTcpView {
    let mut v = zero_view();
    a20_net_inet_send_tcp_view_init(s, &mut v);
    v
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_send_tcp(s: *mut c_void, buf: *const c_void,
                                               len: usize) -> i32 {
    let v = view_of(s);
    /* Linux ABI: writes on a once-established endpoint that died report EPIPE. */
    if *v.connected == 0 || *v.closed != 0 {
        return if *v.ever_connected != 0 {
            -EPIPE
        } else {
            a20_net_inet_send_tcp_notconn(NOTCONN_NOT_CONNECTED)
        };
    }
    if *v.shut_wr != 0 {
        return a20_net_inet_send_tcp_notconn(NOTCONN_SHUT_WR);
    }

    if *v.local_tcp != 0 {
        /* Two sockets, ascending by address; the blocking enqueue takes that
         * same pair itself, so this section only resolves the destination and
         * checks it is still live.  s's own options are copied out because the
         * enqueue runs with no lock on s. */
        let mut src_local = [0u8; 128];
        let src_len;
        let mut dst: *mut c_void = ptr::null_mut();
        let nonblock;
        let send_timeout;
        {
            let mut irq = 0u64;
            a20_net_inet_send_tcp_lock(s, &mut irq);
            if a20_net_inet_send_tcp_is_live(s) == 0 {
                a20_net_inet_send_tcp_unlock(s, irq);
                return -ENOTSOCK;
            }
            src_len = *v.local_len;
            ptr::copy_nonoverlapping(v.local, src_local.as_mut_ptr(), src_len);
            /* Sampled under the lock, which is what keeps it from being freed
             * before the reference is taken. */
            let peer = *v.peer;
            if !peer.is_null() {
                a20_net_inet_send_tcp_ref(peer);
                dst = peer;
            }
            nonblock = *v.nonblock;
            send_timeout = *v.send_timeout_ticks;
            a20_net_inet_send_tcp_unlock(s, irq);
        }
        if dst.is_null() {
            return a20_net_inet_send_tcp_notconn(NOTCONN_NO_PEER);
        }
        {
            let mut pair = SendTcpPair::zero();
            a20_net_inet_send_tcp_lock2(s, dst, &mut pair);
            let usable = a20_net_inet_send_tcp_is_live(dst) != 0;
            a20_net_inet_send_tcp_unlock2(&mut pair);
            if !usable {
                a20_net_inet_send_tcp_free(dst);
                return a20_net_inet_send_tcp_notconn(NOTCONN_PEER_GONE);
            }
        }
        let rv = a20_net_inet_send_tcp_enqueue_blocking(s, dst, buf, len,
                                                        src_local.as_ptr(), src_len,
                                                        nonblock, send_timeout);
        a20_net_inet_send_tcp_free(dst);
        return rv;
    }

    let mut sent = 0usize;
    let start = a20_net_inet_send_tcp_now();
    let snd_buf_max = a20_net_inet_send_tcp_snd_buf_max();
    let queuelen_max = a20_net_inet_send_tcp_snd_queuelen_max();
    while sent < len {
        /* One acquisition per iteration: the poll is the progress half of the
         * full poll and runs inside this critical section, while the bottom
         * halves are left to one drain after the loop because they take socket
         * locks, which are never held together with an lwIP lane lock. */
        let mut opts = OptsBuf::new();
        let lwip_flags = a20_net_inet_send_tcp_ip_lane_lock(s, opts.as_mut_ptr());
        a20_net_inet_send_tcp_poll_locked();
        let tcp = *v.tcp;
        let alive = !tcp.is_null() && *v.closed == 0 && *v.connected != 0;
        let mut room: u32 = 0;
        if alive {
            let avail = a20_net_inet_send_tcp_pcb_snd_buf(tcp);
            /* Room is exactly what the pcb says it will accept; the ceiling is
             * the pcb's own buffer, and the wire length is 16 bits. */
            room = if avail > snd_buf_max { snd_buf_max } else { avail };
            if room > 0xffff {
                room = 0xffff;
            }
            if a20_net_inet_send_tcp_pcb_snd_queuelen(tcp) >= queuelen_max {
                room = 0;
            }
        }
        if !alive {
            a20_net_inet_send_tcp_lane_unlock(lwip_flags);
            return if sent > 0 { sent as i32 } else { -EPIPE };
        }
        a20_net_inet_send_tcp_ip_opts_apply(s, opts.as_mut_ptr());
        let mut allocation_blocked = false;
        if room > 0 {
            let mut n = len - sent;
            if n > room as usize {
                n = room as usize;
            }
            /* snd_buf counts bytes, while tcp_write also needs queue entries and
             * pbufs.  A smaller prefix may fit when the full write does not. */
            let e = loop {
                let e = a20_net_inet_send_tcp_write(tcp,
                                                    (buf as *const u8).add(sent),
                                                    n);
                if e != ERR_MEM || n == 1 {
                    break e;
                }
                n /= 2;
            };
            if e == ERR_OK {
                /* tcp_write already owns these bytes.  An output failure leaves
                 * them queued for retransmission; reporting failure would make
                 * the caller resend data the stack has accepted. */
                sent += n;
                let eo = a20_net_inet_send_tcp_output(tcp);
                if eo != ERR_OK {
                    a20_net_inet_send_tcp_trace_deferred(eo);
                }
                a20_net_inet_send_tcp_lane_unlock(lwip_flags);
                continue;
            }
            if e != ERR_MEM {
                a20_net_inet_send_tcp_lane_unlock(lwip_flags);
                return if sent > 0 {
                    sent as i32
                } else if e == ERR_CONN || e == ERR_CLSD {
                    -EPIPE
                } else {
                    -EIO
                };
            }
            allocation_blocked = true;
            room = 0;
        }
        if room == 0 && alive {
            /* The pcb window, not the pbuf pool: the helper refuses when
             * available space is non-zero, so a queue-length cap or an
             * allocation failure does not mint more send buffer. */
            let grew = a20_net_inet_send_tcp_autotune_grow(s, tcp);
            if grew != 0 {
                a20_net_inet_send_tcp_lane_unlock(lwip_flags);
                a20_net_inet_send_tcp_autotune_store(s, grew);
                continue;
            }
        }
        if room == 0 {
            a20_net_inet_send_tcp_lane_unlock(lwip_flags);
            if sent > 0 || *v.nonblock != 0 {
                return if sent > 0 { sent as i32 } else { -EAGAIN };
            }
            let r = a20_net_inet_send_tcp_wait(s, start,
                                               if allocation_blocked { 1 } else { 0 });
            if r < 0 {
                return r;
            }
            continue;
        }
    }
    /* Outside the lane lock, for the same reason the loop does not run them.
     * The error returns above skip this, which is safe because the scheduler
     * runs both bottom halves before picking the next task. */
    a20_net_inet_send_tcp_bottom_half_all();
    a20_net_inet_send_tcp_packet_bottom_half();
    sent as i32
}

#[cfg(test)]
#[allow(static_mut_refs)]
mod tests {
    use super::*;

    #[repr(C)]
    struct Sock {
        connected: i32,
        closed: i32,
        ever_connected: i32,
        shut_wr: i32,
        local_tcp: i32,
        nonblock: i32,
        send_timeout_ticks: u64,
        local: [u8; 128],
        local_len: usize,
        peer: *mut Sock,
        tcp: *mut c_void,
    }

    impl Sock {
        const fn new() -> Sock {
            Sock {
                connected: 1,
                closed: 0,
                ever_connected: 1,
                shut_wr: 0,
                local_tcp: 0,
                nonblock: 0,
                send_timeout_ticks: 0,
                local: [0u8; 128],
                local_len: 0,
                peer: ptr::null_mut(),
                tcp: 8usize as *mut c_void,
            }
        }
    }

    static mut S_SOCK: Sock = Sock::new();
    static mut S_PEER: Sock = Sock::new();
    static mut T_NOTCONN: [i32; 5] = [0; 5];
    static mut T_REF: i32 = 0;
    static mut T_FREE: i32 = 0;
    static mut T_ENQ_BLOCKING: i32 = 0;
    static mut T_ENQ_RC: i32 = 0;
    static mut T_POLL: i32 = 0;
    static mut T_SND_BUF: u32 = 16384;
    static mut T_QUEUELEN: u32 = 0;
    static mut T_PCB_CALLS: i32 = 0;
    static mut T_DIE_AT: i32 = 0;
    static mut T_SND_BUF_SCRIPT: [u32; 4] = [0; 4];
    static mut T_SND_BUF_N: i32 = 0;
    static mut T_SND_BUF_MAX: u32 = 93440;
    static mut T_QUEUELEN_MAX: u32 = 128;
    static mut T_WRITE_CALLS: i32 = 0;
    static mut T_WRITE_LENS: [usize; 16] = [0; 16];
    static mut T_WRITE_RC: i32 = 0;
    /* Per-call results for tests that need partial acceptance across several
     * tcp_write attempts; the single-value T_WRITE_RC stays the default. */
    static mut T_WRITE_RC_SCRIPT: [i32; 16] = [0; 16];
    static mut T_WRITE_RC_N: i32 = 0;
    static mut T_OUTPUT_CALLS: i32 = 0;
    static mut T_OUTPUT_RC: i32 = 0;
    static mut T_TRACE: i32 = 0;
    static mut T_WAIT_CALLS: i32 = 0;
    static mut T_WAIT_RC: i32 = 0;
    static mut T_WAIT_BLOCKED: i32 = -1;
    static mut T_BH_ALL: i32 = 0;
    static mut T_BH_PKT: i32 = 0;
    static mut T_NOW: u64 = 100;
    static mut T_LANE_UNLOCK: i32 = 0;
    static mut T_GROW: u32 = 0;
    static mut T_GROW_CALLS: i32 = 0;
    static mut T_AUTOTUNE_STORE: u32 = 0;

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
        S_PEER = Sock::new();
        T_NOTCONN = [0; 5];
        T_REF = 0;
        T_FREE = 0;
        T_ENQ_BLOCKING = 0;
        T_ENQ_RC = 0;
        T_POLL = 0;
        T_SND_BUF = 16384;
        T_QUEUELEN = 0;
        T_PCB_CALLS = 0;
        T_DIE_AT = 0;
        T_SND_BUF_SCRIPT = [0; 4];
        T_SND_BUF_N = 0;
        T_SND_BUF_MAX = 93440;
        T_QUEUELEN_MAX = 128;
        T_WRITE_CALLS = 0;
        T_WRITE_LENS = [0; 16];
        T_WRITE_RC = 0;
        T_WRITE_RC_SCRIPT = [0; 16];
        T_WRITE_RC_N = 0;
        T_OUTPUT_CALLS = 0;
        T_OUTPUT_RC = 0;
        T_TRACE = 0;
        T_WAIT_CALLS = 0;
        T_WAIT_RC = 0;
        T_WAIT_BLOCKED = -1;
        T_BH_ALL = 0;
        T_BH_PKT = 0;
        T_NOW = 100;
        T_LANE_UNLOCK = 0;
        T_GROW = 0;
        T_GROW_CALLS = 0;
        T_AUTOTUNE_STORE = 0;
    }

    unsafe fn sock() -> *mut c_void {
        ptr::addr_of_mut!(S_SOCK) as *mut c_void
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_view_init(s: *mut c_void,
                                                         out: *mut SendTcpView) {
        let k = &mut *(s as *mut Sock);
        *out = SendTcpView {
            s,
            connected: &mut k.connected,
            closed: &mut k.closed,
            ever_connected: &mut k.ever_connected,
            shut_wr: &mut k.shut_wr,
            local_tcp: &mut k.local_tcp,
            nonblock: &mut k.nonblock,
            send_timeout_ticks: &mut k.send_timeout_ticks,
            local: k.local.as_mut_ptr(),
            local_len: &mut k.local_len,
            peer: &mut k.peer as *mut *mut Sock as *mut *mut c_void,
            tcp: &mut k.tcp,
        };
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_lock(_s: *mut c_void, _f: *mut u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_unlock(_s: *mut c_void, _f: u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_lock2(_a: *mut c_void, _b: *mut c_void,
                                                     pair: *mut SendTcpPair) {
        (*pair).flags = 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_unlock2(pair: *mut SendTcpPair) {
        assert_eq!((*pair).flags, 1);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_is_live(s: *mut c_void) -> i32 {
        if (s as *mut Sock) == ptr::addr_of_mut!(S_SOCK) { 1 } else { 1 }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_ref(_s: *mut c_void) {
        T_REF += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_free(_s: *mut c_void) {
        T_FREE += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_notconn(why: i32) -> i32 {
        T_NOTCONN[why as usize] += 1;
        -100 - why
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_enqueue_blocking(_s: *mut c_void,
                                                                _d: *mut c_void,
                                                                _b: *const c_void,
                                                                _l: usize,
                                                                _sl: *const u8,
                                                                _sln: usize,
                                                                _nb: i32,
                                                                _t: u64) -> i32 {
        T_ENQ_BLOCKING += 1;
        T_ENQ_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_now() -> u64 {
        T_NOW
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_ip_lane_lock(_s: *mut c_void,
                                                            _o: *mut c_void) -> u64 {
        3
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_lane_unlock(flags: u64) {
        assert_eq!(flags, 3);
        T_LANE_UNLOCK += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_ip_opts_apply(_s: *mut c_void,
                                                             _o: *const c_void) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_poll_locked() {
        T_POLL += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_snd_buf_max() -> u32 {
        T_SND_BUF_MAX
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_snd_queuelen_max() -> u32 {
        T_QUEUELEN_MAX
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_pcb_snd_buf(_t: *mut c_void) -> u32 {
        T_PCB_CALLS += 1;
        if T_DIE_AT != 0 && T_PCB_CALLS >= T_DIE_AT {
            (*ptr::addr_of_mut!(S_SOCK)).connected = 0;
        }
        let at = T_PCB_CALLS - 1;
        if at < T_SND_BUF_N {
            T_SND_BUF_SCRIPT[at as usize]
        } else {
            T_SND_BUF
        }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_pcb_snd_queuelen(_t: *mut c_void) -> u32 {
        T_QUEUELEN
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_autotune_grow(_s: *mut c_void,
                                                             _t: *mut c_void) -> u32 {
        T_GROW_CALLS += 1;
        if T_GROW != 0 {
            T_SND_BUF = T_GROW;
        }
        T_GROW
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_autotune_store(_s: *mut c_void,
                                                              value: u32) {
        T_AUTOTUNE_STORE = value;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_write(_t: *mut c_void, _b: *const u8,
                                                     len: usize) -> i32 {
        let at = T_WRITE_CALLS as usize;
        if at < T_WRITE_LENS.len() {
            T_WRITE_LENS[at] = len;
        }
        let rc = if at < T_WRITE_RC_N as usize {
            T_WRITE_RC_SCRIPT[at]
        } else {
            T_WRITE_RC
        };
        T_WRITE_CALLS += 1;
        rc
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_output(_t: *mut c_void) -> i32 {
        T_OUTPUT_CALLS += 1;
        T_OUTPUT_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_trace_deferred(err: i32) {
        T_TRACE = err;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_wait(_s: *mut c_void, _start: u64,
                                                    blocked: i32) -> i32 {
        T_WAIT_CALLS += 1;
        T_WAIT_BLOCKED = blocked;
        T_WAIT_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_bottom_half_all() {
        T_BH_ALL += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_tcp_packet_bottom_half() {
        T_BH_PKT += 1;
    }

    #[test]
    fn the_early_refusals() {
        unsafe {
            let _serial = serial();
            reset();
            (*sock().cast::<Sock>()).connected = 0;
            (*sock().cast::<Sock>()).ever_connected = 1;
            assert_eq!(a20_net_inet_send_tcp(sock(), ptr::null(), 4), -EPIPE);

            /* Never connected: the notconn counter names the reason. */
            reset();
            (*sock().cast::<Sock>()).connected = 0;
            (*sock().cast::<Sock>()).ever_connected = 0;
            assert_eq!(a20_net_inet_send_tcp(sock(), ptr::null(), 4), -103);
            assert_eq!(T_NOTCONN[NOTCONN_NOT_CONNECTED as usize], 1);

            /* A shut-down writer. */
            reset();
            (*sock().cast::<Sock>()).shut_wr = 1;
            assert_eq!(a20_net_inet_send_tcp(sock(), ptr::null(), 4), -104);
            assert_eq!(T_NOTCONN[NOTCONN_SHUT_WR as usize], 1);
            assert_eq!(T_WRITE_CALLS, 0);
        }
    }

    #[test]
    fn the_local_tcp_path_resolves_and_enqueues() {
        unsafe {
            let _serial = serial();
            reset();
            (*sock().cast::<Sock>()).local_tcp = 1;
            (*sock().cast::<Sock>()).local_len = 4;
            (*sock().cast::<Sock>()).peer = ptr::addr_of_mut!(S_PEER);
            assert_eq!(a20_net_inet_send_tcp(sock(), ptr::null(), 5), 0);
            assert_eq!((T_REF, T_ENQ_BLOCKING, T_FREE), (1, 1, 1));
            assert_eq!(T_BH_ALL, 0);

            /* No peer. */
            reset();
            (*sock().cast::<Sock>()).local_tcp = 1;
            assert_eq!(a20_net_inet_send_tcp(sock(), ptr::null(), 5), -100);
            assert_eq!(T_NOTCONN[NOTCONN_NO_PEER as usize], 1);
            assert_eq!(T_ENQ_BLOCKING, 0);
        }
    }

    #[test]
    fn the_stack_path_writes_outputs_and_drains_once() {
        unsafe {
            let _serial = serial();
            reset();
            let buf = [7u8; 32];
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             16), 16);
            assert_eq!(T_WRITE_CALLS, 1);
            assert_eq!(T_WRITE_LENS[0], 16);
            assert_eq!(T_OUTPUT_CALLS, 1);
            assert_eq!((T_BH_ALL, T_BH_PKT), (1, 1));
            assert_eq!(T_POLL, 1);
        }
    }

    #[test]
    fn a_full_window_blocks_through_the_wait_primitive() {
        unsafe {
            let _serial = serial();
            reset();
            /* No room, no progress: the wait is asked and 0 means retry. */
            T_SND_BUF = 0;
            T_WAIT_RC = -11;
            let buf = [7u8; 4];
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             4), -EAGAIN);
            assert_eq!((T_WAIT_CALLS, T_WAIT_BLOCKED), (1, 0));
            assert_eq!((T_BH_ALL, T_BH_PKT), (0, 0));

            /* Nonblocking: EAGAIN without parking. */
            reset();
            T_SND_BUF = 0;
            (*sock().cast::<Sock>()).nonblock = 1;
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             4), -EAGAIN);
            assert_eq!(T_WAIT_CALLS, 0);

            /* A queue at its entry ceiling looks the same as no room. */
            reset();
            T_QUEUELEN = 128;
            T_WAIT_RC = -4;
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             4), -4);
            assert_eq!(T_WAIT_CALLS, 1);
        }
    }

    #[test]
    fn a_memory_blocked_write_reports_the_retry_flag_to_the_wait() {
        unsafe {
            let _serial = serial();
            reset();
            /* Every tcp_write refuses with ERR_MEM: the halving stops at 1 and
             * the wait is told an allocation was blocked. */
            T_WRITE_RC = ERR_MEM;
            T_WAIT_RC = -11;
            let buf = [7u8; 32];
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             16), -EAGAIN);
            /* 16 -> 8 -> 4 -> 2 -> 1: five attempts. */
            assert_eq!(T_WRITE_CALLS, 5);
            assert_eq!(&T_WRITE_LENS[..5], &[16, 8, 4, 2, 1]);
            assert_eq!(T_WAIT_BLOCKED, 1);
        }
    }

    #[test]
    fn a_dead_connection_mid_loop_returns_the_partial_count() {
        unsafe {
            let _serial = serial();
            reset();
            /* The first write takes 8 bytes, then the connection dies: the
             * second iteration sees a dead pcb and returns what was accepted. */
            let buf = [7u8; 32];
            T_SND_BUF_SCRIPT[0] = 8;
            T_SND_BUF_N = 1;
            T_DIE_AT = 1;
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             16), 8);
            assert_eq!(T_WRITE_CALLS, 1);
            assert_eq!(T_WRITE_LENS[0], 8);
            /* Dying before any byte is -EPIPE. */
            reset();
            (*sock().cast::<Sock>()).connected = 0;
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             16), -EPIPE);
        }
    }

    #[test]
    fn no_room_after_a_partial_write_returns_the_count() {
        unsafe {
            let _serial = serial();
            reset();
            /* 8 accepted, then the window closes; a blocking socket returns the
             * partial count rather than parking. */
            let buf = [7u8; 32];
            T_SND_BUF_SCRIPT[0] = 8;
            T_SND_BUF_SCRIPT[1] = 0;
            T_SND_BUF_N = 2;
            (*sock().cast::<Sock>()).nonblock = 1;
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             16), 8);
            assert_eq!(T_WAIT_CALLS, 0);
        }
    }

    #[test]
    fn an_output_failure_keeps_the_accepted_bytes_and_traces() {
        unsafe {
            let _serial = serial();
            reset();
            T_OUTPUT_RC = -4;
            let buf = [7u8; 8];
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             8), 8);
            assert_eq!(T_OUTPUT_CALLS, 1);
            assert_eq!(T_TRACE, -4);
            assert_eq!(T_BH_ALL, 1);
        }
    }

    #[test]
    fn a_write_error_maps_after_the_partial_count() {
        unsafe {
            let _serial = serial();
            reset();
            T_WRITE_RC = ERR_CONN;
            let buf = [7u8; 8];
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             8), -EPIPE);

            reset();
            T_WRITE_RC = -2; /* ERR_BUF */
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             8), -EIO);
            assert_eq!(T_LANE_UNLOCK, 1);
        }
    }

    #[test]
    fn a_missing_pcb_mid_loop_is_epipe() {
        unsafe {
            let _serial = serial();
            reset();
            (*sock().cast::<Sock>()).tcp = ptr::null_mut();
            let buf = [7u8; 4];
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             4), -EPIPE);
            assert_eq!(T_WRITE_CALLS, 0);
            assert_eq!(T_LANE_UNLOCK, 1);
        }
    }
    /// The historical C fixture's positive control: a write larger than the
    /// pcb will take is halved until it fits, the accepted bytes are reported,
    /// and output is called after each accepted prefix.
    #[test]
    fn test_accepted_bytes_and_retry() {
        unsafe {
            let _serial = serial();
            reset();
            /* len > 2 refuses; 8 -> 4 -> 2 accepts on the third attempt. */
            T_WRITE_RC_SCRIPT = [ERR_MEM, ERR_MEM, ERR_OK, ERR_MEM, ERR_MEM,
                                 ERR_OK, ERR_MEM, ERR_OK, ERR_MEM, ERR_OK,
                                 ERR_OK, 0, 0, 0, 0, 0];
            T_WRITE_RC_N = 11;
            let buf = [7u8; 8];
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             8), 8);
            assert_eq!(T_WRITE_CALLS, 11);
            assert_eq!(&T_WRITE_LENS[..11],
                       &[8, 4, 2, 6, 3, 1, 5, 2, 3, 1, 2]);
            assert_eq!(T_OUTPUT_CALLS, 5);
            /* One unlock per iteration that accepted a prefix. */
            assert_eq!(T_LANE_UNLOCK, 5);
        }
    }

    #[test]
    fn window_exhaustion_grows_once_and_retries() {
        unsafe {
            let _serial = serial();
            reset();
            /* No room, then the growth policy grants 8 bytes: the loop retries
             * immediately instead of reporting EAGAIN or parking. */
            T_SND_BUF = 0;
            T_GROW = 8;
            (*sock().cast::<Sock>()).nonblock = 1;
            let buf = [7u8; 4];
            assert_eq!(a20_net_inet_send_tcp(sock(), buf.as_ptr() as *const c_void,
                                             4), 4);
            assert_eq!(T_GROW_CALLS, 1);
            assert_eq!(T_AUTOTUNE_STORE, 8);
            assert_eq!(T_WRITE_CALLS, 1);
            assert_eq!(T_WAIT_CALLS, 0);
        }
    }

}
