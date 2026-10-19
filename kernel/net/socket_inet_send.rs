#![no_std]

//! The UDP and raw send transactions: the datagram ceiling, the lazy bind, the
//! destination resolution, the local-delivery choice and the stack path.
//!
//! The C side keeps the primitives this module calls: the socket lock and pair,
//! the reference and free, the pbuf alloc/fill/free, the address conversion, the
//! IP-option snapshot and its reapply, the lane lock and the per-protocol shared
//! lock, the lwIP send calls, the receive-queue enqueue (immediate and blocking),
//! the wakeup, the namespace tests and the allocator.
//!
//! Everything that is a *decision* is here: refusing a datagram over the socket's
//! send ceiling before anything is allocated, the lazy bind (a v6 loopback
//! address versus the v4 any-address udp_bind with its lane store), resolving the
//! destination from the connected peer or the table and dropping a stale
//! back-pointer, the namespace refusal, delivering to a local socket through a
//! blocking or immediate enqueue, and the stack path's ordering and status
//! mapping.

use core::ffi::c_void;
use core::ptr;

const AF_INET6: i32 = 10;
const MSG_DONTWAIT: i32 = 0x40;

const ENOMEM: i32 = 12;
const EIO: i32 = 5;
const EMSGSIZE: i32 = 90;
const ENOTSOCK: i32 = 88;
const EDESTADDRREQ: i32 = 89;
const ECONNREFUSED: i32 = 111;

const SHARED_UDP: i32 = 0;
const SHARED_RAW: i32 = 1;

/// `ip_addr_t`, opaque here: C fills it and hands it to the send call.
#[repr(C, align(8))]
struct IpBuf([u8; 32]);

impl IpBuf {
    fn new() -> IpBuf {
        IpBuf([0u8; 32])
    }

    fn as_mut_ptr(&mut self) -> *mut c_void {
        self.0.as_mut_ptr() as *mut c_void
    }
}

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
pub struct SendView {
    pub s: *mut c_void,
    pub net_ns: *mut *mut c_void,
    pub domain: *mut i32,
    pub bound: *mut i32,
    pub connected: *mut i32,
    pub nonblock: *mut i32,
    pub send_timeout_ticks: *mut u64,
    pub rx_count: *mut i32,
    pub local: *mut u8,
    pub local_len: *mut usize,
    pub peer_addr: *mut u8,
    pub peer_len: *mut usize,
    pub peer: *mut *mut c_void,
    pub udp: *mut *mut c_void,
    pub raw: *mut *mut c_void,
}

#[repr(C)]
pub struct SendPair {
    pub lo: *mut c_void,
    pub hi: *mut c_void,
    pub flags: u64,
}

impl SendPair {
    fn zero() -> SendPair {
        SendPair { lo: ptr::null_mut(), hi: ptr::null_mut(), flags: 0 }
    }
}

