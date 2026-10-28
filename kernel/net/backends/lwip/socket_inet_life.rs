#![no_std]

//! The INET socket lifecycle: per-type pcb creation with its callback wiring,
//! the listen conversion, and the teardown.
//!
//! The C side keeps the primitives this module calls: the lwIP lock, the lane
//! primitives, the pcb constructors and destructors, the callback setters, the
//! listen call, the accept-stage purge and the pbuf free.
//!
//! Everything that is a *decision* is here: which pcb a domain/type gets, when a
//! creation failure yields `ENOMEM` and publishes no lane, that a listener is
//! torn down with `tcp_close` plus a stage purge while a connection is unbound
//! and closed gracefully (abort only as the documented fallback), and that the
//! ring's spill references are released while the lwIP lock is still held.

use core::ffi::c_void;
use core::ptr;

const AF_INET: i32 = 2;
const AF_INET6: i32 = 10;
const SOCK_DGRAM: i32 = 2;
const SOCK_RAW: i32 = 3;
const SOCK_STREAM: i32 = 1;

const ENOMEM: i32 = 12;
const EINVAL: i32 = 22;

#[repr(C)]
pub struct LifeView {
    pub s: *mut c_void,
    pub net_ns: *mut *mut c_void,
    pub domain: *mut i32,
    pub sock_type: *mut i32,
    pub protocol: *mut i32,
    pub udp: *mut *mut c_void,
    pub raw: *mut *mut c_void,
    pub tcp: *mut *mut c_void,
}

unsafe extern "C" {
    fn a20_net_inet_life_view_init(s: *mut c_void, out: *mut LifeView);
    fn a20_net_inet_life_lock(flags: *mut u64);
    fn a20_net_inet_life_unlock(flags: u64);
    fn a20_net_inet_life_ns_initial(ns: *mut c_void) -> i32;
    fn a20_net_inet_life_lane_load(s: *mut c_void) -> u32;
    fn a20_net_inet_life_lane_store(s: *mut c_void, lane: u32);
    fn a20_net_inet_life_lane_enter(lane: u32);
    fn a20_net_inet_life_pcb_owner(pcb: *mut c_void, kind: i32) -> u32;
    fn a20_net_inet_life_udp_new(v6: i32) -> *mut c_void;
    fn a20_net_inet_life_udp_cb(pcb: *mut c_void, s: *mut c_void);
    fn a20_net_inet_life_raw_new(v6: i32, protocol: i32) -> *mut c_void;
    fn a20_net_inet_life_raw_cb(pcb: *mut c_void, s: *mut c_void);
    fn a20_net_inet_life_tcp_new(v6: i32) -> *mut c_void;
    fn a20_net_inet_life_tcp_options(s: *mut c_void, pcb: *mut c_void);
    fn a20_net_inet_life_tcp_listen(pcb: *mut c_void, backlog: i32) -> *mut c_void;
    fn a20_net_inet_life_tcp_arg(pcb: *mut c_void, s: *mut c_void);
    fn a20_net_inet_life_tcp_accept(pcb: *mut c_void);
    fn a20_net_inet_life_pcb_is_listen(pcb: *mut c_void) -> i32;
    fn a20_net_inet_life_tcp_close(pcb: *mut c_void) -> i32;
    fn a20_net_inet_life_tcp_abort(pcb: *mut c_void);
    fn a20_net_inet_life_tcp_unbind(pcb: *mut c_void);
    fn a20_net_inet_life_accept_purge(s: *mut c_void);
    fn a20_net_inet_life_udp_remove(pcb: *mut c_void);
    fn a20_net_inet_life_raw_remove(pcb: *mut c_void);
    fn a20_net_inet_life_spill_count() -> i32;
    fn a20_net_inet_life_spill_take(s: *mut c_void, i: i32) -> *mut c_void;
    fn a20_net_inet_life_pbuf_free(p: *mut c_void);
}

