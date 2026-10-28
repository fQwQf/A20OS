#![no_std]

//! Socket vfile lifecycle: the close teardown transaction, the install
//! transaction and its rollback, and the fd pin that keeps close/reuse from
//! running under a live syscall.
//!
//! The C side keeps the primitives this module calls: the socket lock (single
//! and the address-ordered pair), `net_inet_socket_destroy`, the channel
//! endpoint release, the wait-queue collect/wake/flush trio, the receive-queue
//! detach, the registry unregister and the socket free, the vfile allocator and
//! the fd-table install.  Everything that is a *decision* is here: the order in
//! which a teardown marks the socket dead, detaches its queues, unlinks its
//! peer, releases the registry slot and drops each reference; whether a queued
//! accepted child owns a registry reference; whether an install failure tears
//! the socket down before releasing the vfile; and whether a resolved fd is
//! still a socket after the identity test.
//!
//! The socket's fields are reached through a borrowed view built by C, so no
//! struct ABI moves and the lock discipline is unchanged: the pair is taken in
//! ascending address order by `net_sock_lock2()` and the registry slot is
//! released with no net lock held, exactly as the C code did.

use core::ffi::c_void;
use core::ptr;

const SOCK_STREAM: i32 = 1;
const SOCK_SEQPACKET: i32 = 5;

const EBADF: i32 = 9;
const EINVAL: i32 = 22;
const ENOMEM: i32 = 12;
const ENOTSOCK: i32 = 88;

const WAIT_ACCEPT: i32 = 0;
const WAIT_READ: i32 = 1;
const WAIT_WRITE: i32 = 2;

const PROC_WAKE_EVENT: i32 = 0;
const PROC_WAKE_EXIT: i32 = 1;

/// Borrowed view of the fields this module transitions, plus the registry
/// reference the teardown drops.
#[repr(C)]
pub struct CloseView {
    pub s: *mut c_void,
    pub closed: *mut i32,
    pub peer_closed: *mut i32,
    pub peer: *mut *mut c_void,
    pub sock_type: *const i32,
    pub accept_head: *mut *mut c_void,
    pub accept_tail: *mut *mut c_void,
    pub accept_count: *mut i32,
    /// The accepted-child link, for a view over a child.
    pub accept_next: *mut *mut c_void,
    /// `s->refs`, so a peer can be pinned exactly where it is sampled.
    pub refs: *mut i32,
    /// `vf->priv`, cleared at the end of the root teardown.
    pub vf_priv: *mut *mut c_void,
}

unsafe extern "C" {
    fn a20_net_close_view_init(s: *mut c_void, out: *mut CloseView);
    fn a20_net_close_lock(v: *const CloseView, flags: *mut u64);
    fn a20_net_close_unlock(v: *const CloseView, flags: u64);
    fn a20_net_close_lock_pair(a: *const CloseView, b: *const CloseView,
                               lo: *mut *const CloseView,
                               hi: *mut *const CloseView,
                               flags: *mut u64);
    fn a20_net_close_unlock_pair(lo: *const CloseView, hi: *const CloseView,
                                 flags: u64);
    fn a20_net_close_inet_destroy(v: *const CloseView);
    fn a20_net_close_channel_release(v: *const CloseView);
    fn a20_net_close_waitq(v: *const CloseView, which: i32) -> *mut c_void;
    fn a20_net_close_collect(q: *mut c_void, reason: i32, wake_q: *mut c_void) -> i32;
    fn a20_net_close_wake(q: *mut c_void, reason: i32);
    fn a20_net_close_flush(wake_q: *mut c_void);
    fn a20_net_close_wake_q_reinit(wake_q: *mut c_void);
    fn a20_net_close_rxq_detach(v: *const CloseView) -> *mut c_void;
    fn a20_net_close_msg_free_chain(m: *mut c_void);
    fn a20_net_close_unregister(v: *const CloseView) -> i32;
    fn a20_net_close_socket_free(v: *const CloseView);
    fn a20_net_ref_acquire(refs: *mut i32) -> i32;
    /* install / pin */
    fn a20_net_install_prepare(s: *mut c_void, flags: i32) -> *mut c_void;
    fn a20_net_install_fdtable(vf: *mut c_void, flags: i32) -> i32;
    fn a20_net_install_set_owner(s: *mut c_void, gfd: i32, vf: *mut c_void);
    fn a20_net_install_free(vf: *mut c_void);
    fn a20_net_fdtable_current_file(fd: i32) -> *mut c_void;
    fn a20_net_vfile_socket(vf: *mut c_void) -> *mut c_void;
    fn a20_net_vfile_put(vf: *mut c_void);
    fn a20_net_socket_ref(s: *mut c_void);
}

