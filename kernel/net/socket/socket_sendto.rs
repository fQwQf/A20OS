#![no_std]

//! The sendto transaction: the family/type dispatch ladder, the
//! EPIPE-versus-ENOTSOCK rule for a stream whose pcb lwIP tore down, the
//! destination resolution with its four error answers, the queue-full handoff
//! to the blocking enqueue, and the chunk loop for a payload larger than one
//! message.
//!
//! C keeps the primitives this module calls: the flags check, the per-family
//! send entries, the netns test, the socket lock pair, the liveness test, the
//! reference and the free, the loopback autobind, the bound-socket scan, the
//! two enqueues, the read-wake collect and the wake queue.
//!
//! The order below is the whole point of the function.  A stream socket with no
//! pcb, in a family this stack owns, is a connection lwIP tore down rather than
//! a socket that never had one, so it owes EPIPE -- and that test has to sit
//! after the inet dispatch and before the generic two-socket path, or the write
//! lands in a path that reports ENOTSOCK for an fd that is very much a socket.

use core::ffi::c_void;
use core::ptr;

const AF_UNIX: i32 = 1;
const AF_INET: i32 = 2;
const AF_INET6: i32 = 10;
const AF_NETLINK: i32 = 16;
const AF_PACKET: i32 = 17;
const AF_ALG: i32 = 38;

const SOCK_STREAM: i32 = 1;
const SOCK_DGRAM: i32 = 2;
const SOCK_SEQPACKET: i32 = 5;
const SOCK_RAW: i32 = 3;

const MSG_DONTWAIT: i32 = 0x40;

const ENOTSOCK: i32 = 88;
const EMSGSIZE: i32 = 90;
const EPIPE: i32 = 32;
const ECONNREFUSED: i32 = 111;
const EDESTADDRREQ: i32 = 89;
const ENETUNREACH: i32 = 101;

const SOCKADDR_MAX: usize = 128;
const MAX_PAYLOAD: usize = 65535;

#[repr(C)]
pub struct SendtoView {
    pub s: *mut c_void,
    pub net_ns: *mut *mut c_void,
    pub domain: *mut i32,
    pub sock_type: *mut i32,
    pub protocol: *mut i32,
    pub nonblock: *mut i32,
    pub closed: *mut i32,
    pub shut_wr: *mut i32,
    pub bound: *mut i32,
    pub connected: *mut i32,
    pub ever_connected: *mut i32,
    pub local_tcp: *mut i32,
    pub tcp: *mut *mut c_void,
    pub udp: *mut *mut c_void,
    pub raw: *mut *mut c_void,
    pub peer: *mut *mut c_void,
    pub local: *mut u8,
    pub local_len: *mut usize,
    pub peer_addr: *mut u8,
    pub peer_len: *mut usize,
    pub send_timeout_ticks: *mut u64,
}

#[repr(C)]
pub struct SendtoPair {
    pub lo: *mut c_void,
    pub hi: *mut c_void,
    pub flags: u64,
}

unsafe extern "C" {
    fn a20_net_sendto_view_init(s: *mut c_void, out: *mut SendtoView);
    fn a20_net_sendto_flags_check(flags: i32) -> i32;
    fn a20_net_sendto_alg(s: *mut c_void, buf: *const c_void, len: usize) -> i32;
    fn a20_net_sendto_packet(s: *mut c_void, buf: *const c_void, len: usize,
                             addr: *const c_void, addrlen: usize) -> i32;
    fn a20_net_sendto_netlink(s: *mut c_void, buf: *const c_void, len: usize,
                              addr: *const c_void, addrlen: usize) -> i32;
    fn a20_net_sendto_ns_initial(s: *mut c_void) -> i32;
    fn a20_net_sendto_raw_ipv6(s: *mut c_void, buf: *const c_void, len: usize,
                               addr: *const c_void, addrlen: usize) -> i32;
    fn a20_net_sendto_inet(s: *mut c_void, buf: *const c_void, len: usize,
                           flags: i32, addr: *const c_void, addrlen: usize) -> i32;
    fn a20_net_sendto_notconn(s: *mut c_void) -> i32;
    fn a20_net_sendto_unix(s: *mut c_void, buf: *const c_void, len: usize,
                           addr: *const c_void, addrlen: usize) -> i32;
    fn a20_net_sendto_lock(s: *mut c_void) -> u64;
    fn a20_net_sendto_unlock(s: *mut c_void, flags: u64);
    fn a20_net_sendto_is_live(s: *mut c_void) -> i32;
    fn a20_net_sendto_autobind(s: *mut c_void);
    fn a20_net_sendto_ref(s: *mut c_void) -> *mut c_void;
    fn a20_net_sendto_free(s: *mut c_void);
    fn a20_net_sendto_lock2(a: *mut c_void, b: *mut c_void, out: *mut SendtoPair);
    fn a20_net_sendto_unlock2(pair: *const SendtoPair);
    fn a20_net_sendto_find_bound(ns: *mut c_void, domain: i32, sock_type: i32,
                                 addr: *const c_void, addrlen: usize) -> *mut c_void;
    fn a20_net_sendto_max_queue() -> i32;
    fn a20_net_sendto_enqueue_locked(dst: *mut c_void, buf: *const c_void,
                                     len: usize, src: *const c_void,
                                     src_len: usize) -> i32;
    fn a20_net_sendto_enqueue_blocking(s: *mut c_void, dst: *mut c_void,
                                       buf: *const c_void, len: usize,
                                       src: *const c_void, src_len: usize,
                                       dontwait: i32, timeout: u64) -> i32;
    fn a20_net_sendto_collect_read(dst: *mut c_void, wake_q: *mut c_void) -> i32;
    fn a20_net_sendto_wakeq_init(wake_q: *mut c_void);
    fn a20_net_sendto_wakeq_flush(wake_q: *mut c_void);
}