unsafe extern "C" {
    fn a20_net_inet_send_view_init(s: *mut c_void, out: *mut SendView);
    fn a20_net_inet_send_lock(s: *mut c_void, flags: *mut u64);
    fn a20_net_inet_send_unlock(s: *mut c_void, flags: u64);
    fn a20_net_inet_send_lock2(a: *mut c_void, b: *mut c_void,
                               pair: *mut SendPair);
    fn a20_net_inet_send_unlock2(pair: *mut SendPair);
    fn a20_net_inet_send_is_live(s: *mut c_void) -> i32;
    fn a20_net_inet_send_ref(s: *mut c_void);
    fn a20_net_inet_send_free(s: *mut c_void);
    fn a20_net_inet_send_buf_in_force(s: *mut c_void) -> u32;
    fn a20_net_inet_send_ephemeral_port() -> u16;
    fn a20_net_inet_send_bind_v6(s: *mut c_void, port: u16);
    fn a20_net_inet_send_bind_v4(s: *mut c_void, port: u16);
    fn a20_net_inet_send_find_udp_dst(s: *mut c_void, addr: *const c_void,
                                      len: usize) -> *mut c_void;
    fn a20_net_inet_send_namespace_target(s: *mut c_void, addr: *const c_void,
                                          len: usize) -> i32;
    fn a20_net_inet_send_ns_initial(s: *mut c_void) -> i32;
    fn a20_net_inet_send_enqueue_locked(dst: *mut c_void, buf: *const c_void,
                                        len: usize, src_local: *const u8,
                                        src_len: usize) -> i32;
    fn a20_net_inet_send_enqueue_blocking(s: *mut c_void, dst: *mut c_void,
                                          buf: *const c_void, len: usize,
                                          src_local: *const u8, src_len: usize,
                                          dontwait: i32, timeout: u64) -> i32;
    fn a20_net_inet_send_collect_read(dst: *mut c_void, wake_q: *mut c_void);
    fn a20_net_inet_send_wakeq_init(wake_q: *mut c_void);
    fn a20_net_inet_send_wakeq_flush(wake_q: *mut c_void);
    fn a20_net_inet_send_pbuf_alloc(len: usize) -> *mut c_void;
    fn a20_net_inet_send_pbuf_take(p: *mut c_void, buf: *const c_void, len: usize);
    fn a20_net_inet_send_pbuf_free(p: *mut c_void);
    fn a20_net_inet_send_sockaddr_to_ip(addr: *const c_void, len: usize,
                                        ip: *mut c_void, port: *mut u16) -> i32;
    fn a20_net_inet_send_ip_lane_lock(s: *mut c_void, opts: *mut c_void) -> u64;
    fn a20_net_inet_send_lane_unlock(flags: u64);
    fn a20_net_inet_send_ip_opts_apply(s: *mut c_void, opts: *const c_void);
    fn a20_net_inet_send_udp_sendto(pcb: *mut c_void, p: *mut c_void,
                                    ip: *const c_void, port: u16) -> i32;
    fn a20_net_inet_send_udp_send(pcb: *mut c_void, p: *mut c_void) -> i32;
    fn a20_net_inet_send_udp_connected(pcb: *mut c_void) -> i32;
    fn a20_net_inet_send_raw_sendto(pcb: *mut c_void, p: *mut c_void,
                                    ip: *const c_void) -> i32;
    fn a20_net_inet_send_raw_send(pcb: *mut c_void, p: *mut c_void) -> i32;
    fn a20_net_inet_send_raw_connected(pcb: *mut c_void) -> i32;
    fn a20_net_inet_send_shared_lock(kind: i32, pcb: *mut c_void) -> u64;
    fn a20_net_inet_send_shared_unlock(kind: i32, pcb: *mut c_void, flags: u64);
    fn a20_net_inet_send_poll_locked();
}