fn zero_view() -> CloseView {
    CloseView {
        s: ptr::null_mut(),
        closed: ptr::null_mut(),
        peer_closed: ptr::null_mut(),
        peer: ptr::null_mut(),
        sock_type: ptr::null(),
        accept_head: ptr::null_mut(),
        accept_tail: ptr::null_mut(),
        accept_count: ptr::null_mut(),
        accept_next: ptr::null_mut(),
        refs: ptr::null_mut(),
        vf_priv: ptr::null_mut(),
    }
}

unsafe fn collect(v: &CloseView, which: i32, reason: i32,
                  wake_q: *mut c_void) -> i32 {
    let q = unsafe { a20_net_close_waitq(v, which) };
    if q.is_null() {
        return 0;
    }
    unsafe { a20_net_close_collect(q, reason, wake_q) }
}

unsafe fn wake(v: &CloseView, which: i32, reason: i32) {
    let q = unsafe { a20_net_close_waitq(v, which) };
    if !q.is_null() {
        unsafe { a20_net_close_wake(q, reason) };
    }
}

/// The peer-unlink predicate: a stream or seqpacket peer is unlinked even when
/// it is already closed (its writers must still see the hangup); any other type
/// is unlinked only while it is live.
fn should_unlink_peer(sock_type: i32, peer_live: bool, peer_peer: *mut c_void,
                      s: *mut c_void) -> bool {
    (sock_type == SOCK_STREAM || sock_type == SOCK_SEQPACKET || peer_live)
        && peer_peer == s
}