fn zero_view() -> SendtoView {
    SendtoView {
        s: ptr::null_mut(),
        net_ns: ptr::null_mut(),
        domain: ptr::null_mut(),
        sock_type: ptr::null_mut(),
        protocol: ptr::null_mut(),
        nonblock: ptr::null_mut(),
        closed: ptr::null_mut(),
        shut_wr: ptr::null_mut(),
        bound: ptr::null_mut(),
        connected: ptr::null_mut(),
        ever_connected: ptr::null_mut(),
        local_tcp: ptr::null_mut(),
        tcp: ptr::null_mut(),
        udp: ptr::null_mut(),
        raw: ptr::null_mut(),
        peer: ptr::null_mut(),
        local: ptr::null_mut(),
        local_len: ptr::null_mut(),
        peer_addr: ptr::null_mut(),
        peer_len: ptr::null_mut(),
        send_timeout_ticks: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> SendtoView {
    let mut v = zero_view();
    a20_net_sendto_view_init(s, &mut v);
    v
}

/// proc_wake_q_t storage the Rust frame owns; the C bridge asserts the size.
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

#[no_mangle]
pub unsafe extern "C" fn a20_net_sendto_sock(
    s: *mut c_void, buf: *const c_void, len: usize, flags: i32,
    addr: *const c_void, addrlen: usize,
) -> i32 {
    if s.is_null() {
        return -ENOTSOCK;
    }
    let bad = a20_net_sendto_flags_check(flags);
    if bad != 0 {
        return bad;
    }
    let v = view_of(s);
    if len > MAX_PAYLOAD && *v.sock_type != SOCK_STREAM {
        return -EMSGSIZE;
    }
    let dontwait = *v.nonblock != 0 || (flags & MSG_DONTWAIT) != 0;
    if *v.domain == AF_ALG {
        return a20_net_sendto_alg(s, buf, len);
    }
    if *v.domain == AF_PACKET {
        return a20_net_sendto_packet(s, buf, len, addr, addrlen);
    }
    if *v.domain == AF_NETLINK {
        return a20_net_sendto_netlink(s, buf, len, addr, addrlen);
    }
    if a20_net_sendto_ns_initial(s) == 0 && *v.sock_type == SOCK_RAW {
        return -ENETUNREACH;
    }
    if *v.domain == AF_INET6 && *v.sock_type == SOCK_RAW {
        return a20_net_sendto_raw_ipv6(s, buf, len, addr, addrlen);
    }
    if (*v.domain == AF_INET || *v.domain == AF_INET6)
        && (*v.sock_type == SOCK_DGRAM || !(*v.udp).is_null()
            || !(*v.raw).is_null() || !(*v.tcp).is_null())
    {
        return a20_net_sendto_inet(s, buf, len, flags, addr, addrlen);
    }
    /* A stream socket with no pcb, in a family the stack owns, is a connection
     * lwIP tore down (lwip_tcp_err_cb NULLs s->tcp on RST and on every fatal
     * error), not a socket that never had one, so it owes EPIPE.  local_tcp is
     * what keeps this off the fast path: a fast-mode socket drops its pcb by
     * design and is served by the inet dispatch above. */
    if *v.sock_type == SOCK_STREAM && *v.local_tcp == 0 && (*v.tcp).is_null()
        && (*v.domain == AF_INET || *v.domain == AF_INET6)
    {
        return if *v.ever_connected != 0 {
            -EPIPE
        } else {
            a20_net_sendto_notconn(s)
        };
    }
    if *v.domain == AF_UNIX {
        return a20_net_sendto_unix(s, buf, len, addr, addrlen);
    }

    /* Two sockets, taken in ascending address order.  The destination is
     * resolved first, outside any lock, because it comes from a whole-table
     * scan that walks the registry a bucket at a time; the scan returns a
     * reference, which is what lets s's lock be dropped before the pair is
     * taken and is why this path cannot nest a third socket lock. */
    let mut dst: *mut c_void = ptr::null_mut();
    let mut dst_addr: *const c_void = addr;
    let mut dst_len: usize = addrlen;
    let mut dst_scratch = [0u8; SOCKADDR_MAX];
    let mut src_local = [0u8; SOCKADDR_MAX];
    let src_local_len: usize;
    let stream_end: bool;
    let had_peer: bool;
    let send_timeout: u64;
    {
        let sf = a20_net_sendto_lock(s);
        if a20_net_sendto_is_live(s) == 0 {
            a20_net_sendto_unlock(s, sf);
            return -ENOTSOCK;
        }
        if *v.closed != 0 || *v.shut_wr != 0 {
            a20_net_sendto_unlock(s, sf);
            return -EPIPE;
        }
        if *v.bound == 0 && (*v.domain == AF_INET || *v.domain == AF_INET6) {
            a20_net_sendto_autobind(s);
        }
        src_local_len = *v.local_len;
        ptr::copy_nonoverlapping(v.local, src_local.as_mut_ptr(), src_local_len);
        if dst_addr.is_null() && *v.connected != 0 {
            /* Copied out rather than pointed at: the enqueue below runs after
             * this lock is dropped, and s->peer_addr is bind-rewritable. */
            let n = *v.peer_len;
            ptr::copy_nonoverlapping(v.peer_addr, dst_scratch.as_mut_ptr(), n);
            dst_len = n;
            dst_addr = dst_scratch.as_ptr() as *const c_void;
        }
        /* The peer pointer is sampled under s's lock, which is what keeps it
         * from being freed between the test and the reference below. */
        let peer = *v.peer;
        if !peer.is_null()
            && (*v.sock_type == SOCK_STREAM || *v.sock_type == SOCK_SEQPACKET
                || a20_net_sendto_is_live(peer) != 0)
        {
            dst = a20_net_sendto_ref(peer);
        }
        /* Sampled here because every use of it below is with no lock on s. */
        stream_end = *v.sock_type == SOCK_STREAM && *v.ever_connected != 0;
        send_timeout = *v.send_timeout_ticks;
        had_peer = !(*v.peer).is_null();
        a20_net_sendto_unlock(s, sf);
    }
    if dst.is_null() && had_peer {
        /* The peer is gone (or was never a live socket); drop the stale
         * back-pointer and fall back to an address lookup. */
        let sf = a20_net_sendto_lock(s);
        if !(*v.peer).is_null() {
            *v.peer = ptr::null_mut();
        }
        a20_net_sendto_unlock(s, sf);
    }
    if dst.is_null() && !dst_addr.is_null() {
        dst = a20_net_sendto_find_bound(*v.net_ns, *v.domain, *v.sock_type,
                                        dst_addr, dst_len);
    }
    if dst.is_null() {
        /* Linux ABI: writes on a once-connected stream whose endpoint vanished
         * report EPIPE. */
        if *v.sock_type == SOCK_STREAM && *v.ever_connected != 0 {
            return -EPIPE;
        }
        return if !dst_addr.is_null() { -ECONNREFUSED } else { -EDESTADDRREQ };
    }

    let mut pair = SendtoPair { lo: ptr::null_mut(), hi: ptr::null_mut(), flags: 0 };
    a20_net_sendto_lock2(s, dst, &mut pair);
    /* The search above ran without a lock, so the destination may have been
     * closed in between; the reference taken on it keeps it from being freed
     * underneath, so a liveness re-check is all that is left to do. */
    if a20_net_sendto_is_live(dst) == 0 {
        a20_net_sendto_unlock2(&pair);
        a20_net_sendto_free(dst);
        return if stream_end {
            -EPIPE
        } else if !dst_addr.is_null() {
            -ECONNREFUSED
        } else {
            -EDESTADDRREQ
        };
    }
    if len <= MAX_PAYLOAD {
        let mut wq = WakeQ::new();
        a20_net_sendto_wakeq_init(wq.as_mut_ptr());
        if dst_rx_count(dst) >= a20_net_sendto_max_queue() && !dontwait {
            a20_net_sendto_unlock2(&pair);
            let br = a20_net_sendto_enqueue_blocking(s, dst, buf, len,
                                                     src_local.as_ptr() as *const c_void,
                                                     src_local_len,
                                                     if dontwait { 1 } else { 0 },
                                                     send_timeout);
            a20_net_sendto_free(dst);
            return br;
        }
        let r = a20_net_sendto_enqueue_locked(dst, buf, len,
                                              src_local.as_ptr() as *const c_void,
                                              src_local_len);
        if r >= 0 {
            a20_net_sendto_collect_read(dst, wq.as_mut_ptr());
        }
        a20_net_sendto_unlock2(&pair);
        a20_net_sendto_wakeq_flush(wq.as_mut_ptr());
        a20_net_sendto_free(dst);
        return r;
    }
    a20_net_sendto_unlock2(&pair);

    let mut total = 0usize;
    while total < len {
        let mut chunk = len - total;
        if chunk > MAX_PAYLOAD {
            chunk = MAX_PAYLOAD;
        }
        let br = a20_net_sendto_enqueue_blocking(
            s, dst, (buf as *const u8).add(total) as *const c_void, chunk,
            src_local.as_ptr() as *const c_void, src_local_len,
            if dontwait { 1 } else { 0 }, send_timeout);
        if br < 0 {
            a20_net_sendto_free(dst);
            return if total != 0 { total as i32 } else { br };
        }
        total += br as usize;
    }
    a20_net_sendto_free(dst);
    total as i32
}

unsafe fn dst_rx_count(dst: *mut c_void) -> i32 {
    a20_net_sendto_dst_rx_count(dst)
}

unsafe extern "C" {
    fn a20_net_sendto_dst_rx_count(dst: *mut c_void) -> i32;
}


#[cfg(test)]
#[allow(static_mut_refs)]
mod tests {
    use super::*;

    #[repr(C)]
    struct Sock {
        net_ns: *mut c_void,
        domain: i32,
        sock_type: i32,
        protocol: i32,
        nonblock: i32,
        closed: i32,
        shut_wr: i32,
        bound: i32,
        connected: i32,
        ever_connected: i32,
        local_tcp: i32,
        tcp: *mut c_void,
        udp: *mut c_void,
        raw: *mut c_void,
        peer: *mut c_void,
        local: [u8; SOCKADDR_MAX],
        local_len: usize,
        peer_addr: [u8; SOCKADDR_MAX],
        peer_len: usize,
        send_timeout_ticks: u64,
        rx_count: i32,
    }

    impl Sock {
        const fn new() -> Sock {
            Sock {
                net_ns: 0x1000 as *mut c_void,
                domain: AF_INET,
                sock_type: SOCK_STREAM,
                protocol: 0,
                nonblock: 0,
                closed: 0,
                shut_wr: 0,
                bound: 1,
                connected: 1,
                ever_connected: 1,
                local_tcp: 0,
                tcp: 0x2000 as *mut c_void,
                udp: ptr::null_mut(),
                raw: ptr::null_mut(),
                peer: ptr::null_mut(),
                local: [0u8; SOCKADDR_MAX],
                local_len: 16,
                peer_addr: [0u8; SOCKADDR_MAX],
                peer_len: 16,
                send_timeout_ticks: 0,
                rx_count: 0,
            }
        }
    }

    static mut S_SOCK: Sock = Sock::new();
    static mut S_DST: Sock = Sock::new();
    static mut T_FLAGS_RC: i32 = 0;
    static mut T_ALG: i32 = 0;
    static mut T_PACKET: i32 = 0;
    static mut T_NETLINK: i32 = 0;
    static mut T_NS_INITIAL: i32 = 1;
    static mut T_RAW6: i32 = 0;
    static mut T_INET: i32 = 0;
    static mut T_NOTCONN: i32 = -107;
    static mut T_UNIX: i32 = 0;
    static mut T_SELF_LIVE: i32 = 1;
    static mut T_LIVE: [i32; 4] = [1, 1, 1, 1];
    static mut T_LIVE_N: usize = 0;
    static mut T_AUTOBIND: i32 = 0;
    static mut T_REF: i32 = 0;
    static mut T_FREE: i32 = 0;
    static mut T_FIND: i32 = 0;
    static mut T_FIND_RET: i32 = 0;
    static mut T_ENQ_LOCKED: i32 = 0;
    static mut T_ENQ_LOCKED_LEN: i32 = 0;
    static mut T_ENQ_BLOCKING: i32 = 0;
    static mut T_ENQ_BLOCKING_LEN: i32 = 0;
    static mut T_ENQ_BLOCKING_DW: i32 = -1;
    static mut T_COLLECT: i32 = 0;
    static mut T_MAX_QUEUE: i32 = 4;

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
        S_DST = Sock::new();
        T_FLAGS_RC = 0;
        T_ALG = 0;
        T_PACKET = 0;
        T_NETLINK = 0;
        T_NS_INITIAL = 1;
        T_RAW6 = 0;
        T_INET = 0;
        T_NOTCONN = -107;
        T_UNIX = 0;
        T_SELF_LIVE = 1;
        T_LIVE = [1, 1, 1, 1];
        T_LIVE_N = 0;
        T_AUTOBIND = 0;
        T_REF = 0;
        T_FREE = 0;
        T_FIND = 0;
        T_FIND_RET = 0;
        T_ENQ_LOCKED = 0;
        T_ENQ_LOCKED_LEN = 0;
        T_ENQ_BLOCKING = 0;
        T_ENQ_BLOCKING_LEN = 0;
        T_ENQ_BLOCKING_DW = -1;
        T_COLLECT = 0;
        T_MAX_QUEUE = 4;
    }

    unsafe fn sock() -> *mut c_void {
        ptr::addr_of_mut!(S_SOCK) as *mut c_void
    }

    unsafe fn dst() -> *mut c_void {
        ptr::addr_of_mut!(S_DST) as *mut c_void
    }

    unsafe fn me() -> &'static mut Sock {
        &mut *(sock() as *mut Sock)
    }

    /* The generic two-socket path is what a family the ladder does not claim
     * falls through to.  AF_UNSPEC keeps every dispatch test false while the
     * type still selects the stream semantics (the chunk loop, EPIPE) or the
     * datagram ones. */
    unsafe fn generic(stream: bool) {
        me().domain = 0;
        me().sock_type = if stream { SOCK_STREAM } else { SOCK_SEQPACKET };
        me().tcp = ptr::null_mut();
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_view_init(s: *mut c_void,
                                                  out: *mut SendtoView) {
        let k = &mut *(s as *mut Sock);
        *out = SendtoView {
            s,
            net_ns: &mut k.net_ns,
            domain: &mut k.domain,
            sock_type: &mut k.sock_type,
            protocol: &mut k.protocol,
            nonblock: &mut k.nonblock,
            closed: &mut k.closed,
            shut_wr: &mut k.shut_wr,
            bound: &mut k.bound,
            connected: &mut k.connected,
            ever_connected: &mut k.ever_connected,
            local_tcp: &mut k.local_tcp,
            tcp: &mut k.tcp,
            udp: &mut k.udp,
            raw: &mut k.raw,
            peer: &mut k.peer,
            local: k.local.as_mut_ptr(),
            local_len: &mut k.local_len,
            peer_addr: k.peer_addr.as_mut_ptr(),
            peer_len: &mut k.peer_len,
            send_timeout_ticks: &mut k.send_timeout_ticks,
        };
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_flags_check(_f: i32) -> i32 {
        T_FLAGS_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_alg(_s: *mut c_void, _b: *const c_void,
                                            _l: usize) -> i32 {
        T_ALG += 1;
        11
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_packet(_s: *mut c_void, _b: *const c_void,
                                               _l: usize, _a: *const c_void,
                                               _al: usize) -> i32 {
        T_PACKET += 1;
        12
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_netlink(_s: *mut c_void, _b: *const c_void,
                                                _l: usize, _a: *const c_void,
                                                _al: usize) -> i32 {
        T_NETLINK += 1;
        13
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_ns_initial(_s: *mut c_void) -> i32 {
        T_NS_INITIAL
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_raw_ipv6(_s: *mut c_void, _b: *const c_void,
                                                 _l: usize, _a: *const c_void,
                                                 _al: usize) -> i32 {
        T_RAW6 += 1;
        14
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_inet(_s: *mut c_void, _b: *const c_void,
                                             _l: usize, _f: i32,
                                             _a: *const c_void, _al: usize) -> i32 {
        T_INET += 1;
        15
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_notconn(_s: *mut c_void) -> i32 {
        T_NOTCONN
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_unix(_s: *mut c_void, _b: *const c_void,
                                             _l: usize, _a: *const c_void,
                                             _al: usize) -> i32 {
        T_UNIX += 1;
        16
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_lock(_s: *mut c_void) -> u64 {
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_unlock(_s: *mut c_void, _f: u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_is_live(s: *mut c_void) -> i32 {
        if s as usize == ptr::addr_of_mut!(S_SOCK) as usize {
            return T_SELF_LIVE;
        }
        let i = T_LIVE_N;
        T_LIVE_N += 1;
        if i < T_LIVE.len() {
            T_LIVE[i]
        } else {
            1
        }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_autobind(_s: *mut c_void) {
        T_AUTOBIND += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_ref(_s: *mut c_void) -> *mut c_void {
        T_REF += 1;
        dst()
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_free(_s: *mut c_void) {
        T_FREE += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_lock2(a: *mut c_void, b: *mut c_void,
                                              out: *mut SendtoPair) {
        (*out).lo = a;
        (*out).hi = b;
        (*out).flags = 0;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_unlock2(_p: *const SendtoPair) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_find_bound(_ns: *mut c_void, _d: i32,
                                                   _t: i32, _a: *const c_void,
                                                   _al: usize) -> *mut c_void {
        T_FIND += 1;
        if T_FIND_RET != 0 {
            dst()
        } else {
            ptr::null_mut()
        }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_max_queue() -> i32 {
        T_MAX_QUEUE
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_dst_rx_count(s: *mut c_void) -> i32 {
        (*(s as *mut Sock)).rx_count
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_enqueue_locked(_d: *mut c_void,
                                                       _b: *const c_void,
                                                       len: usize,
                                                       _s: *const c_void,
                                                       _sl: usize) -> i32 {
        T_ENQ_LOCKED += 1;
        T_ENQ_LOCKED_LEN = len as i32;
        len as i32
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_enqueue_blocking(
        _s: *mut c_void, _d: *mut c_void, _b: *const c_void, len: usize,
        _src: *const c_void, _sl: usize, dw: i32, _t: u64) -> i32 {
        T_ENQ_BLOCKING += 1;
        T_ENQ_BLOCKING_LEN = len as i32;
        T_ENQ_BLOCKING_DW = dw;
        len as i32
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_collect_read(_d: *mut c_void,
                                                     _w: *mut c_void) -> i32 {
        T_COLLECT += 1;
        1
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_wakeq_init(_w: *mut c_void) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_sendto_wakeq_flush(_w: *mut c_void) {}

    #[test]
    fn the_early_refusals_come_before_any_dispatch() {
        unsafe {
            let _serial = serial();
            reset();
            assert_eq!(a20_net_sendto_sock(ptr::null_mut(), ptr::null_mut(), 1, 0,
                                           ptr::null_mut(), 0), -ENOTSOCK);
            reset();
            T_FLAGS_RC = -95;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 1, 0x1,
                                           ptr::null_mut(), 0), -95);
            assert_eq!(T_INET, 0);
            /* An oversized datagram is refused before any dispatch. */
            reset();
            me().sock_type = SOCK_DGRAM;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(),
                                           MAX_PAYLOAD + 1, 0, ptr::null_mut(), 0),
                       -EMSGSIZE);
            assert_eq!(T_INET, 0);
            /* A stream is allowed one: it is the chunk loop's case. */
            reset();
            generic(true);
            me().peer = dst();
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(),
                                           MAX_PAYLOAD + 1, 0, ptr::null_mut(), 0),
                       (MAX_PAYLOAD + 1) as i32);
        }
    }

    #[test]
    fn the_dispatch_ladder_takes_the_family_before_anything_else() {
        unsafe {
            let _serial = serial();
            reset();
            me().domain = AF_ALG;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 1, 0,
                                           ptr::null_mut(), 0), 11);
            reset();
            me().domain = AF_PACKET;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 1, 0,
                                           ptr::null_mut(), 0), 12);
            reset();
            me().domain = AF_NETLINK;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 1, 0,
                                           ptr::null_mut(), 0), 13);
            /* A raw socket outside the initial namespace is unreachable. */
            reset();
            me().sock_type = SOCK_RAW;
            T_NS_INITIAL = 0;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 1, 0,
                                           ptr::null_mut(), 0), -ENETUNREACH);
            assert_eq!(T_RAW6, 0);
            /* A raw v6 socket inside it goes to the raw send. */
            reset();
            let s = me();
            s.sock_type = SOCK_RAW;
            s.domain = AF_INET6;
            s.tcp = ptr::null_mut();
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 1, 0,
                                           ptr::null_mut(), 0), 14);
        }
    }

    #[test]
    fn a_pcb_or_a_datagram_takes_the_inet_send() {
        unsafe {
            let _serial = serial();
            reset();
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), 15);
            assert_eq!(T_INET, 1);
            assert_eq!(T_UNIX, 0);
            /* A datagram takes it with no pcb of any kind. */
            reset();
            let s = me();
            s.sock_type = SOCK_DGRAM;
            s.tcp = ptr::null_mut();
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), 15);
            /* A raw socket with a pcb takes it too. */
            reset();
            let s = me();
            s.sock_type = SOCK_RAW;
            s.raw = 0x3000 as *mut c_void;
            s.tcp = ptr::null_mut();
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), 15);
        }
    }

    #[test]
    fn a_torn_down_stream_owes_epipe_and_a_never_connected_one_owes_notconn() {
        unsafe {
            let _serial = serial();
            reset();
            me().tcp = ptr::null_mut();
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), -EPIPE);
            assert_eq!(T_UNIX, 0);
            assert_eq!(T_ENQ_LOCKED, 0);
            reset();
            let s = me();
            s.tcp = ptr::null_mut();
            s.ever_connected = 0;
            s.connected = 0;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), -107);
            /* A fast-mode socket drops its pcb by design, so the EPIPE
             * shortcut has to leave it alone: it is served by the generic
             * path, which is where a fast-mode peer lives. */
            reset();
            generic(true);
            let s = me();
            s.local_tcp = 1;
            s.peer = dst();
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), 4);
            assert_eq!(T_ENQ_LOCKED, 1);
        }
    }

    #[test]
    fn a_unix_socket_takes_the_unix_send() {
        unsafe {
            let _serial = serial();
            reset();
            me().domain = AF_UNIX;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), 16);
            assert_eq!(T_UNIX, 1);
        }
    }

    #[test]
    fn the_destination_comes_from_the_peer_then_from_the_address() {
        unsafe {
            let _serial = serial();
            reset();
            generic(true);
            me().peer = dst();
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), 4);
            assert_eq!(T_REF, 1);
            assert_eq!(T_FIND, 0);
            assert_eq!(T_FREE, 1);
            assert_eq!(T_ENQ_LOCKED, 1);
            assert_eq!(T_COLLECT, 1);

            /* No peer but an address: the bound-socket scan finds it. */
            reset();
            generic(true);
            me().connected = 0;
            T_FIND_RET = 1;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           0x9 as *const c_void, 4), 4);
            assert_eq!(T_FIND, 1);
        }
    }

    #[test]
    fn the_four_error_answers_when_no_destination_is_found() {
        unsafe {
            let _serial = serial();
            reset();
            generic(false);
            let s = me();
            s.connected = 0;
            s.ever_connected = 0;
            /* No peer, no address: EDESTADDRREQ. */
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), -EDESTADDRREQ);
            /* An address that resolves to nothing: ECONNREFUSED. */
            reset();
            generic(false);
            let s = me();
            s.connected = 0;
            s.ever_connected = 0;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           0x9 as *const c_void, 4), -ECONNREFUSED);
            /* A once-connected stream: EPIPE even with no address at all. */
            reset();
            generic(true);
            me().connected = 0;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), -EPIPE);
        }
    }

    #[test]
    fn a_destination_that_died_after_the_lookup_keeps_the_same_answer() {
        unsafe {
            let _serial = serial();
            reset();
            generic(true);
            me().peer = dst();
            T_LIVE = [0, 0, 0, 0];
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), -EPIPE);
            assert_eq!(T_FREE, 1);
            assert_eq!(T_ENQ_LOCKED, 0);

            /* A datagram-style socket with an address owes ECONNREFUSED. */
            reset();
            generic(false);
            let s = me();
            s.peer = dst();
            s.connected = 0;
            T_LIVE = [0, 0, 0, 0];
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           0x9 as *const c_void, 4), -ECONNREFUSED);
        }
    }

    #[test]
    fn a_stale_peer_is_dropped_and_the_address_is_tried() {
        unsafe {
            let _serial = serial();
            reset();
            /* A type that is neither stream nor seqpacket: a peer that is not
             * live is not referenced, so the stale back-pointer is cleared and
             * the address scan runs. */
            let s = me();
            s.domain = AF_INET;
            s.sock_type = SOCK_RAW;
            s.tcp = ptr::null_mut();
            s.connected = 0;
            s.ever_connected = 0;
            s.peer = dst();
            T_LIVE = [0, 0, 0, 0];
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           0x9 as *const c_void, 4), -ECONNREFUSED);
            assert_eq!(T_REF, 0);
            assert_eq!(T_FIND, 1);
            assert_eq!(me().peer, ptr::null_mut());
        }
    }

    #[test]
    fn the_queue_full_handoff_and_the_single_enqueue() {
        unsafe {
            let _serial = serial();
            reset();
            generic(true);
            me().peer = dst();
            T_MAX_QUEUE = 4;
            (*dst().cast::<Sock>()).rx_count = 4;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), 4);
            assert_eq!(T_ENQ_BLOCKING, 1);
            assert_eq!(T_ENQ_BLOCKING_DW, 0);
            assert_eq!(T_ENQ_LOCKED, 0);
            assert_eq!(T_COLLECT, 0);
            assert_eq!(T_FREE, 1);

            /* Below the cap: the locked enqueue and the reader wake. */
            reset();
            generic(true);
            me().peer = dst();
            (*dst().cast::<Sock>()).rx_count = 0;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), 4);
            assert_eq!(T_ENQ_LOCKED, 1);
            assert_eq!(T_ENQ_BLOCKING, 0);
            assert_eq!(T_COLLECT, 1);
        }
    }

    #[test]
    fn a_full_queue_does_not_hand_off_when_the_caller_will_not_wait() {
        unsafe {
            let _serial = serial();
            reset();
            generic(true);
            let s = me();
            s.peer = dst();
            s.nonblock = 1;
            (*dst().cast::<Sock>()).rx_count = 4;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), 4);
            assert_eq!(T_ENQ_BLOCKING, 0);
            assert_eq!(T_ENQ_LOCKED, 1);

            /* MSG_DONTWAIT without the socket flag does the same. */
            reset();
            generic(true);
            me().peer = dst();
            (*dst().cast::<Sock>()).rx_count = 4;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4,
                                           MSG_DONTWAIT, ptr::null_mut(), 0), 4);
            assert_eq!(T_ENQ_BLOCKING, 0);
            assert_eq!(T_ENQ_LOCKED, 1);
        }
    }

    #[test]
    fn the_autobind_runs_only_for_an_unbound_inet_socket() {
        unsafe {
            let _serial = serial();
            /* A non-inet family: the autobind stays off. */
            reset();
            generic(true);
            let s = me();
            s.bound = 0;
            s.peer = dst();
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), 4);
            assert_eq!(T_AUTOBIND, 0);

            /* An inet family that reaches the generic path: it runs. */
            reset();
            let s = me();
            s.domain = AF_INET;
            s.sock_type = SOCK_RAW;
            s.tcp = ptr::null_mut();
            s.bound = 0;
            s.peer = dst();
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), 4);
            assert_eq!(T_AUTOBIND, 1);
        }
    }

    #[test]
    fn the_chunk_loop_sends_one_message_at_a_time() {
        unsafe {
            let _serial = serial();
            reset();
            generic(true);
            me().peer = dst();
            let len = MAX_PAYLOAD + 10;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), len, 0,
                                           ptr::null_mut(), 0), len as i32);
            assert_eq!(T_ENQ_BLOCKING, 2);
            assert_eq!(T_ENQ_BLOCKING_LEN, 10);
            assert_eq!(T_ENQ_LOCKED, 0);
        }
    }

    #[test]
    fn a_closed_or_shut_socket_owes_epipe_before_anything_else() {
        unsafe {
            let _serial = serial();
            reset();
            generic(true);
            me().closed = 1;
            me().peer = dst();
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), -EPIPE);
            assert_eq!(T_ENQ_LOCKED, 0);
            reset();
            generic(true);
            me().shut_wr = 1;
            me().peer = dst();
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), -EPIPE);
            /* A socket that is not live at all is ENOTSOCK, not EPIPE. */
            reset();
            generic(true);
            me().peer = dst();
            T_SELF_LIVE = 0;
            assert_eq!(a20_net_sendto_sock(sock(), ptr::null_mut(), 4, 0,
                                           ptr::null_mut(), 0), -ENOTSOCK);
        }
    }
}