fn zero_view() -> LifeView {
    LifeView {
        s: ptr::null_mut(),
        net_ns: ptr::null_mut(),
        domain: ptr::null_mut(),
        sock_type: ptr::null_mut(),
        protocol: ptr::null_mut(),
        udp: ptr::null_mut(),
        raw: ptr::null_mut(),
        tcp: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> LifeView {
    let mut v = zero_view();
    a20_net_inet_life_view_init(s, &mut v);
    v
}

/// `tcp_listen_with_backlog` returns a new pcb and consumes the bound one; the
/// socket takes the new one and wires the accept callback.
#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_tcp_listen(s: *mut c_void, backlog: i32) -> i32 {
    if s.is_null() {
        return -EINVAL;
    }
    let v = view_of(s);
    let mut flags = 0u64;
    a20_net_inet_life_lock(&mut flags);
    let pcb = *v.tcp;
    if pcb.is_null() {
        a20_net_inet_life_unlock(flags);
        return -EINVAL;
    }
    a20_net_inet_life_lane_enter(a20_net_inet_life_lane_load(s));
    let lpcb = a20_net_inet_life_tcp_listen(pcb, backlog);
    if lpcb.is_null() {
        a20_net_inet_life_unlock(flags);
        return -ENOMEM;
    }
    *v.tcp = lpcb;
    a20_net_inet_life_tcp_arg(lpcb, s);
    a20_net_inet_life_tcp_accept(lpcb);
    a20_net_inet_life_unlock(flags);
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_socket_init(s: *mut c_void) -> i32 {
    if s.is_null() {
        return 0;
    }
    let v = view_of(s);
    let domain = *v.domain;
    if domain != AF_INET && domain != AF_INET6 {
        return 0;
    }
    if a20_net_inet_life_ns_initial(*v.net_ns) == 0 {
        return 0;
    }
    let sock_type = *v.sock_type;
    let v6 = domain == AF_INET6;

    let mut flags = 0u64;
    a20_net_inet_life_lock(&mut flags);
    a20_net_inet_life_lane_enter(a20_net_inet_life_lane_load(s));
    let mut ret = 0;
    if sock_type == SOCK_DGRAM {
        let pcb = a20_net_inet_life_udp_new(if v6 { 1 } else { 0 });
        if pcb.is_null() {
            ret = -ENOMEM;
        } else {
            *v.udp = pcb;
            a20_net_inet_life_udp_cb(pcb, s);
        }
    } else if sock_type == SOCK_RAW {
        let pcb = a20_net_inet_life_raw_new(if v6 { 1 } else { 0 }, *v.protocol);
        if pcb.is_null() {
            ret = -ENOMEM;
        } else {
            *v.raw = pcb;
            a20_net_inet_life_raw_cb(pcb, s);
        }
    } else if sock_type == SOCK_STREAM {
        /* Dual-stack: the pcb takes its family from the bind/connect address. */
        let pcb = a20_net_inet_life_tcp_new(if v6 { 1 } else { 0 });
        if pcb.is_null() {
            ret = -ENOMEM;
        } else {
            *v.tcp = pcb;
            a20_net_inet_life_tcp_options(s, pcb);
        }
    }
    /* A fresh unbound pcb belongs to lwIP's wildcard/port-zero bucket, which may
     * not match the CPU-local provisional lane.  Publish its real owner before
     * releasing the all-lane control barrier so later lane retries converge. */
    if ret == 0 {
        let tcp = *v.tcp;
        let udp = *v.udp;
        if !tcp.is_null() {
            a20_net_inet_life_lane_store(s, a20_net_inet_life_pcb_owner(tcp, 1));
        } else if !udp.is_null() {
            a20_net_inet_life_lane_store(s, a20_net_inet_life_pcb_owner(udp, 0));
        }
    }
    a20_net_inet_life_unlock(flags);
    ret
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_socket_destroy(s: *mut c_void) {
    if s.is_null() {
        return;
    }
    let v = view_of(s);
    let mut flags = 0u64;
    a20_net_inet_life_lock(&mut flags);
    a20_net_inet_life_lane_enter(a20_net_inet_life_lane_load(s));
    let udp = *v.udp;
    if !udp.is_null() {
        a20_net_inet_life_udp_remove(udp);
        *v.udp = ptr::null_mut();
    }
    let raw = *v.raw;
    if !raw.is_null() {
        a20_net_inet_life_raw_remove(raw);
        *v.raw = ptr::null_mut();
    }
    let tcp = *v.tcp;
    if !tcp.is_null() {
        if a20_net_inet_life_pcb_is_listen(tcp) != 0 {
            /* `tcp_abort`/`tcp_abandon` assert on a LISTEN pcb; `tcp_close` is
             * the only teardown lwIP accepts, and it reaps the children the
             * listener still owns.  The connections parked in the accept stage
             * are no lwIP list's to reap, so the stage is purged too. */
            a20_net_inet_life_tcp_close(tcp);
            a20_net_inet_life_accept_purge(s);
        } else {
            /* Graceful close, not abort: close() has to flush what write()
             * already handed over and send a FIN.  An RST would make the peer
             * throw away data it has received, so a request/response server's
             * `write(); close()` would lose the tail of every reply.  The
             * callbacks are unbound first so the teardown cannot re-enter the
             * socket layer; abort is the documented fallback for the one case
             * `tcp_close` cannot take the pcb. */
            a20_net_inet_life_tcp_unbind(tcp);
            if a20_net_inet_life_tcp_close(tcp) < 0 {
                a20_net_inet_life_tcp_abort(tcp);
            }
        }
        *v.tcp = ptr::null_mut();
    }
    /* Release any spill reference the ring still holds; this runs under the
     * lwIP lock, the only context where memp may be touched, and it has to
     * happen here because the ring dies with the socket. */
    let n = a20_net_inet_life_spill_count();
    let mut i = 0;
    while i < n {
        let p = a20_net_inet_life_spill_take(s, i);
        if !p.is_null() {
            a20_net_inet_life_pbuf_free(p);
        }
        i += 1;
    }
    a20_net_inet_life_unlock(flags);
}

#[cfg(test)]
#[allow(static_mut_refs)]
mod tests {
    use super::*;

    #[repr(C)]
    struct Sock {
        domain: i32,
        sock_type: i32,
        protocol: i32,
        udp: *mut c_void,
        raw: *mut c_void,
        tcp: *mut c_void,
        lane: u32,
        ns: *mut c_void,
    }

    impl Sock {
        const fn new() -> Sock {
            Sock {
                domain: AF_INET,
                sock_type: SOCK_DGRAM,
                protocol: 0,
                udp: ptr::null_mut(),
                raw: ptr::null_mut(),
                tcp: ptr::null_mut(),
                lane: 1,
                ns: 1usize as *mut c_void,
            }
        }
    }

    const PCB_NEW: *mut c_void = 0x1000 as *mut c_void;
    const PCB_LISTEN: *mut c_void = 0x2000 as *mut c_void;

    static mut S_SOCK: Sock = Sock::new();
    static mut T_NS_INITIAL: i32 = 1;
    static mut T_NEW_NULL: i32 = 0;
    static mut T_NEW_CALLS: [i32; 3] = [0; 3];
    static mut T_CB_CALLS: [i32; 3] = [0; 3];
    static mut T_OPTIONS: i32 = 0;
    static mut T_LANE_STORED: u32 = 99;
    static mut T_LANE_ENTER: u32 = 99;
    static mut T_OWNER: [u32; 2] = [5, 6];
    static mut T_LISTEN_CALLS: i32 = 0;
    static mut T_LISTEN_NULL: i32 = 0;
    static mut T_LISTEN_PCB: *mut c_void = ptr::null_mut();
    static mut T_ARG_CALLS: i32 = 0;
    static mut T_ACCEPT_CALLS: i32 = 0;
    static mut T_IS_LISTEN: i32 = 0;
    static mut T_CLOSE_RC: i32 = 0;
    static mut T_CLOSE_CALLS: i32 = 0;
    static mut T_ABORT_CALLS: i32 = 0;
    static mut T_UNBIND_CALLS: i32 = 0;
    static mut T_PURGE_CALLS: i32 = 0;
    static mut T_UDP_REMOVE: i32 = 0;
    static mut T_RAW_REMOVE: i32 = 0;
    static mut T_SPILLS: [*mut c_void; 2] = [ptr::null_mut(); 2];
    static mut T_SPILL_COUNT: i32 = 2;
    static mut T_FREED: i32 = 0;
    static mut T_TRACE: [u8; 16] = [0; 16];
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
        S_SOCK = Sock::new();
        T_NS_INITIAL = 1;
        T_NEW_NULL = 0;
        T_NEW_CALLS = [0; 3];
        T_CB_CALLS = [0; 3];
        T_OPTIONS = 0;
        T_LANE_STORED = 99;
        T_LANE_ENTER = 99;
        T_OWNER = [5, 6];
        T_LISTEN_CALLS = 0;
        T_LISTEN_NULL = 0;
        T_LISTEN_PCB = PCB_LISTEN;
        T_ARG_CALLS = 0;
        T_ACCEPT_CALLS = 0;
        T_IS_LISTEN = 0;
        T_CLOSE_RC = 0;
        T_CLOSE_CALLS = 0;
        T_ABORT_CALLS = 0;
        T_UNBIND_CALLS = 0;
        T_PURGE_CALLS = 0;
        T_UDP_REMOVE = 0;
        T_RAW_REMOVE = 0;
        T_SPILLS = [0x3000 as *mut c_void, 0x4000 as *mut c_void];
        T_SPILL_COUNT = 2;
        T_FREED = 0;
        T_TRACE = [0; 16];
        T_TRACE_N = 0;
    }

    unsafe fn sock() -> *mut c_void {
        ptr::addr_of_mut!(S_SOCK) as *mut c_void
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_view_init(s: *mut c_void,
                                                     out: *mut LifeView) {
        let k = &mut *(s as *mut Sock);
        *out = LifeView {
            s,
            net_ns: &mut k.ns,
            domain: &mut k.domain,
            sock_type: &mut k.sock_type,
            protocol: &mut k.protocol,
            udp: &mut k.udp,
            raw: &mut k.raw,
            tcp: &mut k.tcp,
        };
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_lock(_f: *mut u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_unlock(_f: u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_ns_initial(_ns: *mut c_void) -> i32 {
        T_NS_INITIAL
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_lane_load(s: *mut c_void) -> u32 {
        (*(s as *const Sock)).lane
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_lane_store(_s: *mut c_void, lane: u32) {
        T_LANE_STORED = lane;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_lane_enter(lane: u32) {
        T_LANE_ENTER = lane;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_pcb_owner(_p: *mut c_void,
                                                     kind: i32) -> u32 {
        T_OWNER[kind as usize]
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_udp_new(v6: i32) -> *mut c_void {
        assert!(v6 == 0 || v6 == 1);
        T_NEW_CALLS[0] += 1;
        trace(b'U');
        if T_NEW_NULL != 0 { ptr::null_mut() } else { PCB_NEW }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_udp_cb(_p: *mut c_void, _s: *mut c_void) {
        T_CB_CALLS[0] += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_raw_new(v6: i32, proto: i32) -> *mut c_void {
        assert!(v6 == 0 || v6 == 1);
        assert_eq!(proto, 17);
        T_NEW_CALLS[1] += 1;
        trace(b'R');
        if T_NEW_NULL != 0 { ptr::null_mut() } else { PCB_NEW }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_raw_cb(_p: *mut c_void, _s: *mut c_void) {
        T_CB_CALLS[1] += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_tcp_new(v6: i32) -> *mut c_void {
        assert!(v6 == 0 || v6 == 1);
        T_NEW_CALLS[2] += 1;
        trace(b'T');
        if T_NEW_NULL != 0 { ptr::null_mut() } else { PCB_NEW }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_tcp_options(_s: *mut c_void,
                                                       _p: *mut c_void) {
        T_OPTIONS += 1;
        T_CB_CALLS[2] += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_tcp_listen(_p: *mut c_void,
                                                      _b: i32) -> *mut c_void {
        T_LISTEN_CALLS += 1;
        if T_LISTEN_NULL != 0 { ptr::null_mut() } else { T_LISTEN_PCB }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_tcp_arg(_p: *mut c_void, _s: *mut c_void) {
        T_ARG_CALLS += 1;
        trace(b'A');
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_tcp_accept(_p: *mut c_void) {
        T_ACCEPT_CALLS += 1;
        trace(b'C');
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_pcb_is_listen(_p: *mut c_void) -> i32 {
        T_IS_LISTEN
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_tcp_close(_p: *mut c_void) -> i32 {
        T_CLOSE_CALLS += 1;
        trace(b'L');
        T_CLOSE_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_tcp_abort(_p: *mut c_void) {
        T_ABORT_CALLS += 1;
        trace(b'B');
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_tcp_unbind(_p: *mut c_void) {
        T_UNBIND_CALLS += 1;
        trace(b'N');
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_accept_purge(_s: *mut c_void) {
        T_PURGE_CALLS += 1;
        trace(b'P');
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_udp_remove(_p: *mut c_void) {
        T_UDP_REMOVE += 1;
        trace(b'u');
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_raw_remove(_p: *mut c_void) {
        T_RAW_REMOVE += 1;
        trace(b'r');
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_spill_count() -> i32 {
        T_SPILL_COUNT
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_spill_take(_s: *mut c_void,
                                                      i: i32) -> *mut c_void {
        let p = T_SPILLS[i as usize];
        T_SPILLS[i as usize] = ptr::null_mut();
        p
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_life_pbuf_free(_p: *mut c_void) {
        T_FREED += 1;
    }

    #[test]
    fn init_creates_the_pcb_per_type_and_publishes_its_lane() {
        unsafe {
            let _serial = serial();
            reset();
            assert_eq!(a20_net_inet_socket_init(sock()), 0);
            assert_eq!(T_NEW_CALLS, [1, 0, 0]);
            assert_eq!(T_CB_CALLS, [1, 0, 0]);
            assert_eq!((*sock().cast::<Sock>()).udp, PCB_NEW);
            assert_eq!((T_LANE_ENTER, T_LANE_STORED), (1, 5));

            /* Raw v4 passes the protocol through. */
            reset();
            (*sock().cast::<Sock>()).sock_type = SOCK_RAW;
            (*sock().cast::<Sock>()).protocol = 17;
            assert_eq!(a20_net_inet_socket_init(sock()), 0);
            assert_eq!(T_NEW_CALLS, [0, 1, 0]);
            /* A raw pcb owns no lane at creation; bind stores its address lane. */
            assert_eq!(T_LANE_STORED, 99);

            /* Stream applies the socket's options to the new pcb. */
            reset();
            (*sock().cast::<Sock>()).sock_type = SOCK_STREAM;
            assert_eq!(a20_net_inet_socket_init(sock()), 0);
            assert_eq!(T_NEW_CALLS, [0, 0, 1]);
            assert_eq!(T_OPTIONS, 1);
            assert_eq!(T_LANE_STORED, 6);
        }
    }

    #[test]
    fn init_failures_report_enomem_and_publish_no_lane() {
        unsafe {
            let _serial = serial();
            reset();
            T_NEW_NULL = 1;
            assert_eq!(a20_net_inet_socket_init(sock()), -ENOMEM);
            assert_eq!(T_LANE_STORED, 99);
            assert_eq!((*sock().cast::<Sock>()).udp, ptr::null_mut());
        }
    }

    #[test]
    fn init_leaves_other_families_and_namespaces_alone() {
        unsafe {
            let _serial = serial();
            reset();
            (*sock().cast::<Sock>()).domain = 16;
            assert_eq!(a20_net_inet_socket_init(sock()), 0);
            reset();
            T_NS_INITIAL = 0;
            assert_eq!(a20_net_inet_socket_init(sock()), 0);
            reset();
            assert_eq!(a20_net_inet_socket_init(ptr::null_mut()), 0);
            assert_eq!((T_NEW_CALLS, T_LANE_ENTER), ([0, 0, 0], 99));
        }
    }

    #[test]
    fn listen_converts_and_rewires_the_pcb() {
        unsafe {
            let _serial = serial();
            reset();
            (*sock().cast::<Sock>()).tcp = PCB_NEW;
            assert_eq!(a20_net_inet_tcp_listen(sock(), 4), 0);
            assert_eq!((*sock().cast::<Sock>()).tcp, PCB_LISTEN);
            assert_eq!((T_ARG_CALLS, T_ACCEPT_CALLS), (1, 1));
            assert_eq!(&T_TRACE[..2], &[b'A', b'C']);

            /* No pcb. */
            reset();
            assert_eq!(a20_net_inet_tcp_listen(sock(), 4), -EINVAL);
            /* Allocation failure. */
            reset();
            (*sock().cast::<Sock>()).tcp = PCB_NEW;
            T_LISTEN_NULL = 1;
            assert_eq!(a20_net_inet_tcp_listen(sock(), 4), -ENOMEM);
            assert_eq!((*sock().cast::<Sock>()).tcp, PCB_NEW);
            assert_eq!(T_ARG_CALLS, 0);
        }
    }

    #[test]
    fn destroy_removes_udp_and_raw_and_nulls_them() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).udp = PCB_NEW;
            (*s).raw = PCB_NEW;
            a20_net_inet_socket_destroy(sock());
            assert_eq!((T_UDP_REMOVE, T_RAW_REMOVE), (1, 1));
            assert_eq!(((*s).udp, (*s).raw), (ptr::null_mut(), ptr::null_mut()));
            assert_eq!(T_FREED, 2); /* the two spill references */
        }
    }

    #[test]
    fn a_listener_closes_and_purges_its_stage() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).tcp = PCB_NEW;
            T_IS_LISTEN = 1;
            a20_net_inet_socket_destroy(sock());
            assert_eq!((T_CLOSE_CALLS, T_PURGE_CALLS, T_ABORT_CALLS), (1, 1, 0));
            assert_eq!(&T_TRACE[..2], &[b'L', b'P']);
            assert_eq!((*s).tcp, ptr::null_mut());
        }
    }

    #[test]
    fn a_connection_is_unbound_and_closed_gracefully() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).tcp = PCB_NEW;
            a20_net_inet_socket_destroy(sock());
            assert_eq!((T_UNBIND_CALLS, T_CLOSE_CALLS, T_ABORT_CALLS), (1, 1, 0));
            assert_eq!(&T_TRACE[..2], &[b'N', b'L']);

            /* The documented fallback when tcp_close cannot take the pcb. */
            reset();
            let s = sock().cast::<Sock>();
            (*s).tcp = PCB_NEW;
            T_CLOSE_RC = -1;
            a20_net_inet_socket_destroy(sock());
            assert_eq!((T_CLOSE_CALLS, T_ABORT_CALLS), (1, 1));
            assert_eq!(T_UNBIND_CALLS, 1);
        }
    }

    #[test]
    fn the_spill_loop_frees_only_owned_slots() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).udp = PCB_NEW;
            T_SPILLS[1] = ptr::null_mut();
            a20_net_inet_socket_destroy(sock());
            assert_eq!(T_FREED, 1);
        }
    }
}