fn zero_view() -> SendView {
    SendView {
        s: ptr::null_mut(),
        net_ns: ptr::null_mut(),
        domain: ptr::null_mut(),
        bound: ptr::null_mut(),
        connected: ptr::null_mut(),
        nonblock: ptr::null_mut(),
        send_timeout_ticks: ptr::null_mut(),
        rx_count: ptr::null_mut(),
        local: ptr::null_mut(),
        local_len: ptr::null_mut(),
        peer_addr: ptr::null_mut(),
        peer_len: ptr::null_mut(),
        peer: ptr::null_mut(),
        udp: ptr::null_mut(),
        raw: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> SendView {
    let mut v = zero_view();
    a20_net_inet_send_view_init(s, &mut v);
    v
}

/// The UDP send transaction.
#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_send_udp(s: *mut c_void, buf: *const c_void,
                                               len: usize, flags: i32,
                                               addr: *const c_void, addrlen: usize,
                                               max_queue: i32) -> i32 {
    let v = view_of(s);
    /* The one datagram this socket will hand on is the only send-side buffer a
     * datagram socket has, and the number that rejects it is the number
     * getsockopt reports. */
    if len > a20_net_inet_send_buf_in_force(s) as usize {
        return -EMSGSIZE;
    }

    if *v.bound == 0 {
        let port = a20_net_inet_send_ephemeral_port();
        if *v.domain == AF_INET6 {
            a20_net_inet_send_bind_v6(s, port);
        } else {
            a20_net_inet_send_bind_v4(s, port);
        }
    }

    /* The destination is resolved first, with no lock held, because the table
     * scan walks one bucket at a time and returns a reference. */
    let mut src_local = [0u8; 128];
    let src_len;
    let mut dst_addr: *const u8 = addr as *const u8;
    let mut dst_len = addrlen;
    let mut peer_copy = [0u8; 128];
    let dontwait;
    let send_timeout;
    let had_peer;
    let mut local_dst: *mut c_void = ptr::null_mut();
    {
        let mut sf = 0u64;
        a20_net_inet_send_lock(s, &mut sf);
        if a20_net_inet_send_is_live(s) == 0 {
            a20_net_inet_send_unlock(s, sf);
            return -ENOTSOCK;
        }
        if dst_addr.is_null() && *v.connected != 0 {
            dst_len = *v.peer_len;
            ptr::copy_nonoverlapping(v.peer_addr, peer_copy.as_mut_ptr(), dst_len);
            dst_addr = peer_copy.as_ptr();
        }
        src_len = *v.local_len;
        ptr::copy_nonoverlapping(v.local, src_local.as_mut_ptr(), src_len);
        let peer = *v.peer;
        had_peer = !peer.is_null();
        if had_peer && a20_net_inet_send_is_live(peer) != 0 {
            a20_net_inet_send_ref(peer);
            local_dst = peer;
        }
        dontwait = if *v.nonblock != 0 || (flags & MSG_DONTWAIT) != 0 { 1 } else { 0 };
        send_timeout = *v.send_timeout_ticks;
        a20_net_inet_send_unlock(s, sf);
    }
    if local_dst.is_null() && had_peer {
        /* The back-pointer is stale: drop it and fall back to a lookup. */
        let mut sf = 0u64;
        a20_net_inet_send_lock(s, &mut sf);
        if !(*v.peer).is_null() {
            *v.peer = ptr::null_mut();
        }
        a20_net_inet_send_unlock(s, sf);
    }
    if local_dst.is_null() && !dst_addr.is_null() {
        local_dst = a20_net_inet_send_find_udp_dst(s, dst_addr as *const c_void,
                                                   dst_len);
    }
    let ns_result = a20_net_inet_send_namespace_target(s, dst_addr as *const c_void,
                                                       dst_len);
    if ns_result < 0 {
        a20_net_inet_send_free(local_dst);
        return ns_result;
    }

    if !local_dst.is_null() {
        let mut pair = SendPair::zero();
        a20_net_inet_send_lock2(s, local_dst, &mut pair);
        if a20_net_inet_send_is_live(local_dst) == 0 {
            a20_net_inet_send_unlock2(&mut pair);
            a20_net_inet_send_free(local_dst);
            return if !dst_addr.is_null() { -ECONNREFUSED } else { -EDESTADDRREQ };
        }
        if *view_of(local_dst).rx_count >= max_queue && dontwait == 0 {
            a20_net_inet_send_unlock2(&mut pair);
            let br = a20_net_inet_send_enqueue_blocking(s, local_dst, buf, len,
                                                        src_local.as_ptr(), src_len,
                                                        dontwait, send_timeout);
            a20_net_inet_send_free(local_dst);
            return br;
        }
        let rr = a20_net_inet_send_enqueue_locked(local_dst, buf, len,
                                                  src_local.as_ptr(), src_len);
        let mut wake_q = WakeQ::new();
        a20_net_inet_send_wakeq_init(wake_q.as_mut_ptr());
        if rr >= 0 {
            a20_net_inet_send_collect_read(local_dst, wake_q.as_mut_ptr());
        }
        a20_net_inet_send_unlock2(&mut pair);
        a20_net_inet_send_wakeq_flush(wake_q.as_mut_ptr());
        a20_net_inet_send_free(local_dst);
        return rr;
    }

    if a20_net_inet_send_ns_initial(s) == 0 {
        return if !dst_addr.is_null() { -ECONNREFUSED } else { -EDESTADDRREQ };
    }
    if *v.domain == AF_INET6 {
        return if !dst_addr.is_null() { -ECONNREFUSED } else { -EDESTADDRREQ };
    }

    let p = a20_net_inet_send_pbuf_alloc(len);
    if p.is_null() {
        return -ENOMEM;
    }
    a20_net_inet_send_pbuf_take(p, buf, len);

    let mut ip = IpBuf::new();
    let mut port: u16 = 0;
    if !addr.is_null() {
        let r = a20_net_inet_send_sockaddr_to_ip(addr, addrlen, ip.as_mut_ptr(),
                                                 &mut port);
        if r < 0 {
            a20_net_inet_send_pbuf_free(p);
            return r;
        }
    }

    let mut opts = OptsBuf::new();
    let lwip_flags = a20_net_inet_send_ip_lane_lock(s, opts.as_mut_ptr());
    let pcb = *v.udp;
    if pcb.is_null() {
        a20_net_inet_send_lane_unlock(lwip_flags);
        a20_net_inet_send_pbuf_free(p);
        return -ENOTSOCK;
    }
    let shared = a20_net_inet_send_shared_lock(SHARED_UDP, pcb);
    a20_net_inet_send_ip_opts_apply(s, opts.as_mut_ptr());
    let e: i32;
    if !addr.is_null() {
        e = a20_net_inet_send_udp_sendto(pcb, p, ip.as_mut_ptr(), port);
    } else if a20_net_inet_send_udp_connected(pcb) != 0 {
        e = a20_net_inet_send_udp_send(pcb, p);
    } else {
        a20_net_inet_send_shared_unlock(SHARED_UDP, pcb, shared);
        a20_net_inet_send_lane_unlock(lwip_flags);
        a20_net_inet_send_pbuf_free(p);
        return -EDESTADDRREQ;
    }
    /* The lwIP raw API retains the caller's pbuf on success and error; release
     * only this side's ownership. */
    a20_net_inet_send_pbuf_free(p);
    a20_net_inet_send_poll_locked();
    a20_net_inet_send_shared_unlock(SHARED_UDP, pcb, shared);
    a20_net_inet_send_lane_unlock(lwip_flags);
    if e == 0 { len as i32 } else { -EIO }
}

/// The raw send transaction.
#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_send_raw(s: *mut c_void, buf: *const c_void,
                                               len: usize, addr: *const c_void,
                                               addrlen: usize, _max_queue: i32) -> i32 {
    let v = view_of(s);
    if a20_net_inet_send_ns_initial(s) == 0 {
        return -ENETUNREACH;
    }
    /* Checked before the pbuf is allocated: the allocation is sized by len, and
     * a request the socket has already said it will not send should not fail for
     * want of memory instead. */
    if len > a20_net_inet_send_buf_in_force(s) as usize {
        return -EMSGSIZE;
    }
    let p = a20_net_inet_send_pbuf_alloc(len);
    if p.is_null() {
        return -ENOMEM;
    }
    a20_net_inet_send_pbuf_take(p, buf, len);

    let mut ip = IpBuf::new();
    if !addr.is_null() {
        let r = a20_net_inet_send_sockaddr_to_ip(addr, addrlen, ip.as_mut_ptr(),
                                                 ptr::null_mut());
        if r < 0 {
            a20_net_inet_send_pbuf_free(p);
            return r;
        }
    }

    let mut opts = OptsBuf::new();
    let lwip_flags = a20_net_inet_send_ip_lane_lock(s, opts.as_mut_ptr());
    let pcb = *v.raw;
    if pcb.is_null() {
        a20_net_inet_send_lane_unlock(lwip_flags);
        a20_net_inet_send_pbuf_free(p);
        return -ENOTSOCK;
    }
    let shared = a20_net_inet_send_shared_lock(SHARED_RAW, pcb);
    a20_net_inet_send_ip_opts_apply(s, opts.as_mut_ptr());
    let e: i32;
    if !addr.is_null() {
        e = a20_net_inet_send_raw_sendto(pcb, p, ip.as_mut_ptr());
    } else if a20_net_inet_send_raw_connected(pcb) != 0 {
        e = a20_net_inet_send_raw_send(pcb, p);
    } else {
        a20_net_inet_send_shared_unlock(SHARED_RAW, pcb, shared);
        a20_net_inet_send_lane_unlock(lwip_flags);
        a20_net_inet_send_pbuf_free(p);
        return -EDESTADDRREQ;
    }
    a20_net_inet_send_pbuf_free(p);
    a20_net_inet_send_poll_locked();
    a20_net_inet_send_shared_unlock(SHARED_RAW, pcb, shared);
    a20_net_inet_send_lane_unlock(lwip_flags);
    if e == 0 { len as i32 } else { -EIO }
}

