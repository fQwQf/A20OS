#![no_std]

//! The accept transaction: the flag validation, the type/domain refusal, the
//! pop-or-block loop with its park, the peer address marshalling, and the
//! rollback that runs when the accepted child cannot be installed.
//!
//! The rollback is why this is Rust.  It is five steps whose order is load
//! bearing: the child is marked closed and its peer's back-pointer is detached
//! under the pair lock (so a concurrent reader of either socket sees a dead
//! socket, not a half-freed one), the peer's waiters are collected while the
//! lock is held and woken after it is dropped, the registry slot is released
//! with *no* socket lock held at all (a bucket lock may never nest under a
//! socket lock), and only then are the three references dropped -- with the
//! child's registry reference dropped only if it had a slot, because an AF_UNIX
//! child is never registered and dropping it would free the socket before the
//! wake calls below.

use core::ffi::c_void;
use core::ptr;

const AF_UNIX: i32 = 1;
const AF_INET: i32 = 2;
const AF_INET6: i32 = 10;
const AF_ALG: i32 = 38;

const SOCK_STREAM: i32 = 1;
const SOCK_SEQPACKET: i32 = 5;

const SOCK_CLOEXEC: i32 = 0o2000000;
const SOCK_NONBLOCK: i32 = 0o4000;

const EINVAL: i32 = 22;
const EAGAIN: i32 = 11;
const ENOTSOCK: i32 = 88;
const EOPNOTSUPP: i32 = 95;
const ERESTARTSYS: i32 = 512;

#[repr(C)]
pub struct AcceptView {
    pub s: *mut c_void,
    pub domain: *mut i32,
    pub sock_type: *mut i32,
    pub listening: *mut i32,
    pub nonblock: *mut i32,
    pub closed: *mut i32,
    pub recv_timeout_ticks: *mut u64,
    pub accept_head: *mut *mut c_void,
}

#[repr(C)]
pub struct AcceptPair {
    pub lo: *mut c_void,
    pub hi: *mut c_void,
    pub flags: u64,
}

unsafe extern "C" {
    fn a20_net_accept_txn_view_init(s: *mut c_void, out: *mut AcceptView);
    fn a20_net_accept_txn_alg(s: *mut c_void, addrlen: *mut usize, flags: i32) -> i32;
    fn a20_net_accept_txn_lock(s: *mut c_void) -> u64;
    fn a20_net_accept_txn_unlock(s: *mut c_void, flags: u64);
    fn a20_net_accept_txn_pop(s: *mut c_void) -> *mut c_void;
    fn a20_net_accept_txn_now() -> u64;
    fn a20_net_accept_txn_have_task() -> i32;
    fn a20_net_accept_txn_has_signal() -> i32;
    fn a20_net_accept_txn_wait_expired_locked(s: *mut c_void, start: u64) -> i32;
    fn a20_net_accept_txn_park(s: *mut c_void, deadline: u64) -> i32;
    fn a20_net_accept_txn_child_ready(child: *mut c_void);
    fn a20_net_accept_txn_child_peer_len(child: *mut c_void) -> usize;
    fn a20_net_accept_txn_child_peer_copy(child: *mut c_void, addr: *mut c_void, n: usize);
    fn a20_net_accept_txn_child_set_nonblock(child: *mut c_void, nonblock: i32);
    fn a20_net_accept_txn_install(child: *mut c_void, flags: i32) -> i32;
    fn a20_net_accept_txn_snapshot_peer(child: *mut c_void, out_peer: *mut *mut c_void);
    fn a20_net_accept_txn_lock2(a: *mut c_void, b: *mut c_void, out: *mut AcceptPair);
    fn a20_net_accept_txn_unlock2(pair: *const AcceptPair);
    fn a20_net_accept_txn_child_mark_closed(child: *mut c_void);
    fn a20_net_accept_txn_peer_detach(peer: *mut c_void, child: *mut c_void) -> i32;
    fn a20_net_accept_txn_peer_collect_read(peer: *mut c_void, wake_q: *mut c_void) -> i32;
    fn a20_net_accept_txn_peer_collect_write(peer: *mut c_void, wake_q: *mut c_void) -> i32;
    fn a20_net_accept_txn_unregister(child: *mut c_void) -> i32;
    fn a20_net_accept_txn_free(s: *mut c_void);
    fn a20_net_accept_txn_child_destroy(child: *mut c_void);
    fn a20_net_accept_txn_peer_wake_read(peer: *mut c_void);
    fn a20_net_accept_txn_peer_wake_write(peer: *mut c_void);
    fn a20_net_accept_txn_wakeq_init(wake_q: *mut c_void);
    fn a20_net_accept_txn_wakeq_flush(wake_q: *mut c_void);
}

