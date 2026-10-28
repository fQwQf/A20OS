#![no_std]

//! The local TCP connect pairing transaction: the shortcut that pairs a
//! connecting stream socket with a listening one through the accept queue
//! instead of the stack.
//!
//! The C side keeps the primitives this module calls: the socket lock and the
//! address-ordered pair, the ephemeral-port bind, the allocator, the listener
//! search, the namespace reference and the initial-namespace test, the
//! configured TCP path, the local-target test, the registry register/unregister,
//! the accept-queue push, the accept wakeup, the lwIP pcb drop and the trace.
//!
//! Everything that is a *decision* is here: whether the ephemeral bind is
//! needed, whether the local shortcut applies (a non-initial namespace always
//! takes it; the initial one only when the fast path is configured and the
//! target is local), whether the listener's gate is open, what the child
//! inherits, that the child is registered *before* it is pushed, the pair wiring
//! on success, and the rollback -- the registry's reference and the creator's --
//! when the push fails.
//!
//! The stack path is not here.  It ends in a loop that parks the task until the
//! handshake completes, and scheduling is not this module's to own.

use core::ffi::c_void;
use core::ptr;

const SOCK_STREAM: i32 = 1;

const ENOMEM: i32 = 12;
const ENOTSOCK: i32 = 88;
const ECONNREFUSED: i32 = 111;

/// Borrowed view of the fields the transaction reads and writes.
#[repr(C)]
pub struct ConnectView {
    pub s: *mut c_void,
    pub net_ns: *mut *mut c_void,
    pub domain: *mut i32,
    pub sock_type: *mut i32,
    pub protocol: *mut i32,
    pub bound: *mut i32,
    pub connected: *mut i32,
    pub ever_connected: *mut i32,
    pub local_tcp: *mut i32,
    pub closed: *mut i32,
    pub listening: *mut i32,
    pub accept_count: *mut i32,
    pub local: *mut u8,
    pub local_len: *mut usize,
    pub peer_addr: *mut u8,
    pub peer_len: *mut usize,
    pub peer: *mut *mut c_void,
}

#[repr(C)]
pub struct ConnectPair {
    pub lo: *mut c_void,
    pub hi: *mut c_void,
    pub flags: u64,
}

impl ConnectPair {
    fn zero() -> ConnectPair {
        ConnectPair { lo: ptr::null_mut(), hi: ptr::null_mut(), flags: 0 }
    }
}

unsafe extern "C" {
    fn a20_net_inet_connect_view_init(s: *mut c_void, out: *mut ConnectView);
    fn a20_net_inet_connect_lock(s: *mut c_void, flags: *mut u64);
    fn a20_net_inet_connect_unlock(s: *mut c_void, flags: u64);
    fn a20_net_inet_connect_lock2(a: *mut c_void, b: *mut c_void,
                                  pair: *mut ConnectPair);
    fn a20_net_inet_connect_unlock2(pair: *mut ConnectPair);
    fn a20_net_inet_connect_is_live(s: *mut c_void) -> i32;
    fn a20_net_inet_connect_bind_ephemeral(s: *mut c_void);
    fn a20_net_inet_connect_alloc() -> *mut c_void;
    fn a20_net_inet_connect_free(s: *mut c_void);
    fn a20_net_inet_connect_sockaddr_port(addr: *const c_void, len: usize,
                                          port: *mut u16) -> i32;
    fn a20_net_inet_connect_ns_initial(ns: *mut c_void) -> i32;
    fn a20_net_inet_connect_tcp_path_fast() -> i32;
    fn a20_net_inet_connect_is_local_target(addr: *const c_void, len: usize) -> i32;
    fn a20_net_inet_connect_find_listener(s: *mut c_void, port: u16) -> *mut c_void;
    fn a20_net_inet_connect_ns_put(ns: *mut c_void);
    fn a20_net_inet_connect_ns_get(ns: *mut c_void) -> *mut c_void;
    fn a20_net_inet_connect_register(child: *mut c_void) -> i32;
    fn a20_net_inet_connect_unregister(child: *mut c_void) -> i32;
    fn a20_net_inet_connect_queue_push(listener: *mut c_void, child: *mut c_void) -> i32;
    fn a20_net_inet_connect_wake_accept(listener: *mut c_void, wake_q: *mut c_void);
    fn a20_net_inet_connect_wakeq_init(wake_q: *mut c_void);
    fn a20_net_inet_connect_wakeq_flush(wake_q: *mut c_void);
    fn a20_net_inet_connect_drop_pcb(s: *mut c_void);
    fn a20_net_inet_connect_trace(which: i32);
}