/// Tear down one socket: mark it dead, detach its queues and peer, release the
/// registry slot, then drop every reference in the order that makes each drop
/// safe.  `root` owns the vfile; a queued accepted child does not.
unsafe fn teardown(root: *const CloseView, wake_q: *mut c_void,
                   is_root: bool) -> *mut c_void {
    let v = unsafe { &*root };
    if v.s.is_null() {
        return ptr::null_mut();
    }
    if is_root {
        unsafe { a20_net_close_inet_destroy(v) };
        unsafe { a20_net_close_channel_release(v) };
    }

    /* Sample the peer and pin it under the socket's own lock: the back-pointer
     * carries no reference of its own, so reading it before any lock could name
     * a socket another CPU has already freed. */
    let mut peer = zero_view();
    let p: *mut c_void;
    {
        let mut flags = 0u64;
        unsafe { a20_net_close_lock(v, &mut flags) };
        p = unsafe { *v.peer };
        if !p.is_null() {
            unsafe { a20_net_close_view_init(p, &mut peer) };
            unsafe { a20_net_ref_acquire(peer.refs) };
        }
        unsafe { a20_net_close_unlock(v, flags) };
    }

    let mut lo: *const CloseView = ptr::null();
    let mut hi: *const CloseView = ptr::null();
    let mut flags = 0u64;
    unsafe {
        a20_net_close_lock_pair(
            v,
            if p.is_null() { ptr::null() } else { &peer },
            &mut lo,
            &mut hi,
            &mut flags,
        )
    };

    /* Marked dead before the slot is released: the unregister below runs with no
     * net lock held, so a table scan landing on this socket must see it closed. */
    unsafe { *v.closed = 1 };
    let mut drain_accept = 0;
    let mut drain_read = 0;
    let mut drain_write = 0;
    /* The root drains its own queues before unlinking the peer; a child does it
     * after, which is the order the C teardown used for each shape. */
    if is_root {
        drain_accept = unsafe { collect(v, WAIT_ACCEPT, PROC_WAKE_EXIT, wake_q) };
        drain_read = unsafe { collect(v, WAIT_READ, PROC_WAKE_EXIT, wake_q) };
        drain_write = unsafe { collect(v, WAIT_WRITE, PROC_WAKE_EXIT, wake_q) };
    }
    let mut drain_peer_read = 0;
    let mut drain_peer_write = 0;
    if !p.is_null() {
        let s = v.s;
        let live = unsafe { *peer.closed == 0 };
        if should_unlink_peer(unsafe { *v.sock_type }, live, unsafe { *peer.peer }, s) {
            unsafe {
                *peer.peer = ptr::null_mut();
                *peer.peer_closed = 1;
            }
            drain_peer_read = unsafe { collect(&peer, WAIT_READ, PROC_WAKE_EVENT, wake_q) };
            drain_peer_write =
                unsafe { collect(&peer, WAIT_WRITE, PROC_WAKE_EVENT, wake_q) };
        }
    }
    if !is_root {
        drain_read = unsafe { collect(v, WAIT_READ, PROC_WAKE_EXIT, wake_q) };
        drain_write = unsafe { collect(v, WAIT_WRITE, PROC_WAKE_EXIT, wake_q) };
    }

    let m = if is_root {
        unsafe { a20_net_close_rxq_detach(v) }
    } else {
        ptr::null_mut()
    };
    let mut accepted_head: *mut c_void = ptr::null_mut();
    if is_root {
        accepted_head = unsafe { *v.accept_head };
        unsafe {
            *v.accept_head = ptr::null_mut();
            *v.accept_tail = ptr::null_mut();
            *v.accept_count = 0;
        }
    }
    unsafe { a20_net_close_unlock_pair(lo, hi, flags) };

    /* The registry slot is released with no net lock held at all: unregister
     * takes a bucket lock of its own, and a bucket lock may never be nested
     * under a socket lock.  Only a socket that owned a slot owes the registry's
     * reference -- a queued AF_UNIX child owns none and carries exactly one. */
    let had_slot = unsafe { a20_net_close_unregister(v) } != 0;
    if had_slot {
        unsafe { a20_net_close_socket_free(v) };
    }
    unsafe { a20_net_close_flush(wake_q) };
    if is_root && drain_accept != 0 {
        unsafe { wake(v, WAIT_ACCEPT, PROC_WAKE_EXIT) };
    }
    if drain_read != 0 {
        unsafe { wake(v, WAIT_READ, PROC_WAKE_EXIT) };
    }
    if drain_write != 0 {
        unsafe { wake(v, WAIT_WRITE, PROC_WAKE_EXIT) };
    }
    if !p.is_null() {
        if drain_peer_read != 0 {
            unsafe { wake(&peer, WAIT_READ, PROC_WAKE_EVENT) };
        }
        if drain_peer_write != 0 {
            unsafe { wake(&peer, WAIT_WRITE, PROC_WAKE_EVENT) };
        }
        /* The reference taken above; it can free. */
        unsafe { a20_net_close_socket_free(&peer) };
    }
    if !m.is_null() {
        unsafe { a20_net_close_msg_free_chain(m) };
    }

    if !is_root {
        /* A child's lwIP side is destroyed after the peer is released, before
         * the creator's reference is dropped. */
        unsafe { a20_net_close_inet_destroy(v) };
    }
    /* The creator's reference (and, for a root, the last one). */
    unsafe { a20_net_close_socket_free(v) };
    accepted_head
}

