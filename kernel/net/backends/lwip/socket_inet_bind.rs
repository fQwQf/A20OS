#![no_std]

//! The INET bind transaction: per-type dispatch, the address conversion, the
//! pcb-present check, the lane entry and the lane-store decision on success,
//! and the status mapping.
//!
//! The C side keeps the primitives this module calls: the lwIP lock, the lane
//! load/store/enter and the pcb-owner and address hashes, the three lwIP bind
//! calls, and the sockaddr-to-ip_addr_t conversion.

use core::ffi::c_void;
use core::ptr;

const AF_INET: i32 = 2;
const AF_INET6: i32 = 10;
const SOCK_DGRAM: i32 = 2;
const SOCK_RAW: i32 = 3;
const SOCK_STREAM: i32 = 1;

const ENOTSOCK: i32 = 88;
const EADDRINUSE: i32 = 98;

/// `ip_addr_t`, opaque here: C fills it and hands it to the bind call.
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

#[repr(C)]
pub struct BindView {
    pub s: *mut c_void,
    pub net_ns: *mut *mut c_void,
    pub domain: *mut i32,
    pub sock_type: *mut i32,
    pub udp: *mut *mut c_void,
    pub raw: *mut *mut c_void,
    pub tcp: *mut *mut c_void,
}

unsafe extern "C" {
    fn a20_net_inet_bind_view_init(s: *mut c_void, out: *mut BindView);
    fn a20_net_inet_bind_lock(flags: *mut u64);
    fn a20_net_inet_bind_unlock(flags: u64);
    fn a20_net_inet_bind_ns_initial(ns: *mut c_void) -> i32;
    fn a20_net_inet_bind_sockaddr_to_ip(addr: *const c_void, len: usize,
                                        ip: *mut c_void, port: *mut u16) -> i32;
    fn a20_net_inet_bind_lane_load(s: *mut c_void) -> u32;
    fn a20_net_inet_bind_lane_store(s: *mut c_void, lane: u32);
    fn a20_net_inet_bind_lane_enter(lane: u32);
    fn a20_net_inet_bind_pcb_owner(pcb: *mut c_void, kind: i32) -> u32;
    fn a20_net_inet_bind_lane_of_addr(addr: *const c_void, len: usize,
                                      fallback: u32) -> u32;
    fn a20_net_inet_bind_udp(pcb: *mut c_void, ip: *const c_void, port: u16) -> i32;
    fn a20_net_inet_bind_raw(pcb: *mut c_void, ip: *const c_void) -> i32;
    fn a20_net_inet_bind_tcp(pcb: *mut c_void, ip: *const c_void, port: u16) -> i32;
}