fn zero_view() -> ConnectView {
    ConnectView {
        s: ptr::null_mut(),
        net_ns: ptr::null_mut(),
        domain: ptr::null_mut(),
        sock_type: ptr::null_mut(),
        protocol: ptr::null_mut(),
        bound: ptr::null_mut(),
        connected: ptr::null_mut(),
        ever_connected: ptr::null_mut(),
        local_tcp: ptr::null_mut(),
        closed: ptr::null_mut(),
        listening: ptr::null_mut(),
        accept_count: ptr::null_mut(),
        local: ptr::null_mut(),
        local_len: ptr::null_mut(),
        peer_addr: ptr::null_mut(),
        peer_len: ptr::null_mut(),
        peer: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> ConnectView {
    let mut v = zero_view();
    a20_net_inet_connect_view_init(s, &mut v);
    v
}

/// The local pairing attempt.  0 = paired, negative = failed after rollback,
/// 1 = not handled here (the caller continues with the stack path).
#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_connect_local(s: *mut c_void,
                                                    connect_addr: *const c_void,
                                                    peer_len: usize,
                                                    max_queue: i32) -> i32 {
    let sv = view_of(s);

    /* A connecting socket with no bound address gets a loopback address and an
     * ephemeral port, which is what makes the child's peer address meaningful. */
    {
        let mut irq = 0u64;
        a20_net_inet_connect_lock(s, &mut irq);
        if a20_net_inet_connect_is_live(s) == 0 {
            a20_net_inet_connect_unlock(s, irq);
            return -ENOTSOCK;
        }
        if *sv.bound == 0 {
            a20_net_inet_connect_bind_ephemeral(s);
        }
        a20_net_inet_connect_unlock(s, irq);
    }

    let child = a20_net_inet_connect_alloc();
    if child.is_null() {
        return -ENOMEM;
    }

    let mut connect_port: u16 = 0;
    a20_net_inet_connect_sockaddr_port(connect_addr, peer_len, &mut connect_port);

    let ns_initial = a20_net_inet_connect_ns_initial(*sv.net_ns) != 0;
    let local_target = !ns_initial
        || (a20_net_inet_connect_tcp_path_fast() != 0
            && a20_net_inet_connect_is_local_target(connect_addr, peer_len) != 0);

    /* Resolved outside every other lock: the search walks the table one bucket
     * at a time and comes back with a reference, which is what lets the
     * connecting socket's lock be dropped before the pairs below. */
    let listener = if local_target {
        a20_net_inet_connect_find_listener(s, connect_port)
    } else {
        ptr::null_mut()
    };

    if !listener.is_null() {
        let lv = view_of(listener);
        let gate;
        {
            /* Everything the child inherits is read under the listener's own
             * lock alone.  The child is not yet registered and nothing else can
             * name it, so filling it in needs no lock of its own. */
            let mut lf = 0u64;
            a20_net_inet_connect_lock(listener, &mut lf);
            gate = a20_net_inet_connect_is_live(listener) != 0
                && *lv.listening != 0
                && *lv.accept_count < max_queue;
            if gate {
                let cv = view_of(child);
                a20_net_inet_connect_ns_put(*cv.net_ns);
                *cv.net_ns = a20_net_inet_connect_ns_get(*lv.net_ns);
                *cv.domain = *lv.domain;
                *cv.sock_type = SOCK_STREAM;
                *cv.protocol = *sv.protocol;
                *cv.bound = 1;
                *cv.connected = 1;
                *cv.ever_connected = 1;
                ptr::copy_nonoverlapping(lv.local, cv.local, *lv.local_len);
                *cv.local_len = *lv.local_len;
                ptr::copy_nonoverlapping(sv.local, cv.peer_addr, *sv.local_len);
                *cv.peer_len = *sv.local_len;
            }
            a20_net_inet_connect_unlock(listener, lf);
        }

        if gate {
            let cv = view_of(child);
            /* No net lock is held: registration takes a bucket lock of its own
             * and must not run under a socket lock. */
            let rr = a20_net_inet_connect_register(child);
            if rr < 0 {
                a20_net_inet_connect_free(listener);
                a20_net_inet_connect_free(child);
                return rr;
            }

            let mut wake_q = WakeQ::new();
            a20_net_inet_connect_wakeq_init(wake_q.as_mut_ptr());

            /* Second visit to both sockets: wire the pair and push.  Both may
             * have been closed during the unlocked window, so the liveness of
             * each is re-checked here. */
            let mut pair = ConnectPair::zero();
            a20_net_inet_connect_lock2(s, listener, &mut pair);
            let mut qr = -ECONNREFUSED;
            if a20_net_inet_connect_is_live(s) != 0
                && a20_net_inet_connect_is_live(listener) != 0
            {
                qr = a20_net_inet_connect_queue_push(listener, child);
            }
            if qr >= 0 {
                *cv.peer = s;
                *sv.peer = child;
                *sv.connected = 1;
                *sv.ever_connected = 1;
                *sv.local_tcp = 1;
                *cv.local_tcp = 1;
                a20_net_inet_connect_trace(0);
                a20_net_inet_connect_wake_accept(listener, wake_q.as_mut_ptr());
                a20_net_inet_connect_unlock2(&mut pair);
                a20_net_inet_connect_free(listener);
                a20_net_inet_connect_wakeq_flush(wake_q.as_mut_ptr());
                a20_net_inet_connect_drop_pcb(s);
                a20_net_inet_connect_trace(1);
                return 0;
            }

            /* The push failed.  The child is not in the pair -- it has just been
             * registered and nothing else can name it yet, so its own fields are
             * only written here. */
            *sv.connected = 0;
            *sv.peer = ptr::null_mut();
            *cv.closed = 1;
            a20_net_inet_connect_unlock2(&mut pair);
            let had_slot = a20_net_inet_connect_unregister(child);
            if had_slot != 0 {
                a20_net_inet_connect_free(child);
            }
            a20_net_inet_connect_free(child);
            a20_net_inet_connect_free(listener);
            a20_net_inet_connect_wakeq_flush(wake_q.as_mut_ptr());
            return qr;
        }
        a20_net_inet_connect_free(listener);
    }

    a20_net_inet_connect_free(child);
    if !ns_initial {
        /* A namespace that is not the initial one has no stack path to fall
         * back to, so the refusal is final. */
        return -ECONNREFUSED;
    }
    1
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
        protocol: i32,
        bound: i32,
        connected: i32,
        ever_connected: i32,
        local_tcp: i32,
        closed: i32,
        listening: i32,
        accept_count: i32,
        local: [u8; 32],
        local_len: usize,
        peer_addr: [u8; 32],
        peer_len: usize,
        peer: *mut Sock,
        live: i32,
        /* The view points at this slot: a temporary would leave the consumer
         * comparing a dangling value. */
        ns: *mut c_void,
    }

    impl Sock {
        const fn new() -> Sock {
            Sock {
                domain: 2,
                sock_type: SOCK_STREAM,
                protocol: 6,
                bound: 0,
                connected: 0,
                ever_connected: 0,
                local_tcp: 0,
                closed: 0,
                listening: 0,
                accept_count: 0,
                local: [1u8; 32],
                local_len: 4,
                peer_addr: [0u8; 32],
                peer_len: 0,
                peer: ptr::null_mut(),
                live: 1,
                ns: 1usize as *mut c_void,
            }
        }
    }

    static mut S_CONN: Sock = Sock::new();
    static mut S_LISTEN: Sock = Sock::new();
    static mut S_CHILD: Sock = Sock::new();
    static mut T_CHILD: *mut Sock = ptr::null_mut();
    static mut T_LISTENER: *mut Sock = ptr::null_mut();
    static mut T_NS_INITIAL: i32 = 1;
    static mut T_PATH_FAST: i32 = 1;
    static mut T_LOCAL_TARGET: i32 = 1;
    static mut T_PORT: u16 = 0;
    static mut T_BIND_EPHEMERAL: i32 = 0;
    static mut T_ALLOC_CALLS: i32 = 0;
    static mut T_FREE_CALLS: i32 = 0;
    static mut T_FIND_CALLS: i32 = 0;
    static mut T_REGISTER_RC: i32 = 0;
    static mut T_REGISTER_CALLS: i32 = 0;
    static mut T_UNREGISTER_RC: i32 = 1;
    static mut T_UNREGISTER_CALLS: i32 = 0;
    static mut T_PUSH_RC: i32 = 0;
    static mut T_PUSH_CALLS: i32 = 0;
    static mut T_NS_PUT: i32 = 0;
    static mut T_NS_GET: i32 = 0;
    static mut T_WAKE_ACCEPT: i32 = 0;
    static mut T_FLUSH: i32 = 0;
    static mut T_DROP_PCB: i32 = 0;
    static mut T_TRACE: [i32; 4] = [0; 4];
    static mut T_LOCK2: i32 = 0;

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
        S_CONN = Sock::new();
        S_LISTEN = Sock::new();
        S_CHILD = Sock::new();
        T_CHILD = ptr::null_mut();
        T_LISTENER = ptr::null_mut();
        T_NS_INITIAL = 1;
        T_PATH_FAST = 1;
        T_LOCAL_TARGET = 1;
        T_PORT = 0;
        T_BIND_EPHEMERAL = 0;
        T_ALLOC_CALLS = 0;
        T_FREE_CALLS = 0;
        T_FIND_CALLS = 0;
        T_REGISTER_RC = 0;
        T_REGISTER_CALLS = 0;
        T_UNREGISTER_RC = 1;
        T_UNREGISTER_CALLS = 0;
        T_PUSH_RC = 0;
        T_PUSH_CALLS = 0;
        T_NS_PUT = 0;
        T_NS_GET = 0;
        T_WAKE_ACCEPT = 0;
        T_FLUSH = 0;
        T_DROP_PCB = 0;
        T_TRACE = [0; 4];
        T_LOCK2 = 0;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_view_init(s: *mut c_void,
                                                        out: *mut ConnectView) {
        let k = &mut *(s as *mut Sock);
        *out = ConnectView {
            s,
            net_ns: &mut k.ns,
            domain: &mut k.domain,
            sock_type: &mut k.sock_type,
            protocol: &mut k.protocol,
            bound: &mut k.bound,
            connected: &mut k.connected,
            ever_connected: &mut k.ever_connected,
            local_tcp: &mut k.local_tcp,
            closed: &mut k.closed,
            listening: &mut k.listening,
            accept_count: &mut k.accept_count,
            local: k.local.as_mut_ptr(),
            local_len: &mut k.local_len,
            peer_addr: k.peer_addr.as_mut_ptr(),
            peer_len: &mut k.peer_len,
            peer: &mut k.peer as *mut *mut Sock as *mut *mut c_void,
        };
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_lock(_s: *mut c_void, _f: *mut u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_unlock(_s: *mut c_void, _f: u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_lock2(_a: *mut c_void, _b: *mut c_void,
                                                    pair: *mut ConnectPair) {
        T_LOCK2 += 1;
        (*pair).flags = 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_unlock2(pair: *mut ConnectPair) {
        assert_eq!((*pair).flags, 1);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_is_live(s: *mut c_void) -> i32 {
        (*(s as *const Sock)).live
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_bind_ephemeral(s: *mut c_void) {
        T_BIND_EPHEMERAL += 1;
        (*(s as *mut Sock)).bound = 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_alloc() -> *mut c_void {
        T_ALLOC_CALLS += 1;
        T_CHILD as *mut c_void
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_free(_s: *mut c_void) {
        T_FREE_CALLS += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_sockaddr_port(_a: *const c_void,
                                                            _l: usize,
                                                            port: *mut u16) -> i32 {
        *port = T_PORT;
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_ns_initial(_ns: *mut c_void) -> i32 {
        T_NS_INITIAL
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_tcp_path_fast() -> i32 {
        T_PATH_FAST
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_is_local_target(_a: *const c_void,
                                                              _l: usize) -> i32 {
        T_LOCAL_TARGET
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_find_listener(_s: *mut c_void,
                                                            _p: u16) -> *mut c_void {
        T_FIND_CALLS += 1;
        T_LISTENER as *mut c_void
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_ns_put(_ns: *mut c_void) {
        T_NS_PUT += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_ns_get(ns: *mut c_void) -> *mut c_void {
        T_NS_GET += 1;
        ns
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_register(_c: *mut c_void) -> i32 {
        T_REGISTER_CALLS += 1;
        T_REGISTER_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_unregister(_c: *mut c_void) -> i32 {
        T_UNREGISTER_CALLS += 1;
        T_UNREGISTER_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_queue_push(_l: *mut c_void,
                                                         _c: *mut c_void) -> i32 {
        T_PUSH_CALLS += 1;
        T_PUSH_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_wake_accept(_l: *mut c_void,
                                                          _w: *mut c_void) {
        T_WAKE_ACCEPT += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_wakeq_init(_w: *mut c_void) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_wakeq_flush(_w: *mut c_void) {
        T_FLUSH += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_drop_pcb(_s: *mut c_void) {
        T_DROP_PCB += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_connect_trace(which: i32) {
        if which >= 0 && which < 4 {
            T_TRACE[which as usize] += 1;
        }
    }

    unsafe fn sock() -> *mut Sock {
        ptr::addr_of_mut!(S_CONN)
    }

    /// A listener that accepts, plus the child the allocator hands back.
    unsafe fn pair_setup() -> (*mut Sock, *mut Sock, *mut Sock) {
        let s = ptr::addr_of_mut!(S_CONN);
        let listener = &mut *ptr::addr_of_mut!(S_LISTEN);
        listener.listening = 1;
        listener.local = [9u8; 32];
        listener.local_len = 6;
        listener.domain = 2;
        let child = &mut *ptr::addr_of_mut!(S_CHILD);
        (*s).local = [5u8; 32];
        (*s).local_len = 4;
        (*s).protocol = 6;
        T_LISTENER = listener;
        T_CHILD = child;
        (s, listener, child)
    }

    #[test]
    fn a_local_connect_wires_the_pair_and_pushes_the_child() {
        unsafe {
            let _serial = serial();
            reset();
            let (s, _listener, child) = pair_setup();
            assert_eq!(a20_net_inet_connect_local(s as *mut c_void, ptr::null(), 16, 4), 0);
            /* The connecting socket is bound on demand and connected. */
            assert_eq!(T_BIND_EPHEMERAL, 1);
            assert_eq!(((*s).bound, (*s).connected, (*s).ever_connected), (1, 1, 1));
            assert_eq!(((*s).local_tcp, (*child).local_tcp), (1, 1));
            assert_eq!((*s).peer, child);
            assert_eq!((*child).peer, s);
            /* The child inherited the listener's address and the connector's. */
            assert_eq!(core::slice::from_raw_parts((*child).local.as_ptr(), 6), &[9u8; 6]);
            assert_eq!((*child).local_len, 6);
            assert_eq!(core::slice::from_raw_parts((*child).peer_addr.as_ptr(), 4), &[5u8; 4]);
            assert_eq!((*child).peer_len, 4);
            assert_eq!(((*child).domain, (*child).sock_type, (*child).protocol), (2, 1, 6));
            assert_eq!((T_NS_PUT, T_NS_GET), (1, 1));
            /* Registered before the push, then woken and the pcb dropped. */
            assert_eq!((T_REGISTER_CALLS, T_PUSH_CALLS), (1, 1));
            assert_eq!((T_WAKE_ACCEPT, T_FLUSH, T_DROP_PCB), (1, 1, 1));
            assert_eq!(&T_TRACE[..2], &[1, 1]);
            /* The listener reference from the search is returned. */
            assert_eq!(T_FREE_CALLS, 1);
        }
    }

    #[test]
    fn a_closed_gate_falls_through_to_the_stack_path() {
        unsafe {
            let _serial = serial();
            reset();
            let (s, listener, _child) = pair_setup();
            /* Backlog full. */
            (*listener).accept_count = 4;
            assert_eq!(a20_net_inet_connect_local(s as *mut c_void, ptr::null(), 16, 4), 1);
            assert_eq!((T_REGISTER_CALLS, T_PUSH_CALLS), (0, 0));
            assert_eq!((*s).connected, 0);
            /* The child and the listener reference are both dropped. */
            assert_eq!(T_FREE_CALLS, 2);

            /* A listener that stopped listening. */
            reset();
            let (s, listener, _child) = pair_setup();
            (*listener).listening = 0;
            assert_eq!(a20_net_inet_connect_local(s as *mut c_void, ptr::null(), 16, 4), 1);
            assert_eq!(T_PUSH_CALLS, 0);

            /* A listener that died between the search and the gate. */
            reset();
            let (s, listener, _child) = pair_setup();
            (*listener).live = 0;
            assert_eq!(a20_net_inet_connect_local(s as *mut c_void, ptr::null(), 16, 4), 1);
            assert_eq!(T_PUSH_CALLS, 0);
        }
    }

    #[test]
    fn a_non_initial_namespace_without_a_listener_is_refused() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock();
            (*s).bound = 1;
            T_CHILD = ptr::addr_of_mut!(S_CHILD);
            T_NS_INITIAL = 0;
            T_LISTENER = ptr::null_mut();
            assert_eq!(a20_net_inet_connect_local(s as *mut c_void, ptr::null(), 16, 4), -ECONNREFUSED);
            assert_eq!(T_ALLOC_CALLS, 1);
            assert_eq!(T_FREE_CALLS, 1);
        }
    }

    #[test]
    fn a_stack_path_target_never_searches_for_a_listener() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock();
            (*s).bound = 1;
            T_CHILD = ptr::addr_of_mut!(S_CHILD);
            /* The fast path is configured but the target is not local. */
            T_LOCAL_TARGET = 0;
            assert_eq!(a20_net_inet_connect_local(s as *mut c_void, ptr::null(), 16, 4), 1);
            assert_eq!(T_FIND_CALLS, 0);
            /* The fast path is off entirely. */
            reset();
            let s = sock();
            (*s).bound = 1;
            T_CHILD = ptr::addr_of_mut!(S_CHILD);
            T_PATH_FAST = 0;
            assert_eq!(a20_net_inet_connect_local(s as *mut c_void, ptr::null(), 16, 4), 1);
            assert_eq!(T_FIND_CALLS, 0);
        }
    }

    #[test]
    fn a_failed_registration_rolls_back_before_any_pairing() {
        unsafe {
            let _serial = serial();
            reset();
            let (s, _listener, _child) = pair_setup();
            T_REGISTER_RC = -12;
            assert_eq!(a20_net_inet_connect_local(s as *mut c_void, ptr::null(), 16, 4), -12);
            assert_eq!(T_LOCK2, 0);
            assert_eq!(T_PUSH_CALLS, 0);
            /* The listener reference and the child both go back. */
            assert_eq!(T_FREE_CALLS, 2);
            assert_eq!((*s).connected, 0);
        }
    }

    #[test]
    fn a_failed_push_rolls_back_both_references() {
        unsafe {
            let _serial = serial();
            reset();
            let (s, _listener, child) = pair_setup();
            T_PUSH_RC = -111;
            assert_eq!(a20_net_inet_connect_local(s as *mut c_void, ptr::null(), 16, 4), -111);
            assert_eq!(((*s).connected, (*child).closed), (0, 1));
            assert!((*s).peer.is_null());
            assert_eq!((T_REGISTER_CALLS, T_UNREGISTER_CALLS), (1, 1));
            /* The registry's reference, the creator's and the listener's. */
            assert_eq!(T_FREE_CALLS, 3);
            assert_eq!(T_DROP_PCB, 0);
        }
    }

    #[test]
    fn an_unregistered_push_failure_drops_one_reference() {
        unsafe {
            let _serial = serial();
            reset();
            let (s, _listener, _child) = pair_setup();
            T_PUSH_RC = -111;
            T_UNREGISTER_RC = 0; /* no slot was released */
            assert_eq!(a20_net_inet_connect_local(s as *mut c_void, ptr::null(), 16, 4), -111);
            assert_eq!(T_FREE_CALLS, 2);
        }
    }

    #[test]
    fn a_dead_socket_is_refused_before_anything_is_allocated() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock();
            (*s).live = 0;
            assert_eq!(a20_net_inet_connect_local(s as *mut c_void, ptr::null(), 16, 4), -ENOTSOCK);
            assert_eq!((T_ALLOC_CALLS, T_BIND_EPHEMERAL), (0, 0));
        }
    }

    #[test]
    fn an_allocation_failure_reports_enomem() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock();
            (*s).bound = 1;
            T_CHILD = ptr::null_mut();
            assert_eq!(a20_net_inet_connect_local(s as *mut c_void, ptr::null(), 16, 4), -ENOMEM);
            assert_eq!(T_FIND_CALLS, 0);
        }
    }

    #[test]
    fn a_bound_socket_keeps_its_address() {
        unsafe {
            let _serial = serial();
            reset();
            let (s, _listener, _child) = pair_setup();
            (*s).bound = 1;
            a20_net_inet_connect_local(s as *mut c_void, ptr::null(), 16, 4);
            assert_eq!(T_BIND_EPHEMERAL, 0);
        }
    }
}