/// Close the socket a vfile carries.  C owns the vfile and allocates the
/// wake-queue storage; this decides and performs the teardown.
#[no_mangle]
pub unsafe extern "C" fn a20_net_socket_close(root: *const CloseView,
                                              wake_q: *mut c_void) -> i32 {
    if root.is_null() || wake_q.is_null() {
        return -EINVAL;
    }
    if unsafe { (*root).s.is_null() } {
        return 0;
    }
    let mut accepted = unsafe { teardown(root, wake_q, true) };

    /* Each queued accepted child is torn down on its own: its accept list was
     * detached above, so no lock from the listener's teardown is still held and
     * the children need not share a registry bucket. */
    while !accepted.is_null() {
        let mut cv = zero_view();
        unsafe { a20_net_close_view_init(accepted, &mut cv) };
        let next = unsafe { *cv.accept_next };
        unsafe { a20_net_close_wake_q_reinit(wake_q) };
        let _ = unsafe { teardown(&cv, wake_q, false) };
        accepted = next;
    }
    unsafe { *(*root).vf_priv = ptr::null_mut() };
    0
}

/// Install a vfile for `s` into the current task's fd table.
///
/// An fd-table failure must tear the socket down before the vfile is released:
/// the socket was created for this call and owns the descriptor's lifecycle from
/// then on, and a failure that only freed the vfile would leave a half-built
/// socket with no owner.
#[no_mangle]
pub unsafe extern "C" fn a20_net_socket_install(s: *mut c_void, flags: i32,
                                                wake_q: *mut c_void) -> i32 {
    if s.is_null() {
        return -EINVAL;
    }
    let vf = unsafe { a20_net_install_prepare(s, flags) };
    if vf.is_null() {
        return -ENOMEM;
    }
    let gfd = unsafe { a20_net_install_fdtable(vf, flags) };
    if gfd < 0 {
        let mut v = zero_view();
        unsafe { a20_net_close_view_init(s, &mut v) };
        let _ = unsafe { a20_net_socket_close(&v, wake_q) };
        unsafe { a20_net_install_free(vf) };
        return gfd;
    }
    /* The socket's EventQ identity is the vfile pointer itself. */
    unsafe { a20_net_install_set_owner(s, gfd, vf) };
    gfd
}

/// Resolve the vfile observed under the current fd-table lock and pin its
/// socket before the fd-table reference is dropped, so closing or reusing the
/// numeric descriptor cannot run the close callback under the caller.
#[no_mangle]
pub unsafe extern "C" fn a20_net_socket_pin_current_fd(fd: i32,
                                                       out_vf: *mut *mut c_void,
                                                       out_sock: *mut *mut c_void) -> i32 {
    if out_vf.is_null() || out_sock.is_null() {
        return -EINVAL;
    }
    unsafe {
        *out_vf = ptr::null_mut();
        *out_sock = ptr::null_mut();
    }
    let vf = unsafe { a20_net_fdtable_current_file(fd) };
    if vf.is_null() {
        return -EBADF;
    }
    let s = unsafe { a20_net_vfile_socket(vf) };
    if s.is_null() {
        unsafe { a20_net_vfile_put(vf) };
        return -ENOTSOCK;
    }
    unsafe { a20_net_socket_ref(s) };
    unsafe {
        *out_vf = vf;
        *out_sock = s;
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn peer_unlink_keeps_a_closed_stream_peer_and_a_live_other_peer() {
        let s = 0x1000usize as *mut c_void;
        let peer = 0x2000usize as *mut c_void;
        /* A stream peer is unlinked even when it is already closed: its writers
         * still have to see the hangup. */
        assert!(should_unlink_peer(SOCK_STREAM, false, s, s));
        assert!(should_unlink_peer(SOCK_SEQPACKET, false, s, s));
        /* A datagram peer is unlinked only while it is live. */
        assert!(!should_unlink_peer(2, false, s, s));
        assert!(should_unlink_peer(2, true, s, s));
        /* Identity matters: a peer that has already rebound elsewhere is left
         * alone. */
        assert!(!should_unlink_peer(SOCK_STREAM, true, peer, s));
    }

    #[test]
    fn protocol_constants_match_net_socket_h() {
        assert_eq!(SOCK_STREAM, 1);
        assert_eq!(SOCK_SEQPACKET, 5);
        assert_eq!(WAIT_ACCEPT, 0);
        assert_eq!(WAIT_READ, 1);
        assert_eq!(WAIT_WRITE, 2);
        assert_eq!(PROC_WAKE_EVENT, 0);
        assert_eq!(PROC_WAKE_EXIT, 1);
    }
}