const ENETUNREACH: i32 = 101;

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
        bound: i32,
        connected: i32,
        nonblock: i32,
        send_timeout_ticks: u64,
        rx_count: i32,
        local: [u8; 128],
        local_len: usize,
        peer_addr: [u8; 128],
        peer_len: usize,
        peer: *mut Sock,
        udp: *mut c_void,
        raw: *mut c_void,
        live: i32,
        /* The view points at this slot: a temporary would leave the consumer
         * comparing a dangling value. */
        ns: *mut c_void,
    }

    impl Sock {
        const fn new() -> Sock {
            Sock {
                domain: 2,
                bound: 0,
                connected: 0,
                nonblock: 0,
                send_timeout_ticks: 0,
                rx_count: 0,
                local: [0u8; 128],
                local_len: 0,
                peer_addr: [0u8; 128],
                peer_len: 0,
                peer: ptr::null_mut(),
                udp: ptr::null_mut(),
                raw: ptr::null_mut(),
                live: 1,
                ns: 1usize as *mut c_void,
            }
        }
    }

    static mut S_SRC: Sock = Sock::new();
    static mut T_PEER: *mut Sock = ptr::null_mut();
    static mut T_BUF_FORCE: u32 = 65535;
    static mut T_PORT: u16 = 40000;
    static mut T_BIND_V4: i32 = 0;
    static mut T_BIND_V6: i32 = 0;
    static mut T_REF: i32 = 0;
    static mut T_FREE: i32 = 0;
    static mut T_FIND: i32 = 0;
    static mut T_NS_TARGET: i32 = 0;
    static mut T_NS_INITIAL: i32 = 1;
    static mut T_ENQ_LOCKED: i32 = 0;
    static mut T_ENQ_BLOCKING: i32 = 0;
    static mut T_ENQ_RC: i32 = 0;
    static mut T_COLLECT: i32 = 0;
    static mut T_PBUF_ALLOC: i32 = 0;
    static mut T_PBUF_FREE: i32 = 0;
    static mut T_TAKE: i32 = 0;
    static mut T_SOCKADDR_RC: i32 = 0;
    static mut T_LANE_LOCK: i32 = 0;
    static mut T_LANE_UNLOCK: i32 = 0;
    static mut T_OPTS_APPLY: i32 = 0;
    static mut T_SHARED_LOCK: i32 = 0;
    static mut T_SHARED_UNLOCK: i32 = 0;
    static mut T_SEND_RC: i32 = 0;
    static mut T_SENDTO_CALLS: i32 = 0;
    static mut T_CONNECTED_FLAG: i32 = 0;
    static mut T_SEND_CALLS: i32 = 0;
    static mut T_POLL: i32 = 0;
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

    unsafe fn trace(tag: u8) {
        if T_TRACE_N < T_TRACE.len() {
            T_TRACE[T_TRACE_N] = tag;
            T_TRACE_N += 1;
        }
    }

    unsafe fn reset() {
        S_SRC = Sock::new();
        T_PEER = ptr::null_mut();
        T_BUF_FORCE = 65535;
        T_PORT = 40000;
        T_BIND_V4 = 0;
        T_BIND_V6 = 0;
        T_REF = 0;
        T_FREE = 0;
        T_FIND = 0;
        T_NS_TARGET = 0;
        T_NS_INITIAL = 1;
        T_ENQ_LOCKED = 0;
        T_ENQ_BLOCKING = 0;
        T_ENQ_RC = 0;
        T_COLLECT = 0;
        T_PBUF_ALLOC = 0;
        T_PBUF_FREE = 0;
        T_TAKE = 0;
        T_SOCKADDR_RC = 0;
        T_LANE_LOCK = 0;
        T_LANE_UNLOCK = 0;
        T_OPTS_APPLY = 0;
        T_SHARED_LOCK = 0;
        T_SHARED_UNLOCK = 0;
        T_SEND_RC = 0;
        T_SENDTO_CALLS = 0;
        T_SEND_CALLS = 0;
        T_CONNECTED_FLAG = 0;
        T_POLL = 0;
        T_TRACE = [0; 32];
        T_TRACE_N = 0;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_view_init(s: *mut c_void,
                                                     out: *mut SendView) {
        let k = &mut *(s as *mut Sock);
        *out = SendView {
            s,
            net_ns: &mut k.ns,
            domain: &mut k.domain,
            bound: &mut k.bound,
            connected: &mut k.connected,
            nonblock: &mut k.nonblock,
            send_timeout_ticks: &mut k.send_timeout_ticks,
            rx_count: &mut k.rx_count,
            local: k.local.as_mut_ptr(),
            local_len: &mut k.local_len,
            peer_addr: k.peer_addr.as_mut_ptr(),
            peer_len: &mut k.peer_len,
            peer: &mut k.peer as *mut *mut Sock as *mut *mut c_void,
            udp: &mut k.udp,
            raw: &mut k.raw,
        };
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_lock(_s: *mut c_void, _f: *mut u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_unlock(_s: *mut c_void, _f: u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_lock2(_a: *mut c_void, _b: *mut c_void,
                                                 pair: *mut SendPair) {
        (*pair).flags = 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_unlock2(pair: *mut SendPair) {
        assert_eq!((*pair).flags, 1);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_is_live(s: *mut c_void) -> i32 {
        (*(s as *const Sock)).live
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_ref(_s: *mut c_void) {
        T_REF += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_free(_s: *mut c_void) {
        T_FREE += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_buf_in_force(_s: *mut c_void) -> u32 {
        T_BUF_FORCE
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_ephemeral_port() -> u16 {
        T_PORT
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_bind_v4(s: *mut c_void, port: u16) {
        assert_eq!(port, T_PORT);
        T_BIND_V4 += 1;
        (*(s as *mut Sock)).bound = 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_bind_v6(s: *mut c_void, port: u16) {
        assert_eq!(port, T_PORT);
        T_BIND_V6 += 1;
        (*(s as *mut Sock)).bound = 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_find_udp_dst(_s: *mut c_void,
                                                        _a: *const c_void,
                                                        _l: usize) -> *mut c_void {
        T_FIND += 1;
        T_PEER as *mut c_void
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_namespace_target(_s: *mut c_void,
                                                            _a: *const c_void,
                                                            _l: usize) -> i32 {
        T_NS_TARGET
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_ns_initial(_s: *mut c_void) -> i32 {
        T_NS_INITIAL
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_enqueue_locked(_d: *mut c_void,
                                                          _b: *const c_void,
                                                          _l: usize,
                                                          _sl: *const u8,
                                                          _sln: usize) -> i32 {
        T_ENQ_LOCKED += 1;
        trace(b'L');
        T_ENQ_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_enqueue_blocking(_s: *mut c_void,
                                                            _d: *mut c_void,
                                                            _b: *const c_void,
                                                            _l: usize,
                                                            _sl: *const u8,
                                                            _sln: usize,
                                                            _dw: i32,
                                                            _t: u64) -> i32 {
        T_ENQ_BLOCKING += 1;
        trace(b'B');
        T_ENQ_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_collect_read(_d: *mut c_void,
                                                        _w: *mut c_void) {
        T_COLLECT += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_wakeq_init(_w: *mut c_void) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_wakeq_flush(_w: *mut c_void) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_pbuf_alloc(_len: usize) -> *mut c_void {
        T_PBUF_ALLOC += 1;
        8usize as *mut c_void
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_pbuf_take(_p: *mut c_void,
                                                     _b: *const c_void, _l: usize) {
        T_TAKE += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_pbuf_free(_p: *mut c_void) {
        T_PBUF_FREE += 1;
        trace(b'F');
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_sockaddr_to_ip(_a: *const c_void,
                                                          _l: usize,
                                                          _ip: *mut c_void,
                                                          _port: *mut u16) -> i32 {
        T_SOCKADDR_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_ip_lane_lock(_s: *mut c_void,
                                                        _o: *mut c_void) -> u64 {
        T_LANE_LOCK += 1;
        trace(b'A');
        3
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_lane_unlock(flags: u64) {
        assert_eq!(flags, 3);
        T_LANE_UNLOCK += 1;
        trace(b'Z');
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_ip_opts_apply(_s: *mut c_void,
                                                         _o: *const c_void) {
        T_OPTS_APPLY += 1;
        trace(b'O');
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_udp_sendto(_p: *mut c_void,
                                                      _b: *mut c_void,
                                                      _ip: *const c_void,
                                                      _port: u16) -> i32 {
        T_SENDTO_CALLS += 1;
        trace(b'S');
        T_SEND_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_udp_send(_p: *mut c_void,
                                                    _b: *mut c_void) -> i32 {
        T_SEND_CALLS += 1;
        trace(b'C');
        T_SEND_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_udp_connected(_pcb: *mut c_void) -> i32 {
        T_CONNECTED_FLAG
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_raw_sendto(_p: *mut c_void,
                                                      _b: *mut c_void,
                                                      _ip: *const c_void) -> i32 {
        T_SENDTO_CALLS += 1;
        trace(b'S');
        T_SEND_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_raw_send(_p: *mut c_void,
                                                    _b: *mut c_void) -> i32 {
        T_SEND_CALLS += 1;
        trace(b'C');
        T_SEND_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_raw_connected(_pcb: *mut c_void) -> i32 {
        T_CONNECTED_FLAG
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_shared_lock(_k: i32,
                                                       _p: *mut c_void) -> u64 {
        T_SHARED_LOCK += 1;
        trace(b'H');
        5
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_shared_unlock(_k: i32, _p: *mut c_void,
                                                         flags: u64) {
        assert_eq!(flags, 5);
        T_SHARED_UNLOCK += 1;
        trace(b'X');
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_send_poll_locked() {
        T_POLL += 1;
        trace(b'P');
    }

    /// A UDP socket with a pcb, so the stack path can reach the send call.
    unsafe fn udp_src() -> *mut Sock {
        let s = ptr::addr_of_mut!(S_SRC);
        (*s).udp = 16usize as *mut c_void;
        (*s).bound = 1;
        s
    }

    #[test]
    fn the_send_ceiling_is_checked_before_anything_else() {
        unsafe {
            let _serial = serial();
            reset();
            let s = udp_src();
            T_BUF_FORCE = 8;
            assert_eq!(a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 9, 0,
                                             ptr::null(), 0, 4), -EMSGSIZE);
            assert_eq!((T_PBUF_ALLOC, T_BIND_V4, T_ENQ_LOCKED), (0, 0, 0));
        }
    }

    #[test]
    fn an_unbound_socket_binds_lazily_per_family() {
        unsafe {
            let _serial = serial();
            reset();
            let s = udp_src();
            (*s).bound = 0;
            /* A v4 socket: the any-address udp_bind. */
            let _ = a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 1, 0,
                                          ptr::null(), 0, 4);
            assert_eq!((T_BIND_V4, T_BIND_V6), (1, 0));
            /* A v6 socket: the loopback address, no udp_bind. */
            reset();
            let s = udp_src();
            (*s).bound = 0;
            (*s).domain = AF_INET6;
            let _ = a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 1, 0,
                                          ptr::null(), 0, 4);
            assert_eq!((T_BIND_V4, T_BIND_V6), (0, 1));
        }
    }

    #[test]
    fn the_connected_peer_is_used_and_a_stale_one_is_dropped() {
        unsafe {
            let _serial = serial();
            reset();
            let s = udp_src();
            let mut peer = Sock::new();
            T_PEER = &mut peer;
            (*s).connected = 1;
            (*s).peer = &mut peer;
            (*s).peer_len = 4;
            let _ = a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 1, 0,
                                          ptr::null(), 0, 4);
            assert_eq!((T_REF, T_FIND), (1, 0));
            assert_eq!(T_ENQ_LOCKED, 1);

            /* A dead peer is dropped and the table is consulted. */
            reset();
            let s = udp_src();
            let mut peer = Sock::new();
            peer.live = 0;
            T_PEER = &mut peer;
            (*s).connected = 1;
            (*s).peer = &mut peer;
            (*s).peer_len = 4;
            let _ = a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 1, 0,
                                          ptr::null(), 0, 4);
            assert!((*s).peer.is_null());
            assert_eq!(T_FIND, 1);
        }
    }

    #[test]
    fn a_full_local_queue_blocks_unless_dontwait() {
        unsafe {
            let _serial = serial();
            reset();
            let s = udp_src();
            let mut peer = Sock::new();
            peer.rx_count = 4;
            T_PEER = &mut peer;
            (*s).connected = 1;
            (*s).peer = &mut peer;
            (*s).peer_len = 4;
            let _ = a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 1, 0,
                                          ptr::null(), 0, 4);
            assert_eq!((T_ENQ_BLOCKING, T_ENQ_LOCKED), (1, 0));

            /* With MSG_DONTWAIT the immediate enqueue is used instead. */
            reset();
            let s = udp_src();
            let mut peer = Sock::new();
            peer.rx_count = 4;
            T_PEER = &mut peer;
            (*s).connected = 1;
            (*s).peer = &mut peer;
            (*s).peer_len = 4;
            let _ = a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 1,
                                          MSG_DONTWAIT, ptr::null(), 0, 4);
            assert_eq!((T_ENQ_BLOCKING, T_ENQ_LOCKED, T_COLLECT), (0, 1, 1));
        }
    }

    #[test]
    fn a_dead_local_destination_is_refused_after_the_reference() {
        unsafe {
            let _serial = serial();
            reset();
            let s = udp_src();
            let mut peer = Sock::new();
            peer.live = 0;
            T_PEER = &mut peer;
            (*s).connected = 1;
            (*s).peer = &mut peer;
            (*s).peer_len = 4;
            assert_eq!(a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 1, 0,
                                             ptr::null(), 0, 4), -ECONNREFUSED);
            assert_eq!(T_FREE, 1);
        }
    }

    #[test]
    fn a_non_initial_namespace_without_a_local_target_is_refused() {
        unsafe {
            let _serial = serial();
            reset();
            let s = udp_src();
            T_NS_INITIAL = 0;
            assert_eq!(a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 1, 0,
                                             ptr::null(), 0, 4), -EDESTADDRREQ);
            assert_eq!(T_PBUF_ALLOC, 0);
        }
    }

    #[test]
    fn the_stack_path_orders_snapshot_lane_shared_and_poll() {
        unsafe {
            let _serial = serial();
            reset();
            let s = udp_src();
            (*s).local_len = 4;
            let addr = 1u8;
            assert_eq!(a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 7, 0,
                                             &addr as *const u8 as *const c_void,
                                             16, 4), 7);
            assert_eq!(&T_TRACE[..6], &[b'A', b'H', b'O', b'S', b'F', b'P']);
            assert_eq!((T_SHARED_UNLOCK, T_LANE_UNLOCK), (1, 1));
            assert_eq!((T_PBUF_ALLOC, T_TAKE), (1, 1));
        }
    }

    #[test]
    fn the_stack_path_uses_the_connected_send_or_refuses() {
        unsafe {
            let _serial = serial();
            reset();
            let s = udp_src();
            /* Connected: udp_send. */
            (*s).connected = 1;
            T_CONNECTED_FLAG = 1;
            assert_eq!(a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 5, 0,
                                             ptr::null(), 0, 4), 5);
            assert_eq!(T_SEND_CALLS, 1);

            /* Unconnected and unaddressed: EDESTADDRREQ with the pbuf freed and
             * both locks released. */
            reset();
            let s = udp_src();
            assert_eq!(a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 5, 0,
                                             ptr::null(), 0, 4), -EDESTADDRREQ);
            assert_eq!((T_PBUF_FREE, T_SHARED_UNLOCK, T_LANE_UNLOCK), (1, 1, 1));
            assert_eq!(T_POLL, 0);
        }
    }

    #[test]
    fn the_stack_path_maps_statuses_and_conversion_errors() {
        unsafe {
            let _serial = serial();
            reset();
            let s = udp_src();
            T_SEND_RC = -1;
            (*s).connected = 1;
            T_CONNECTED_FLAG = 1;
            assert_eq!(a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 5, 0,
                                             ptr::null(), 0, 4), -EIO);

            /* An address the conversion rejects frees the pbuf and returns its
             * errno before any lock is taken. */
            reset();
            let s = udp_src();
            T_SOCKADDR_RC = -22;
            let addr = 1u8;
            assert_eq!(a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 5, 0,
                                             &addr as *const u8 as *const c_void,
                                             16, 4), -22);
            assert_eq!((T_PBUF_FREE, T_LANE_LOCK), (1, 0));

            /* A socket that lost its pcb mid-send reports ENOTSOCK. */
            reset();
            let s = udp_src();
            (*s).udp = ptr::null_mut();
            assert_eq!(a20_net_inet_send_udp(s as *mut c_void, ptr::null(), 5, 0,
                                             ptr::null(), 0, 4), -ENOTSOCK);
            assert_eq!((T_PBUF_FREE, T_LANE_UNLOCK), (1, 1));
        }
    }

    #[test]
    fn raw_refuses_a_foreign_namespace_before_anything_else() {
        unsafe {
            let _serial = serial();
            reset();
            let s = ptr::addr_of_mut!(S_SRC);
            (*s).raw = 16usize as *mut c_void;
            T_NS_INITIAL = 0;
            T_BUF_FORCE = 8;
            assert_eq!(a20_net_inet_send_raw(s as *mut c_void, ptr::null(), 9,
                                             ptr::null(), 0, 4), -ENETUNREACH);
            assert_eq!(T_PBUF_ALLOC, 0);
        }
    }

    #[test]
    fn raw_orders_the_same_way_and_refuses_without_a_destination() {
        unsafe {
            let _serial = serial();
            reset();
            let s = ptr::addr_of_mut!(S_SRC);
            (*s).raw = 16usize as *mut c_void;
            let addr = 1u8;
            assert_eq!(a20_net_inet_send_raw(s as *mut c_void, ptr::null(), 3,
                                             &addr as *const u8 as *const c_void,
                                             8, 4), 3);
            assert_eq!(&T_TRACE[..6], &[b'A', b'H', b'O', b'S', b'F', b'P']);

            /* Unconnected, unaddressed. */
            reset();
            let s = ptr::addr_of_mut!(S_SRC);
            (*s).raw = 16usize as *mut c_void;
            assert_eq!(a20_net_inet_send_raw(s as *mut c_void, ptr::null(), 3,
                                             ptr::null(), 0, 4), -EDESTADDRREQ);
            assert_eq!((T_PBUF_FREE, T_SHARED_UNLOCK, T_LANE_UNLOCK), (1, 1, 1));

            /* A lost pcb. */
            reset();
            let s = ptr::addr_of_mut!(S_SRC);
            assert_eq!(a20_net_inet_send_raw(s as *mut c_void, ptr::null(), 3,
                                             ptr::null(), 0, 4), -ENOTSOCK);
            assert_eq!((T_PBUF_FREE, T_LANE_UNLOCK), (1, 1));
        }
    }
}