fn zero_view() -> AcceptView {
    AcceptView {
        s: ptr::null_mut(),
        domain: ptr::null_mut(),
        sock_type: ptr::null_mut(),
        listening: ptr::null_mut(),
        nonblock: ptr::null_mut(),
        closed: ptr::null_mut(),
        recv_timeout_ticks: ptr::null_mut(),
        accept_head: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> AcceptView {
    let mut v = zero_view();
    a20_net_accept_txn_view_init(s, &mut v);
    v
}

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
pub unsafe extern "C" fn a20_net_accept_sock(
    s: *mut c_void, addr: *mut c_void, addrlen: *mut usize, flags: i32,
) -> i32 {
    if (flags & !(SOCK_CLOEXEC | SOCK_NONBLOCK)) != 0 {
        return -EINVAL;
    }
    if s.is_null() {
        return -ENOTSOCK;
    }
    let v = view_of(s);
    if *v.domain == AF_ALG {
        return a20_net_accept_txn_alg(s, addrlen, flags);
    }
    let unix_seqpacket = *v.domain == AF_UNIX && *v.sock_type == SOCK_SEQPACKET;
    if (*v.sock_type != SOCK_STREAM && !unix_seqpacket)
        || (*v.domain != AF_INET && *v.domain != AF_INET6 && *v.domain != AF_UNIX)
    {
        return -EOPNOTSUPP;
    }
    if *v.listening == 0 {
        return -EINVAL;
    }

    let start = a20_net_accept_txn_now();
    let child: *mut c_void;
    loop {
        let irq = a20_net_accept_txn_lock(s);
        if *v.closed != 0 {
            a20_net_accept_txn_unlock(s, irq);
            return -EINVAL;
        }
        let popped = a20_net_accept_txn_pop(s);
        if !popped.is_null() {
            a20_net_accept_txn_unlock(s, irq);
            child = popped;
            break;
        }
        if *v.nonblock != 0 {
            a20_net_accept_txn_unlock(s, irq);
            return -EAGAIN;
        }
        if a20_net_accept_txn_have_task() == 0 {
            a20_net_accept_txn_unlock(s, irq);
            return -EAGAIN;
        }
        if a20_net_accept_txn_has_signal() != 0 {
            a20_net_accept_txn_unlock(s, irq);
            return -ERESTARTSYS;
        }
        if a20_net_accept_txn_wait_expired_locked(s, start) != 0 {
            a20_net_accept_txn_unlock(s, irq);
            return -EAGAIN;
        }
        let deadline = if *v.recv_timeout_ticks != 0 {
            start.wrapping_add(*v.recv_timeout_ticks)
        } else {
            0
        };
        a20_net_accept_txn_unlock(s, irq);
        let e = a20_net_accept_txn_park(s, deadline);
        if e == 0 {
            continue;
        }
        return e;
    }

    a20_net_accept_txn_child_ready(child);

    if !addr.is_null() && !addrlen.is_null() && *addrlen > 0 {
        let n = {
            let plen = a20_net_accept_txn_child_peer_len(child);
            if plen < *addrlen {
                plen
            } else {
                *addrlen
            }
        };
        a20_net_accept_txn_child_peer_copy(child, addr, n);
        *addrlen = n;
    }

    /* Linux accept/accept4 do not inherit O_NONBLOCK from the listener.
     * A nonblocking service listener may hand blocking request I/O to its
     * worker; inheriting here races the client's first write and loses it. */
    a20_net_accept_txn_child_set_nonblock(
        child,
        if (flags & SOCK_NONBLOCK) != 0 {
            1
        } else {
            0
        },
    );
    let newfd = a20_net_accept_txn_install(child, flags);
    if newfd < 0 {
        let mut wake_q = WakeQ::new();
        a20_net_accept_txn_wakeq_init(wake_q.as_mut_ptr());
        let mut peer: *mut c_void = ptr::null_mut();
        let mut drain_peer_read = 0;
        let mut drain_peer_write = 0;
        /* child and its peer, two socket locks, ascending by address.  child is
         * pinned -- it is the socket accept() just popped off the listener's
         * queue, carrying its creator's reference -- but child->peer is a plain
         * back-pointer with no reference of its own, so it is sampled and
         * referenced under the child's own lock before the pair is taken. */
        a20_net_accept_txn_snapshot_peer(child, &mut peer);
        let mut pair = AcceptPair {
            lo: ptr::null_mut(),
            hi: ptr::null_mut(),
            flags: 0,
        };
        a20_net_accept_txn_lock2(child, peer, &mut pair);
        a20_net_accept_txn_child_mark_closed(child);
        if !peer.is_null() && a20_net_accept_txn_peer_detach(peer, child) != 0 {
            drain_peer_read = a20_net_accept_txn_peer_collect_read(peer, wake_q.as_mut_ptr());
            drain_peer_write = a20_net_accept_txn_peer_collect_write(peer, wake_q.as_mut_ptr());
        }
        a20_net_accept_txn_unlock2(&pair);
        /* The registry slot has to be released with no net lock held at all:
         * net_socket_unregister() takes a bucket lock of its own, and a bucket
         * lock may never be nested under a socket lock.  The child is already
         * marked closed, so a table scan that lands on it in between finds it
         * dead and skips it. */
        let had_slot = a20_net_accept_txn_unregister(child) != 0;
        /* Registry reference, dropped outside the lock: it can free.  An
         * AF_UNIX child is never registered, so on that path the reference does
         * not exist and dropping it here would free the socket before the
         * wake_all() calls below. */
        if had_slot {
            a20_net_accept_txn_free(child);
        }
        a20_net_accept_txn_wakeq_flush(wake_q.as_mut_ptr());
        if drain_peer_read != 0 {
            a20_net_accept_txn_peer_wake_read(peer);
        }
        if drain_peer_write != 0 {
            a20_net_accept_txn_peer_wake_write(peer);
        }
        a20_net_accept_txn_child_destroy(child);
        a20_net_accept_txn_free(child);
        if !peer.is_null() {
            a20_net_accept_txn_free(peer);
        }
        return newfd;
    }
    newfd
}

#[cfg(test)]
#[allow(static_mut_refs)]
mod tests {
    use super::*;

    #[repr(C)]
    struct Sock {
        domain: i32,
        sock_type: i32,
        listening: i32,
        nonblock: i32,
        closed: i32,
        recv_timeout_ticks: u64,
        accept_head: *mut c_void,
        peer: *mut c_void,
        peer_len: usize,
        peer_addr: [u8; 16],
        install_rc: i32,
        is_child: i32,
    }

    impl Sock {
        const fn new() -> Sock {
            Sock {
                domain: AF_INET,
                sock_type: SOCK_STREAM,
                listening: 1,
                nonblock: 0,
                closed: 0,
                recv_timeout_ticks: 0,
                accept_head: ptr::null_mut(),
                peer: ptr::null_mut(),
                peer_len: 0,
                peer_addr: [0u8; 16],
                install_rc: 7,
                is_child: 0,
            }
        }
    }

    static mut S_SOCK: Sock = Sock::new();
    static mut S_CHILD: Sock = Sock::new();
    static mut S_PEER: Sock = Sock::new();
    static mut T_ALG: i32 = 0;
    static mut T_POP: i32 = 0;
    static mut T_POP_RC: i32 = 0;
    static mut T_HAVE_TASK: i32 = 1;
    static mut T_SIGNAL: i32 = 0;
    static mut T_EXPIRED: i32 = 0;
    static mut T_PARK_CALLS: i32 = 0;
    static mut T_PARK_RC: [i32; 4] = [0; 4];
    static mut T_READY: i32 = 0;
    static mut T_INSTALL: i32 = 0;
    static mut T_INSTALL_FLAGS: i32 = 0;
    static mut T_SNAPSHOT: i32 = 0;
    static mut T_LOCK2: i32 = 0;
    static mut T_MARK_CLOSED: i32 = 0;
    static mut T_DETACH: i32 = 0;
    static mut T_DETACH_RC: i32 = 0;
    static mut T_COLLECT_READ: i32 = 0;
    static mut T_COLLECT_WRITE: i32 = 0;
    static mut T_UNREGISTER: i32 = 0;
    static mut T_UNREGISTER_RC: i32 = 0;
    static mut T_FREE: i32 = 0;
    static mut T_DESTROY: i32 = 0;
    static mut T_WAKE_READ: i32 = 0;
    static mut T_WAKE_WRITE: i32 = 0;
    static mut T_TRACE: [i32; 16] = [0; 16];
    static mut T_TRACE_N: usize = 0;
    static mut T_ORDER: i32 = 0;

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
        S_CHILD = Sock::new();
        S_CHILD.is_child = 1;
        S_PEER = Sock::new();
        T_ALG = 0;
        T_POP = 0;
        T_POP_RC = 0;
        T_HAVE_TASK = 1;
        T_SIGNAL = 0;
        T_EXPIRED = 0;
        T_PARK_CALLS = 0;
        T_PARK_RC = [0; 4];
        T_READY = 0;
        T_INSTALL = 0;
        T_INSTALL_FLAGS = 0;
        T_SNAPSHOT = 0;
        T_LOCK2 = 0;
        T_MARK_CLOSED = 0;
        T_DETACH = 0;
        T_DETACH_RC = 0;
        T_COLLECT_READ = 0;
        T_COLLECT_WRITE = 0;
        T_UNREGISTER = 0;
        T_UNREGISTER_RC = 0;
        T_FREE = 0;
        T_DESTROY = 0;
        T_WAKE_READ = 0;
        T_WAKE_WRITE = 0;
        T_TRACE = [0; 16];
        T_TRACE_N = 0;
        T_ORDER = 0;
    }

    unsafe fn sock() -> *mut c_void {
        ptr::addr_of_mut!(S_SOCK) as *mut c_void
    }

    unsafe fn child() -> *mut c_void {
        ptr::addr_of_mut!(S_CHILD) as *mut c_void
    }

    unsafe fn peer() -> *mut c_void {
        ptr::addr_of_mut!(S_PEER) as *mut c_void
    }

    unsafe fn me() -> &'static mut Sock {
        &mut *(sock() as *mut Sock)
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_view_init(s: *mut c_void,
                                                 out: *mut AcceptView) {
        let k = &mut *(s as *mut Sock);
        *out = AcceptView {
            s,
            domain: &mut k.domain,
            sock_type: &mut k.sock_type,
            listening: &mut k.listening,
            nonblock: &mut k.nonblock,
            closed: &mut k.closed,
            recv_timeout_ticks: &mut k.recv_timeout_ticks,
            accept_head: &mut k.accept_head,
        };
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_alg(_s: *mut c_void, _al: *mut usize,
                                            _f: i32) -> i32 {
        T_ALG += 1;
        21
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_lock(_s: *mut c_void) -> u64 {
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_unlock(_s: *mut c_void, _f: u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_pop(_s: *mut c_void) -> *mut c_void {
        T_POP += 1;
        if T_POP == 1 && T_POP_RC == 0 {
            T_ORDER = 1;
            return child();
        }
        if T_POP_RC == 0 {
            ptr::null_mut()
        } else if T_POP >= T_POP_RC {
            child()
        } else {
            ptr::null_mut()
        }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_have_task() -> i32 {
        T_HAVE_TASK
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_has_signal() -> i32 {
        T_SIGNAL
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_wait_expired_locked(_s: *mut c_void,
                                                             _st: u64) -> i32 {
        T_EXPIRED
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_now() -> u64 {
        1000
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_park(_s: *mut c_void, _d: u64) -> i32 {
        let at = T_PARK_CALLS as usize;
        T_PARK_CALLS += 1;
        if at < T_PARK_RC.len() {
            T_PARK_RC[at]
        } else {
            0
        }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_child_ready(_c: *mut c_void) {
        T_READY += 1;
        T_TRACE[T_TRACE_N] = 10;
        T_TRACE_N += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_child_peer_len(c: *mut c_void) -> usize {
        (*(c as *mut Sock)).peer_len
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_child_peer_copy(c: *mut c_void,
                                                        addr: *mut c_void,
                                                        n: usize) {
        let k = &mut *(c as *mut Sock);
        ptr::copy_nonoverlapping(k.peer_addr.as_ptr(), addr as *mut u8, n);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_child_set_nonblock(c: *mut c_void,
                                                           v: i32) {
        (*(c as *mut Sock)).nonblock = v;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_install(c: *mut c_void, flags: i32) -> i32 {
        T_INSTALL += 1;
        T_INSTALL_FLAGS = flags;
        (*(c as *mut Sock)).install_rc
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_snapshot_peer(c: *mut c_void,
                                                      out: *mut *mut c_void) {
        T_SNAPSHOT += 1;
        T_TRACE[T_TRACE_N] = 20;
        T_TRACE_N += 1;
        *out = (*(c as *mut Sock)).peer;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_lock2(a: *mut c_void, b: *mut c_void,
                                              out: *mut AcceptPair) {
        T_LOCK2 += 1;
        (*out).lo = a;
        (*out).hi = b;
        (*out).flags = 0;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_unlock2(_p: *const AcceptPair) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_child_mark_closed(c: *mut c_void) {
        T_MARK_CLOSED += 1;
        (*(c as *mut Sock)).closed = 1;
        T_TRACE[T_TRACE_N] = 30;
        T_TRACE_N += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_peer_detach(p: *mut c_void,
                                                    c: *mut c_void) -> i32 {
        T_DETACH += 1;
        if (*(p as *mut Sock)).peer == c {
            (*(p as *mut Sock)).peer = ptr::null_mut();
            return 1;
        }
        T_DETACH_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_peer_collect_read(_p: *mut c_void,
                                                          _w: *mut c_void) -> i32 {
        T_COLLECT_READ += 1;
        T_TRACE[T_TRACE_N] = 40;
        T_TRACE_N += 1;
        1
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_peer_collect_write(_p: *mut c_void,
                                                           _w: *mut c_void) -> i32 {
        T_COLLECT_WRITE += 1;
        T_TRACE[T_TRACE_N] = 41;
        T_TRACE_N += 1;
        1
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_unregister(_c: *mut c_void) -> i32 {
        T_UNREGISTER += 1;
        T_TRACE[T_TRACE_N] = 50;
        T_TRACE_N += 1;
        T_UNREGISTER_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_free(_s: *mut c_void) {
        T_FREE += 1;
        T_TRACE[T_TRACE_N] = 60;
        T_TRACE_N += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_child_destroy(_c: *mut c_void) {
        T_DESTROY += 1;
        T_TRACE[T_TRACE_N] = 70;
        T_TRACE_N += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_peer_wake_read(_p: *mut c_void) {
        T_WAKE_READ += 1;
        T_TRACE[T_TRACE_N] = 80;
        T_TRACE_N += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_peer_wake_write(_p: *mut c_void) {
        T_WAKE_WRITE += 1;
        T_TRACE[T_TRACE_N] = 81;
        T_TRACE_N += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_wakeq_init(_w: *mut c_void) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_txn_wakeq_flush(_w: *mut c_void) {
        T_TRACE[T_TRACE_N] = 90;
        T_TRACE_N += 1;
    }

    #[test]
    fn the_flag_validation_and_the_type_refusal() {
        unsafe {
            let _serial = serial();
            reset();
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0x1), -EINVAL);
            assert_eq!(a20_net_accept_sock(ptr::null_mut(), ptr::null_mut(),
                                           ptr::null_mut(), 0), -ENOTSOCK);
            /* A datagram socket cannot accept. */
            reset();
            me().sock_type = 2;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), -EOPNOTSUPP);
            /* A unix seqpacket socket can. */
            reset();
            let s = me();
            s.domain = AF_UNIX;
            s.sock_type = SOCK_SEQPACKET;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), 7);
            /* A family the stack does not own cannot. */
            reset();
            me().domain = 0;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), -EOPNOTSUPP);
            /* A socket that is not listening cannot. */
            reset();
            me().listening = 0;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), -EINVAL);
            /* The algorithm domain is dispatched before all of it. */
            reset();
            me().domain = AF_ALG;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), 21);
        }
    }

    #[test]
    fn a_child_is_handed_back_with_its_peer_address_and_nonblock() {
        unsafe {
            let _serial = serial();
            reset();
            let c = &mut *(child() as *mut Sock);
            c.peer_len = 8;
            c.peer_addr[..4].copy_from_slice(&[1, 2, 3, 4]);
            let mut buf = [0u8; 16];
            let mut alen: usize = 16;
            assert_eq!(a20_net_accept_sock(sock(), buf.as_mut_ptr() as *mut c_void,
                                           &mut alen, 0), 7);
            assert_eq!(alen, 8);
            assert_eq!(&buf[..4], &[1, 2, 3, 4]);
            assert_eq!(T_READY, 1);
            /* The caller's buffer is shorter: the copy is truncated and the
             * length reports what was copied. */
            reset();
            let c = &mut *(child() as *mut Sock);
            c.peer_len = 8;
            let mut alen2: usize = 4;
            assert_eq!(a20_net_accept_sock(sock(), buf.as_mut_ptr() as *mut c_void,
                                           &mut alen2, 0), 7);
            assert_eq!(alen2, 4);
        }
    }

    #[test]
    fn accepted_nonblock_depends_only_on_accept_flags() {
        unsafe {
            let _serial = serial();
            for (domain, sock_type) in [(AF_INET, SOCK_STREAM), (AF_INET6, SOCK_STREAM),
                                        (AF_UNIX, SOCK_STREAM), (AF_UNIX, SOCK_SEQPACKET)] {
                for listener_nonblock in [0, 1] {
                    for flags in [0, SOCK_CLOEXEC, SOCK_NONBLOCK, SOCK_NONBLOCK | SOCK_CLOEXEC] {
                        reset();
                        me().domain = domain;
                        me().sock_type = sock_type;
                        me().nonblock = listener_nonblock;
                        assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                                       ptr::null_mut(), flags), 7);
                        assert_eq!((*(child() as *mut Sock)).nonblock,
                                   if flags & SOCK_NONBLOCK != 0 { 1 } else { 0 });
                        assert_eq!(me().nonblock, listener_nonblock);
                        assert_eq!(T_INSTALL_FLAGS, flags);
                    }
                }
            }
        }
    }

    #[test]
    fn a_closed_listener_answers_einval_and_a_nonblocking_one_eagain() {
        unsafe {
            let _serial = serial();
            reset();
            me().closed = 1;
            T_POP_RC = 99;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), -EINVAL);
            reset();
            me().nonblock = 1;
            T_POP_RC = 99;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), -EAGAIN);
            assert_eq!(T_PARK_CALLS, 0);
        }
    }

    #[test]
    fn the_waiting_loop_rechecks_and_parks_with_the_timeout() {
        unsafe {
            let _serial = serial();
            reset();
            me().recv_timeout_ticks = 50;
            /* The second pop finds the child. */
            T_POP_RC = 2;
            T_PARK_RC[0] = 0;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), 7);
            assert_eq!(T_POP, 2);
            assert_eq!(T_PARK_CALLS, 1);

            /* An expired wait does not park at all. */
            reset();
            T_POP_RC = 99;
            T_EXPIRED = 1;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), -EAGAIN);
            assert_eq!(T_PARK_CALLS, 0);

            /* A signal before the park is ERESTARTSYS. */
            reset();
            T_POP_RC = 99;
            T_SIGNAL = 1;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), -ERESTARTSYS);
            assert_eq!(T_PARK_CALLS, 0);

            /* No task at all: EAGAIN. */
            reset();
            T_POP_RC = 99;
            T_HAVE_TASK = 0;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), -EAGAIN);
        }
    }

    #[test]
    fn the_install_failure_rollback_runs_in_the_documented_order() {
        unsafe {
            let _serial = serial();
            reset();
            let c = &mut *(child() as *mut Sock);
            c.install_rc = -24;
            c.peer = peer();
            (*(peer() as *mut Sock)).peer = child();
            T_UNREGISTER_RC = 1;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), -24);
            /* snapshot peer, mark closed, detach, collect both queues, unlock,
             * unregister, free the registry reference, flush, wake read, wake
             * write, destroy, free the child, free the peer. */
            assert_eq!(T_SNAPSHOT, 1);
            assert_eq!(T_MARK_CLOSED, 1);
            assert_eq!(T_DETACH, 1);
            assert_eq!(T_COLLECT_READ, 1);
            assert_eq!(T_COLLECT_WRITE, 1);
            assert_eq!(T_UNREGISTER, 1);
            assert_eq!(T_DESTROY, 1);
            assert_eq!(T_WAKE_READ, 1);
            assert_eq!(T_WAKE_WRITE, 1);
            assert_eq!(T_FREE, 3);
            assert_eq!((*(child() as *mut Sock)).closed, 1);
            assert_eq!((*(peer() as *mut Sock)).peer, ptr::null_mut());
            let order = &T_TRACE[..T_TRACE_N];
            assert_eq!(order[0], 10);
            assert_eq!(order[1], 20);
            assert_eq!(order[2], 30);
            assert_eq!(order[3], 40);
            assert_eq!(order[4], 41);
            assert_eq!(order[5], 50);
            assert_eq!(order[6], 60);
            assert_eq!(order[7], 90);
            assert_eq!(order[8], 80);
            assert_eq!(order[9], 81);
            assert_eq!(order[10], 70);
        }
    }

    #[test]
    fn a_unix_child_without_a_slot_drops_no_registry_reference() {
        unsafe {
            let _serial = serial();
            reset();
            let c = &mut *(child() as *mut Sock);
            c.install_rc = -24;
            c.peer = peer();
            (*(peer() as *mut Sock)).peer = child();
            T_UNREGISTER_RC = 0;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), -24);
            /* The child's registry reference does not exist, so only the
             * creator's reference and the peer's are dropped. */
            assert_eq!(T_FREE, 2);
            assert_eq!(T_WAKE_READ, 1);
            assert_eq!(T_WAKE_WRITE, 1);
        }
    }

    #[test]
    fn a_peer_that_does_not_point_back_is_not_drained() {
        unsafe {
            let _serial = serial();
            reset();
            let c = &mut *(child() as *mut Sock);
            c.install_rc = -24;
            c.peer = peer();
            /* The peer's back-pointer names somebody else. */
            (*(peer() as *mut Sock)).peer = 0x999 as *mut c_void;
            T_UNREGISTER_RC = 1;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), -24);
            assert_eq!(T_DETACH, 1);
            assert_eq!(T_COLLECT_READ, 0);
            assert_eq!(T_COLLECT_WRITE, 0);
            assert_eq!(T_WAKE_READ, 0);
            assert_eq!(T_WAKE_WRITE, 0);
            /* The peer is still referenced and still freed. */
            assert_eq!(T_FREE, 3);
        }
    }

    #[test]
    fn a_child_with_no_peer_skips_the_whole_detach_block() {
        unsafe {
            let _serial = serial();
            reset();
            (*(child() as *mut Sock)).install_rc = -24;
            T_UNREGISTER_RC = 1;
            assert_eq!(a20_net_accept_sock(sock(), ptr::null_mut(),
                                           ptr::null_mut(), 0), -24);
            assert_eq!(T_DETACH, 0);
            assert_eq!(T_COLLECT_READ, 0);
            assert_eq!(T_DESTROY, 1);
            assert_eq!(T_FREE, 2);
        }
    }
}