fn zero_view() -> BindView {
    BindView {
        s: ptr::null_mut(),
        net_ns: ptr::null_mut(),
        domain: ptr::null_mut(),
        sock_type: ptr::null_mut(),
        udp: ptr::null_mut(),
        raw: ptr::null_mut(),
        tcp: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> BindView {
    let mut v = zero_view();
    a20_net_inet_bind_view_init(s, &mut v);
    v
}

/// Bind the socket's lwIP pcb.  Returns 0 for "nothing to do" on a socket this
/// path does not own (another family, a foreign namespace, another type), the
/// conversion error, or `EADDRINUSE` when the stack refuses the address.
#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_bind_pcb(s: *mut c_void, addr: *const c_void,
                                               addrlen: usize) -> i32 {
    if s.is_null() {
        return 0;
    }
    let v = view_of(s);
    let domain = *v.domain;
    if domain != AF_INET && domain != AF_INET6 {
        return 0;
    }
    if a20_net_inet_bind_ns_initial(*v.net_ns) == 0 {
        return 0;
    }
    let sock_type = *v.sock_type;
    let mut ip = IpBuf::new();
    if sock_type == SOCK_DGRAM || sock_type == SOCK_STREAM {
        let mut port: u16 = 0;
        let r = a20_net_inet_bind_sockaddr_to_ip(addr, addrlen, ip.as_mut_ptr(),
                                                 &mut port);
        if r < 0 {
            return r;
        }
        let mut flags = 0u64;
        a20_net_inet_bind_lock(&mut flags);
        let pcb = if sock_type == SOCK_DGRAM { *v.udp } else { *v.tcp };
        if pcb.is_null() {
            a20_net_inet_bind_unlock(flags);
            return -ENOTSOCK;
        }
        a20_net_inet_bind_lane_enter(a20_net_inet_bind_lane_load(s));
        let e = if sock_type == SOCK_DGRAM {
            a20_net_inet_bind_udp(pcb, ip.as_mut_ptr(), port)
        } else {
            a20_net_inet_bind_tcp(pcb, ip.as_mut_ptr(), port)
        };
        if e == 0 {
            let kind = if sock_type == SOCK_DGRAM { 0 } else { 1 };
            a20_net_inet_bind_lane_store(s,
                a20_net_inet_bind_pcb_owner(pcb, kind));
        }
        a20_net_inet_bind_unlock(flags);
        return if e == 0 { 0 } else { -EADDRINUSE };
    }
    if sock_type == SOCK_RAW {
        let r = a20_net_inet_bind_sockaddr_to_ip(addr, addrlen, ip.as_mut_ptr(),
                                                 ptr::null_mut());
        if r < 0 {
            return r;
        }
        let mut flags = 0u64;
        a20_net_inet_bind_lock(&mut flags);
        let pcb = *v.raw;
        if pcb.is_null() {
            a20_net_inet_bind_unlock(flags);
            return -ENOTSOCK;
        }
        let lane = a20_net_inet_bind_lane_load(s);
        a20_net_inet_bind_lane_enter(lane);
        let e = a20_net_inet_bind_raw(pcb, ip.as_mut_ptr());
        if e == 0 {
            /* A raw pcb is hashed by the address it was bound to. */
            a20_net_inet_bind_lane_store(
                s, a20_net_inet_bind_lane_of_addr(addr, addrlen, lane));
        }
        a20_net_inet_bind_unlock(flags);
        return if e == 0 { 0 } else { -EADDRINUSE };
    }
    0
}

#[cfg(test)]
#[allow(static_mut_refs)]
mod tests {
    use super::*;

    #[repr(C)]
    struct Sock {
        domain: i32,
        sock_type: i32,
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
                udp: 8usize as *mut c_void,
                raw: ptr::null_mut(),
                tcp: ptr::null_mut(),
                lane: 1,
                ns: 1usize as *mut c_void,
            }
        }
    }

    static mut S_SOCK: Sock = Sock::new();
    static mut T_NS_INITIAL: i32 = 1;
    static mut T_SOCKADDR_RC: i32 = 0;
    static mut T_BIND_RC: i32 = 0;
    static mut T_BIND_CALLS: [i32; 3] = [0; 3];
    static mut T_LANE_ENTER: u32 = 99;
    static mut T_LANE_STORED: u32 = 99;
    static mut T_LANE_OF_ADDR: u32 = 7;
    static mut T_PCB_OWNER: u32 = 5;
    static mut T_LOCK: i32 = 0;

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
        T_NS_INITIAL = 1;
        T_SOCKADDR_RC = 0;
        T_BIND_RC = 0;
        T_BIND_CALLS = [0; 3];
        T_LANE_ENTER = 99;
        T_LANE_STORED = 99;
        T_LANE_OF_ADDR = 7;
        T_PCB_OWNER = 5;
        T_LOCK = 0;
    }

    unsafe fn sock() -> *mut c_void {
        ptr::addr_of_mut!(S_SOCK) as *mut c_void
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_bind_view_init(s: *mut c_void,
                                                     out: *mut BindView) {
        let k = &mut *(s as *mut Sock);
        *out = BindView {
            s,
            net_ns: &mut k.ns,
            domain: &mut k.domain,
            sock_type: &mut k.sock_type,
            udp: &mut k.udp,
            raw: &mut k.raw,
            tcp: &mut k.tcp,
        };
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_bind_lock(_f: *mut u64) {
        T_LOCK += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_bind_unlock(_f: u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_bind_ns_initial(_ns: *mut c_void) -> i32 {
        T_NS_INITIAL
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_bind_sockaddr_to_ip(_a: *const c_void,
                                                          _l: usize,
                                                          _ip: *mut c_void,
                                                          _p: *mut u16) -> i32 {
        T_SOCKADDR_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_bind_lane_load(_s: *mut c_void) -> u32 {
        (*(_s as *const Sock)).lane
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_bind_lane_store(_s: *mut c_void, lane: u32) {
        T_LANE_STORED = lane;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_bind_lane_enter(lane: u32) {
        T_LANE_ENTER = lane;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_bind_pcb_owner(_p: *mut c_void, kind: i32) -> u32 {
        assert!(kind == 0 || kind == 1);
        T_PCB_OWNER
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_bind_lane_of_addr(_a: *const c_void,
                                                        _l: usize,
                                                        _f: u32) -> u32 {
        T_LANE_OF_ADDR
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_bind_udp(_p: *mut c_void, _i: *const c_void,
                                               _port: u16) -> i32 {
        T_BIND_CALLS[0] += 1;
        T_BIND_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_bind_raw(_p: *mut c_void,
                                               _i: *const c_void) -> i32 {
        T_BIND_CALLS[1] += 1;
        T_BIND_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_bind_tcp(_p: *mut c_void, _i: *const c_void,
                                               _port: u16) -> i32 {
        T_BIND_CALLS[2] += 1;
        T_BIND_RC
    }

    #[test]
    fn a_udp_bind_enters_the_lane_and_stores_the_pcb_owner() {
        unsafe {
            let _serial = serial();
            reset();
            assert_eq!(a20_net_inet_bind_pcb(sock(), ptr::null(), 16), 0);
            assert_eq!(T_BIND_CALLS, [1, 0, 0]);
            assert_eq!((T_LANE_ENTER, T_LANE_STORED), (1, 5));
            assert_eq!(T_LOCK, 1);
        }
    }

    #[test]
    fn a_raw_bind_stores_the_address_lane() {
        unsafe {
            let _serial = serial();
            reset();
            (*sock().cast::<Sock>()).sock_type = SOCK_RAW;
            (*sock().cast::<Sock>()).raw = 9usize as *mut c_void;
            assert_eq!(a20_net_inet_bind_pcb(sock(), ptr::null(), 8), 0);
            assert_eq!(T_BIND_CALLS, [0, 1, 0]);
            /* The raw pcb is hashed by the bound address, not by the pcb. */
            assert_eq!(T_LANE_STORED, 7);
            assert_eq!(T_LANE_ENTER, 1);
        }
    }

    #[test]
    fn a_tcp_bind_stores_the_pcb_owner() {
        unsafe {
            let _serial = serial();
            reset();
            (*sock().cast::<Sock>()).sock_type = SOCK_STREAM;
            (*sock().cast::<Sock>()).tcp = 10usize as *mut c_void;
            assert_eq!(a20_net_inet_bind_pcb(sock(), ptr::null(), 16), 0);
            assert_eq!(T_BIND_CALLS, [0, 0, 1]);
            assert_eq!(T_LANE_STORED, 5);
        }
    }

    #[test]
    fn a_refused_bind_keeps_the_lane_and_maps_to_addr_in_use() {
        unsafe {
            let _serial = serial();
            reset();
            T_BIND_RC = -8; /* ERR_USE */
            assert_eq!(a20_net_inet_bind_pcb(sock(), ptr::null(), 16), -EADDRINUSE);
            assert_eq!(T_LANE_STORED, 99);
            assert_eq!(T_LANE_ENTER, 1);
        }
    }

    #[test]
    fn a_missing_pcb_is_refused_under_the_lock() {
        unsafe {
            let _serial = serial();
            reset();
            (*sock().cast::<Sock>()).udp = ptr::null_mut();
            assert_eq!(a20_net_inet_bind_pcb(sock(), ptr::null(), 16), -ENOTSOCK);
            assert_eq!(T_BIND_CALLS, [0, 0, 0]);
            assert_eq!(T_LOCK, 1);
        }
    }

    #[test]
    fn a_conversion_error_returns_before_any_lock() {
        unsafe {
            let _serial = serial();
            reset();
            T_SOCKADDR_RC = -97;
            assert_eq!(a20_net_inet_bind_pcb(sock(), ptr::null(), 16), -97);
            assert_eq!(T_LOCK, 0);
            assert_eq!(T_BIND_CALLS, [0, 0, 0]);
        }
    }

    #[test]
    fn sockets_outside_this_path_are_left_alone() {
        unsafe {
            let _serial = serial();
            reset();
            /* Another family. */
            (*sock().cast::<Sock>()).domain = 16;
            assert_eq!(a20_net_inet_bind_pcb(sock(), ptr::null(), 16), 0);
            /* A foreign namespace. */
            reset();
            T_NS_INITIAL = 0;
            assert_eq!(a20_net_inet_bind_pcb(sock(), ptr::null(), 16), 0);
            /* Another type. */
            reset();
            (*sock().cast::<Sock>()).sock_type = 5;
            assert_eq!(a20_net_inet_bind_pcb(sock(), ptr::null(), 16), 0);
            assert_eq!((T_LOCK, T_BIND_CALLS), (0, [0, 0, 0]));
            /* A null socket. */
            assert_eq!(a20_net_inet_bind_pcb(ptr::null_mut(), ptr::null(), 16), 0);
        }
    }

    #[test]
    fn ipv6_takes_the_same_path() {
        unsafe {
            let _serial = serial();
            reset();
            (*sock().cast::<Sock>()).domain = AF_INET6;
            assert_eq!(a20_net_inet_bind_pcb(sock(), ptr::null(), 28), 0);
            assert_eq!(T_BIND_CALLS, [1, 0, 0]);
        }
    }
}
