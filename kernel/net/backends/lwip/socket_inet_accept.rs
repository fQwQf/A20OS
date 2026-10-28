#![no_std]

//! The staged-accept state machine of socket_inet.c: the ring the lwIP accept
//! callback publishes into, the bottom half's drain that turns a staged pcb into
//! a registered child on the listener's accept queue, and the purge a closing
//! listener runs over its stage.
//!
//! The C side keeps the primitives this module calls: the listener-keyed shared
//! guard the producer takes, the lwIP lock and the lane entry, the pcb callback
//! installation and abort, the pbuf walk and free, the child's socket
//! alloc/free and namespace reference, the options/peer-address inheritance, the
//! payload staging into the child's bottom-half ring, the registry
//! register/unregister, the accept-queue push, the event notify and wake-queue
//! collect, the bottom-half schedule, the perf counters and the socket lock.
//!
//! Everything that is a *decision* is here: whether the ring has room and what a
//! full ring costs; that the pcb is serviced before the slot is published (so the
//! slot address can be the pcb's callback_arg); the single-consumer claim that
//! makes the slot pointer valid across the window where the listener's lock is
//! dropped; the backlog-full refusal; the adoption condition and the order in
//! which the child is wired, registered and pushed; the rollback when the push
//! fails, including the double reference the registration added; the `dead`
//! handling that must neither adopt nor abort; the tail publication that makes a
//! slot recyclable only after the re-arg is visible; and the purge walk.

use core::ffi::c_void;
use core::ptr;
use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};

const SOCK_STREAM: i32 = 1;

/// `net_accept_stage_slot_t`, the staged-slot layout the ring is indexed by.
#[repr(C)]
struct AcceptSlot {
    listener: *mut c_void,
    pcb: *mut c_void,
    pending: *mut c_void,
    fin: u32,
    dead: u32,
    error: i32,
}

/// Borrowed view of the fields these transactions read and write.
#[repr(C)]
pub struct AcceptView {
    pub s: *mut c_void,
    pub net_ns: *mut *mut c_void,
    pub domain: *mut i32,
    pub sock_type: *mut i32,
    pub protocol: *mut i32,
    pub closed: *mut i32,
    pub listening: *mut i32,
    pub nonblock: *mut i32,
    pub bound: *mut i32,
    pub connected: *mut i32,
    pub ever_connected: *mut i32,
    pub accept_count: *mut i32,
    pub tcp_nodelay: *mut i32,
    pub tcp_congestion: *mut u8,
    pub snd_buf: *mut u32,
    pub rcv_buf: *mut u32,
    pub keepalive: *mut i32,
    pub keep_idle: *mut i32,
    pub keep_intvl: *mut i32,
    pub keep_cnt: *mut i32,
    pub recv_timeout_ticks: *mut u64,
    pub send_timeout_ticks: *mut u64,
    pub local: *mut u8,
    pub local_len: *mut usize,
    pub tcp: *mut *mut c_void,
    pub peer_closed: *mut i32,
    pub bh_closed: *mut i32,
    pub slots: *mut c_void,
    pub head: *mut u32,
    pub tail: *mut u32,
    pub dropped: *mut i32,
    pub drain_active: *mut i32,
}

unsafe extern "C" {
    fn a20_net_accept_view_init(s: *mut c_void, out: *mut AcceptView);
    fn a20_net_accept_stage_lock(s: *mut c_void) -> u64;
    fn a20_net_accept_stage_unlock(s: *mut c_void, flags: u64);
    fn a20_net_accept_lwip_lock() -> u64;
    fn a20_net_accept_lwip_unlock(flags: u64);
    fn a20_net_accept_sock_lock(s: *mut c_void, flags: *mut u64);
    fn a20_net_accept_sock_unlock(s: *mut c_void, flags: u64);
    fn a20_net_accept_is_live(s: *mut c_void) -> i32;
    fn a20_net_accept_slot_arm(slot: *mut c_void, pcb: *mut c_void);
    fn a20_net_accept_slot_release(slot: *mut c_void);
    fn a20_net_accept_alloc() -> *mut c_void;
    fn a20_net_accept_free(s: *mut c_void);
    fn a20_net_accept_ns_put(ns: *mut c_void);
    fn a20_net_accept_ns_get(ns: *mut c_void) -> *mut c_void;
    fn a20_net_accept_lane(child: *mut c_void, pcb: *mut c_void, listener: *mut c_void);
    fn a20_net_accept_apply_options(child: *mut c_void, pcb: *mut c_void);
    fn a20_net_accept_peer_addr(child: *mut c_void, pcb: *mut c_void);
    fn a20_net_accept_pbuf_next(p: *mut c_void) -> *mut c_void;
    fn a20_net_accept_pbuf_unlink(p: *mut c_void);
    fn a20_net_accept_pbuf_free(p: *mut c_void);
    fn a20_net_accept_stage_payload(child: *mut c_void, p: *mut c_void) -> i32;
    fn a20_net_accept_pcb_abort(pcb: *mut c_void);
    fn a20_net_accept_bh_closed(child: *mut c_void);
    fn a20_net_accept_register(child: *mut c_void) -> i32;
    fn a20_net_accept_unregister(child: *mut c_void) -> i32;
    fn a20_net_accept_queue_push(listener: *mut c_void, child: *mut c_void) -> i32;
    fn a20_net_accept_abort_child(child: *mut c_void);
    fn a20_net_accept_notify_accept(listener: *mut c_void, wake_q: *mut c_void,
                                    woke: *mut i32);
    fn a20_net_accept_bh_schedule(s: *mut c_void);
    fn a20_net_accept_race_delay();
    fn a20_net_accept_perf(which: i32);
}

const PERF_STAGED: i32 = 0;
const PERF_QUEUED: i32 = 1;
const PERF_DROP: i32 = 2;
const PERF_ALLOC: i32 = 3;
const PERF_OVERFLOW: i32 = 4;

fn zero_view() -> AcceptView {
    AcceptView {
        s: ptr::null_mut(),
        net_ns: ptr::null_mut(),
        domain: ptr::null_mut(),
        sock_type: ptr::null_mut(),
        protocol: ptr::null_mut(),
        closed: ptr::null_mut(),
        listening: ptr::null_mut(),
        nonblock: ptr::null_mut(),
        bound: ptr::null_mut(),
        connected: ptr::null_mut(),
        ever_connected: ptr::null_mut(),
        accept_count: ptr::null_mut(),
        tcp_nodelay: ptr::null_mut(),
        tcp_congestion: ptr::null_mut(),
        snd_buf: ptr::null_mut(),
        rcv_buf: ptr::null_mut(),
        keepalive: ptr::null_mut(),
        keep_idle: ptr::null_mut(),
        keep_intvl: ptr::null_mut(),
        keep_cnt: ptr::null_mut(),
        recv_timeout_ticks: ptr::null_mut(),
        send_timeout_ticks: ptr::null_mut(),
        local: ptr::null_mut(),
        local_len: ptr::null_mut(),
        tcp: ptr::null_mut(),
        peer_closed: ptr::null_mut(),
        bh_closed: ptr::null_mut(),
        slots: ptr::null_mut(),
        head: ptr::null_mut(),
        tail: ptr::null_mut(),
        dropped: ptr::null_mut(),
        drain_active: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> AcceptView {
    let mut v = zero_view();
    a20_net_accept_view_init(s, &mut v);
    v
}

unsafe fn slot_at(v: &AcceptView, idx: u32, stage_size: u32) -> *mut AcceptSlot {
    let slot_bytes = core::mem::size_of::<AcceptSlot>() as u32;
    ((*v).slots as *mut u8).add((idx & (stage_size - 1)) as usize * slot_bytes as usize)
        as *mut AcceptSlot
}

/* --------------------------------------------------------------- publish */

/// `lwip_tcp_accept_cb`'s publication transaction.
///
/// Returns 0 for ERR_OK (the pcb was parked) and -1 for ERR_MEM (the ring was
/// full, and the pcb has to be aborted by lwIP rather than leaked).
#[no_mangle]
pub unsafe extern "C" fn a20_net_accept_publish(listener: *mut c_void,
                                                pcb: *mut c_void,
                                                stage_size: u32) -> i32 {
    if pcb.is_null() {
        return 0;
    }
    if listener.is_null() || stage_size == 0 {
        return -1;
    }
    let v = view_of(listener);
    let sf = a20_net_accept_stage_lock(listener);

    let head = (*(v.head as *const AtomicU32)).load(Ordering::Relaxed);
    let tail = (*(v.tail as *const AtomicU32)).load(Ordering::Acquire);
    if head.wrapping_sub(tail) >= stage_size {
        let dropped = &*(v.dropped as *const AtomicI32);
        dropped.fetch_add(1, Ordering::Relaxed);
        a20_net_accept_stage_unlock(listener, sf);
        a20_net_accept_perf(PERF_DROP);
        return -1;
    }

    /* Service the pcb before publishing it: the slot address becomes the pcb's
     * callback_arg and identifies the entry for every later callback, so the
     * bottom half may not recycle the slot while the pcb can still produce an
     * event.  It hands the pcb to its child first and only then advances tail. */
    let slot = slot_at(&v, head, stage_size);
    (*slot).listener = listener;
    (*slot).pcb = pcb;
    (*slot).pending = ptr::null_mut();
    (*slot).fin = 0;
    (*slot).dead = 0;
    (*slot).error = 0;
    a20_net_accept_slot_arm(slot as *mut c_void, pcb);

    core::sync::atomic::fence(Ordering::Release);
    (*(v.head as *mut AtomicU32)).store(head.wrapping_add(1), Ordering::Relaxed);
    a20_net_accept_stage_unlock(listener, sf);
    a20_net_accept_perf(PERF_STAGED);
    a20_net_accept_bh_schedule(listener);
    0
}

/* ----------------------------------------------------------------- purge */

/// Tear down every pcb still parked in a listener's accept stage.
///
/// Runs with the lwIP lock held, which also means the producer is not running
/// and head/tail cannot move underneath this.
#[no_mangle]
pub unsafe extern "C" fn a20_net_accept_purge(listener: *mut c_void, stage_size: u32) {
    if listener.is_null() || stage_size == 0 {
        return;
    }
    let v = view_of(listener);
    let tail = (*(v.tail as *const AtomicU32)).load(Ordering::Relaxed);
    let head = (*(v.head as *const AtomicU32)).load(Ordering::Acquire);
    let mut i = tail;
    while i != head {
        let slot = slot_at(&v, i, stage_size);
        let pcb = (*slot).pcb;
        let dead = (*slot).dead != 0;
        a20_net_accept_slot_release(slot as *mut c_void);
        if !pcb.is_null() && !dead {
            a20_net_accept_pcb_abort(pcb);
        }
        i = i.wrapping_add(1);
    }
    (*(v.tail as *mut AtomicU32)).store(head, Ordering::Release);
}

/* ----------------------------------------------------------------- drain */

/// Turn a staged, already-ESTABLISHED pcb into a registered child socket on the
/// listener's accept queue.  Returns 1 when an accept waiter was woken.
#[no_mangle]
pub unsafe extern "C" fn a20_net_accept_drain(listener: *mut c_void, wake_q: *mut c_void,
                                              stage_size: u32, max_queue: i32) -> i32 {
    if listener.is_null() || stage_size == 0 {
        return 0;
    }
    let lv = view_of(listener);
    let mut woke = false;

    let mut tail;
    let head;
    {
        /* Claim the ring.  The drain has to drop the listener's lock to
         * register the children, so the single-consumer exclusion is explicit:
         * a second drain arriving while one is in flight returns, and the
         * bottom-half re-mark below picks the work up again. */
        let mut cf = 0u64;
        a20_net_accept_sock_lock(listener, &mut cf);
        if *lv.drain_active != 0 {
            a20_net_accept_sock_unlock(listener, cf);
            return 0;
        }
        *lv.drain_active = 1;
        tail = (*(lv.tail as *const AtomicU32)).load(Ordering::Relaxed);
        head = (*(lv.head as *const AtomicU32)).load(Ordering::Acquire);
        a20_net_accept_sock_unlock(listener, cf);
    }
    if tail == head {
        let mut cf = 0u64;
        a20_net_accept_sock_lock(listener, &mut cf);
        *lv.drain_active = 0;
        a20_net_accept_sock_unlock(listener, cf);
        return 0;
    }

    while tail != head {
        let slot = slot_at(&lv, tail, stage_size);

        /* Diagnostic amplifier; see net_profile.h.  It sits in the gap between
         * the dequeue and the lwIP lock acquisition, which is the window it
         * exists to widen. */
        a20_net_accept_race_delay();

        /* Everything the child inherits is read (and nothing is written) under
         * the listener's own lock, which is dropped again before the pcb
         * handoff.  The slot pointer stays valid across that drop: a staged pcb
         * is only recyclable once tail moves past it below, and no other
         * consumer drains this listener's ring. */
        let mut child: *mut c_void = ptr::null_mut();
        {
            let mut af = 0u64;
            a20_net_accept_sock_lock(listener, &mut af);
            if *lv.accept_count >= max_queue {
                /* The application is not accepting.  Refusing here is the same
                 * choice the connect path makes, and the peer sees a reset
                 * rather than a connection nobody will ever accept. */
                a20_net_accept_perf(PERF_DROP);
            } else {
                child = a20_net_accept_alloc();
                if child.is_null() {
                    a20_net_accept_perf(PERF_ALLOC);
                }
            }
            if !child.is_null() {
                let cv = view_of(child);
                /* The accepted connection inherits the listener's family.  It
                 * used to be hardcoded AF_INET, so a v6 connection that reached
                 * this drain was adopted into a v4 socket while holding a v6
                 * pcb. */
                a20_net_accept_ns_put(*cv.net_ns);
                *cv.net_ns = a20_net_accept_ns_get(*lv.net_ns);
                *cv.domain = *lv.domain;
                *cv.sock_type = SOCK_STREAM;
                *cv.protocol = *lv.protocol;
                *cv.bound = 1;
                *cv.connected = 1;
                *cv.ever_connected = 1;
                *cv.nonblock = *lv.nonblock;
                *cv.tcp_nodelay = *lv.tcp_nodelay;
                /* Linux: an accepted socket inherits the listener's congestion
                 * control and buffer ceilings, so getsockopt on the child
                 * reports what the listener asked for. */
                *cv.tcp_congestion = *lv.tcp_congestion;
                *cv.snd_buf = *lv.snd_buf;
                *cv.rcv_buf = *lv.rcv_buf;
                *cv.keepalive = *lv.keepalive;
                *cv.keep_idle = *lv.keep_idle;
                *cv.keep_intvl = *lv.keep_intvl;
                *cv.keep_cnt = *lv.keep_cnt;
                *cv.recv_timeout_ticks = *lv.recv_timeout_ticks;
                *cv.send_timeout_ticks = *lv.send_timeout_ticks;
                ptr::copy_nonoverlapping(lv.local, cv.local, *lv.local_len);
                *cv.local_len = *lv.local_len;
            }
            a20_net_accept_sock_unlock(listener, af);
        }

        /* No net lock is held here, so the short lwIP lock for the pcb handoff
         * runs on its own.  The ring's producer holds the lwIP lock and never
         * takes a net lock, so dropping the listener's lock around this window
         * cannot deadlock against it -- and it keeps the rule that the lwIP
         * lock is never held together with a net lock. */
        let cv = if child.is_null() {
            zero_view()
        } else {
            view_of(child)
        };
        let lf = a20_net_accept_lwip_lock();
        let pcb = (*slot).pcb;
        let mut adopted = false;
        let fin = (*slot).fin != 0;
        let payload = !(*slot).pending.is_null();

        a20_net_accept_lane(child, pcb, listener);

        if (*slot).dead == 0 && !pcb.is_null() && !child.is_null() {
            *cv.tcp = pcb;
            /* Re-args the pcb to the child socket, which is what makes the slot
             * recyclable: after it, no pcb can name `slot` any more. */
            a20_net_accept_apply_options(child, pcb);
            /* accept() reports the remote endpoint, which the accepted pcb
             * already carries. */
            a20_net_accept_peer_addr(child, pcb);
            /* Anything the peer sent before the handshake was adopted belongs to
             * the child now, staged rather than dropped: a client that pipelines
             * its first request into the handshake would otherwise lose it. */
            while !(*slot).pending.is_null() {
                let p = (*slot).pending;
                (*slot).pending = a20_net_accept_pbuf_next(p);
                a20_net_accept_pbuf_unlink(p);
                if a20_net_accept_stage_payload(child, p) == 0 {
                    a20_net_accept_perf(PERF_OVERFLOW);
                }
                a20_net_accept_pbuf_free(p);
            }
            if fin {
                /* The peer sent a FIN before we got here: hand the child a
                 * connection already at EOF.  `peer_closed`, not `closed`, so
                 * the staging loop above does not drop the payload the peer
                 * pipelined into the handshake. */
                *cv.peer_closed = 1;
                a20_net_accept_bh_closed(child);
            }
            adopted = true;
        } else if (*slot).dead == 0 && !pcb.is_null() {
            a20_net_accept_pcb_abort(pcb);
        } else if (*slot).dead != 0 {
            a20_net_accept_perf(PERF_DROP);
        }
        a20_net_accept_slot_release(slot as *mut c_void);
        a20_net_accept_lwip_unlock(lf);

        /* Published last, and with release semantics: the producer may reuse
         * this slot as soon as tail moves, so the re-arg above has to be
         * visible to it first. */
        (*(lv.tail as *mut AtomicU32)).store(tail.wrapping_add(1), Ordering::Release);
        tail = tail.wrapping_add(1);

        if !adopted {
            if !child.is_null() {
                a20_net_accept_free(child);
            }
            continue;
        }

        /* No net lock is held: registration takes a bucket lock of its own and
         * is documented as unsafe under a socket lock. */
        if a20_net_accept_register(child) < 0 {
            a20_net_accept_abort_child(child);
            a20_net_accept_free(child);
            a20_net_accept_perf(PERF_ALLOC);
            continue;
        }

        /* Second (and last) visit to the listener: the push and the wake.  The
         * listener is re-validated because it may have been closed during the
         * unlocked window above. */
        let mut queued = false;
        {
            let mut pf = 0u64;
            a20_net_accept_sock_lock(listener, &mut pf);
            if a20_net_accept_is_live(listener) != 0 {
                if a20_net_accept_queue_push(listener, child) >= 0 {
                    queued = true;
                    if fin || payload {
                        /* The ring and the EOF flag both landed on the child
                         * before it was registered, so nothing would otherwise
                         * ask its bottom half to look at them. */
                        a20_net_accept_bh_schedule(child);
                    }
                    let mut w = 0i32;
                    a20_net_accept_notify_accept(listener, wake_q, &mut w);
                    if w != 0 {
                        woke = true;
                    }
                }
            }
            a20_net_accept_sock_unlock(listener, pf);
        }
        if queued {
            a20_net_accept_perf(PERF_QUEUED);
            continue;
        }

        /* The child was registered a moment ago and is about to be dropped
         * again, so its slot goes back here -- with no net lock held, because
         * unregistering takes a bucket lock.  Marking it closed first keeps a
         * table scan that lands on it in this window from enqueueing against a
         * queue the application never learns about. */
        {
            let mut chf = 0u64;
            a20_net_accept_sock_lock(child, &mut chf);
            *cv.closed = 1;
            a20_net_accept_sock_unlock(child, chf);
        }
        let had_slot = a20_net_accept_unregister(child);
        a20_net_accept_abort_child(child);
        /* The creator's reference from the allocator, plus the one registration
         * added if there was a slot to release.  Both are gone because the child
         * never reached the accept queue. */
        a20_net_accept_free(child);
        if had_slot != 0 {
            a20_net_accept_free(child);
        }
        a20_net_accept_perf(PERF_DROP);
    }

    {
        let mut cf = 0u64;
        a20_net_accept_sock_lock(listener, &mut cf);
        *lv.drain_active = 0;
        a20_net_accept_sock_unlock(listener, cf);
    }
    if woke { 1 } else { 0 }
}

#[cfg(test)]
#[allow(static_mut_refs)]
mod tests {
    use super::*;

    const STAGE: u32 = 4;

    #[repr(C)]
    struct Sock {
        domain: i32,
        sock_type: i32,
        protocol: i32,
        closed: i32,
        listening: i32,
        nonblock: i32,
        bound: i32,
        connected: i32,
        ever_connected: i32,
        accept_count: i32,
        tcp_nodelay: i32,
        tcp_congestion: u8,
        snd_buf: u32,
        rcv_buf: u32,
        keepalive: i32,
        keep_idle: i32,
        keep_intvl: i32,
        keep_cnt: i32,
        recv_timeout_ticks: u64,
        send_timeout_ticks: u64,
        local: [u8; 32],
        local_len: usize,
        tcp: *mut c_void,
        peer_closed: i32,
        bh_closed: i32,
        live: i32,
        /* The view points at this slot: a temporary would leave the consumer
         * comparing a dangling value. */
        ns: *mut c_void,
    }

    impl Sock {
        fn new() -> Sock {
            Sock {
                domain: 2,
                sock_type: 1,
                protocol: 6,
                closed: 0,
                listening: 1,
                nonblock: 0,
                bound: 1,
                connected: 0,
                ever_connected: 0,
                accept_count: 0,
                tcp_nodelay: 0,
                tcp_congestion: 1,
                snd_buf: 16384,
                rcv_buf: 32768,
                keepalive: 0,
                keep_idle: 7200,
                keep_intvl: 75,
                keep_cnt: 9,
                recv_timeout_ticks: 11,
                send_timeout_ticks: 22,
                local: [7u8; 32],
                local_len: 8,
                tcp: ptr::null_mut(),
                peer_closed: 0,
                bh_closed: 0,
                live: 1,
                ns: 1usize as *mut c_void,
            }
        }

        fn p(&mut self) -> *mut c_void {
            self as *mut Sock as *mut c_void
        }
    }

    /* The real slot array is a net_accept_stage_slot_t[], so the stub must be
     * 8-byte aligned for the Rust view to dereference it. */
    static mut T_SLOTS: [u64; (STAGE as usize) * 40 / 8] = [0; (STAGE as usize) * 5];
    static mut T_HEAD: u32 = 0;
    static mut T_TAIL: u32 = 0;
    static mut T_DROPPED: i32 = 0;
    static mut T_DRAIN_ACTIVE: i32 = 0;
    static mut T_CHILD: *mut Sock = ptr::null_mut();
    static mut T_ALLOC_CALLS: i32 = 0;
    static mut T_FREE_CALLS: i32 = 0;
    static mut T_NS_PUT: i32 = 0;
    static mut T_NS_GET: i32 = 0;
    static mut T_ARM_CALLS: i32 = 0;
    static mut T_RELEASE_CALLS: i32 = 0;
    static mut T_LANE_CALLS: i32 = 0;
    static mut T_APPLY_CALLS: i32 = 0;
    static mut T_PEER_ADDR_CALLS: i32 = 0;
    static mut T_PAYLOAD_CALLS: i32 = 0;
    static mut T_PAYLOAD_RC: i32 = 1;
    static mut T_ABORT_PCB_CALLS: i32 = 0;
    static mut T_BH_CLOSED_CALLS: i32 = 0;
    static mut T_REGISTER_RC: i32 = 0;
    static mut T_REGISTER_CALLS: i32 = 0;
    static mut T_UNREGISTER_RC: i32 = 1;
    static mut T_UNREGISTER_CALLS: i32 = 0;
    static mut T_PUSH_RC: i32 = 0;
    static mut T_PUSH_CALLS: i32 = 0;
    static mut T_NOTIFY_CALLS: i32 = 0;
    static mut T_NOTIFY_WOKE: i32 = 0;
    static mut T_SCHEDULE_CALLS: i32 = 0;
    static mut T_LOCK_CALLS: i32 = 0;
    static mut T_STAGE_LOCK_CALLS: i32 = 0;
    static mut T_PERF: [i32; 8] = [0; 8];
    static mut T_PBUF_FREED: i32 = 0;

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
        T_SLOTS = [0; (STAGE as usize) * 5];
        T_HEAD = 0;
        T_TAIL = 0;
        T_DROPPED = 0;
        T_DRAIN_ACTIVE = 0;
        T_CHILD = ptr::null_mut();
        T_ALLOC_CALLS = 0;
        T_FREE_CALLS = 0;
        T_NS_PUT = 0;
        T_NS_GET = 0;
        T_ARM_CALLS = 0;
        T_RELEASE_CALLS = 0;
        T_LANE_CALLS = 0;
        T_APPLY_CALLS = 0;
        T_PEER_ADDR_CALLS = 0;
        T_PAYLOAD_CALLS = 0;
        T_PAYLOAD_RC = 1;
        T_ABORT_PCB_CALLS = 0;
        T_BH_CLOSED_CALLS = 0;
        T_REGISTER_RC = 0;
        T_REGISTER_CALLS = 0;
        T_UNREGISTER_RC = 1;
        T_UNREGISTER_CALLS = 0;
        T_PUSH_RC = 0;
        T_PUSH_CALLS = 0;
        T_NOTIFY_CALLS = 0;
        T_NOTIFY_WOKE = 0;
        T_SCHEDULE_CALLS = 0;
        T_LOCK_CALLS = 0;
        T_STAGE_LOCK_CALLS = 0;
        T_PERF = [0; 8];
        T_PBUF_FREED = 0;
    }

    /// Publish one pcb into the ring the way the callback does.
    unsafe fn publish(listener: &mut Sock, pcb: *mut c_void) -> i32 {
        a20_net_accept_publish(listener.p(), pcb, STAGE)
    }

    unsafe fn slot(i: u32) -> *mut AcceptSlot {
        (T_SLOTS.as_mut_ptr() as *mut u8).add(i as usize * 40) as *mut AcceptSlot
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_view_init(s: *mut c_void, out: *mut AcceptView) {
        let k = &mut *(s as *mut Sock);
        *out = AcceptView {
            s,
            net_ns: &mut k.ns,
            domain: &mut k.domain,
            sock_type: &mut k.sock_type,
            protocol: &mut k.protocol,
            closed: &mut k.closed,
            listening: &mut k.listening,
            nonblock: &mut k.nonblock,
            bound: &mut k.bound,
            connected: &mut k.connected,
            ever_connected: &mut k.ever_connected,
            accept_count: &mut k.accept_count,
            tcp_nodelay: &mut k.tcp_nodelay,
            tcp_congestion: &mut k.tcp_congestion,
            snd_buf: &mut k.snd_buf,
            rcv_buf: &mut k.rcv_buf,
            keepalive: &mut k.keepalive,
            keep_idle: &mut k.keep_idle,
            keep_intvl: &mut k.keep_intvl,
            keep_cnt: &mut k.keep_cnt,
            recv_timeout_ticks: &mut k.recv_timeout_ticks,
            send_timeout_ticks: &mut k.send_timeout_ticks,
            local: k.local.as_mut_ptr(),
            local_len: &mut k.local_len,
            tcp: &mut k.tcp,
            peer_closed: &mut k.peer_closed,
            bh_closed: &mut k.bh_closed,
            slots: T_SLOTS.as_mut_ptr() as *mut c_void,
            head: ptr::addr_of_mut!(T_HEAD),
            tail: ptr::addr_of_mut!(T_TAIL),
            dropped: ptr::addr_of_mut!(T_DROPPED),
            drain_active: ptr::addr_of_mut!(T_DRAIN_ACTIVE),
        };
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_stage_lock(_s: *mut c_void) -> u64 {
        T_STAGE_LOCK_CALLS += 1;
        1
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_stage_unlock(_s: *mut c_void, flags: u64) {
        assert_eq!(flags, 1);
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_sock_lock(_s: *mut c_void, _flags: *mut u64) {
        T_LOCK_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_sock_unlock(_s: *mut c_void, _flags: u64) {}

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_lwip_lock() -> u64 {
        2
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_lwip_unlock(flags: u64) {
        assert_eq!(flags, 2);
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_is_live(s: *mut c_void) -> i32 {
        (*(s as *const Sock)).live
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_slot_arm(_slot: *mut c_void, _pcb: *mut c_void) {
        T_ARM_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_slot_release(slot: *mut c_void) {
        let c = &mut *(slot as *mut AcceptSlot);
        c.pcb = ptr::null_mut();
        c.listener = ptr::null_mut();
        c.pending = ptr::null_mut();
        c.fin = 0;
        c.dead = 0;
        c.error = 0;
        T_RELEASE_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_alloc() -> *mut c_void {
        T_ALLOC_CALLS += 1;
        T_CHILD as *mut c_void
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_free(_s: *mut c_void) {
        T_FREE_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_ns_put(_ns: *mut c_void) {
        T_NS_PUT += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_ns_get(ns: *mut c_void) -> *mut c_void {
        T_NS_GET += 1;
        ns
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_lane(_c: *mut c_void, _p: *mut c_void,
                                             _l: *mut c_void) {
        T_LANE_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_apply_options(_c: *mut c_void,
                                                      _p: *mut c_void) {
        T_APPLY_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_peer_addr(_c: *mut c_void, _p: *mut c_void) {
        T_PEER_ADDR_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_pbuf_next(p: *mut c_void) -> *mut c_void {
        (*(p as *mut *mut c_void)).cast()
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_pbuf_unlink(p: *mut c_void) {
        *(p as *mut *mut c_void) = ptr::null_mut();
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_pbuf_free(_p: *mut c_void) {
        T_PBUF_FREED += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_stage_payload(_c: *mut c_void,
                                                      _p: *mut c_void) -> i32 {
        T_PAYLOAD_CALLS += 1;
        T_PAYLOAD_RC
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_pcb_abort(_pcb: *mut c_void) {
        T_ABORT_PCB_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_bh_closed(_c: *mut c_void) {
        T_BH_CLOSED_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_register(_c: *mut c_void) -> i32 {
        T_REGISTER_CALLS += 1;
        T_REGISTER_RC
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_unregister(_c: *mut c_void) -> i32 {
        T_UNREGISTER_CALLS += 1;
        T_UNREGISTER_RC
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_queue_push(_l: *mut c_void,
                                                   _c: *mut c_void) -> i32 {
        T_PUSH_CALLS += 1;
        T_PUSH_RC
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_abort_child(_c: *mut c_void) {
        T_ABORT_PCB_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_notify_accept(_l: *mut c_void,
                                                      _w: *mut c_void,
                                                      woke: *mut i32) {
        T_NOTIFY_CALLS += 1;
        *woke = T_NOTIFY_WOKE;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_bh_schedule(_s: *mut c_void) {
        T_SCHEDULE_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_race_delay() {}

    #[no_mangle]
    unsafe extern "C" fn a20_net_accept_perf(which: i32) {
        if which >= 0 && which < 8 {
            T_PERF[which as usize] += 1;
        }
    }

    /* -------------------------------------------------------- publish */

    #[test]
    fn publish_parks_the_pcb_and_advances_head() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new();
            let pcb = 0x1000usize as *mut c_void;
            assert_eq!(publish(&mut listener, pcb), 0);
            assert_eq!((T_HEAD, T_TAIL), (1, 0));
            assert_eq!((T_ARM_CALLS, T_STAGE_LOCK_CALLS, T_SCHEDULE_CALLS), (1, 1, 1));
            assert_eq!(T_PERF[PERF_STAGED as usize], 1);
            let s = slot(0);
            assert_eq!((*s).pcb, pcb);
            assert_eq!((*s).listener, listener.p());
            assert!((*s).pending.is_null());
            assert_eq!(((*s).fin, (*s).dead, (*s).error), (0, 0, 0));
        }
    }

    #[test]
    fn publish_drops_and_counts_when_the_ring_is_full() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new();
            let pcb = 0x2000usize as *mut c_void;
            for i in 0..STAGE {
                assert_eq!(publish(&mut listener, pcb), 0, "publish {}", i);
            }
            assert_eq!(T_DROPPED, 0);
            /* The ring holds STAGE entries; the next one is refused. */
            assert_eq!(publish(&mut listener, pcb), -1);
            assert_eq!((T_HEAD, T_DROPPED), (STAGE, 1));
            assert_eq!(T_PERF[PERF_DROP as usize], 1);
            /* The refused pcb is never armed, so lwIP still owns it. */
            assert_eq!(T_ARM_CALLS, STAGE as i32);
            /* A null pcb is not a drop: the callback returns ERR_OK for it. */
            assert_eq!(publish(&mut listener, ptr::null_mut()), 0);
            assert_eq!(T_DROPPED, 1);
        }
    }

    /* ---------------------------------------------------------- drain */

    #[test]
    fn drain_adopts_a_staged_pcb_into_a_registered_child() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new();
            listener.local = [7u8; 32];
            listener.local_len = 8;
            let mut child = Sock::new();
            child.sock_type = 0;
            T_CHILD = &mut child;
            let pcb = 0x3000usize as *mut c_void;
            assert_eq!(publish(&mut listener, pcb), 0);

            assert_eq!(a20_net_accept_drain(listener.p(), ptr::null_mut(), STAGE, 4), 0);
            /* The child inherited the listener's identity and options. */
            assert_eq!((child.domain, child.sock_type, child.protocol), (2, 1, 6));
            assert_eq!((child.bound, child.connected, child.ever_connected), (1, 1, 1));
            assert_eq!(child.tcp_congestion, 1);
            assert_eq!((child.snd_buf, child.rcv_buf), (16384, 32768));
            assert_eq!((child.keep_idle, child.keep_intvl, child.keep_cnt), (7200, 75, 9));
            assert_eq!((child.recv_timeout_ticks, child.send_timeout_ticks), (11, 22));
            assert_eq!(&child.local[..8], &[7u8; 8]);
            assert_eq!(child.local_len, 8);
            assert_eq!((T_NS_PUT, T_NS_GET), (1, 1));
            /* The pcb moved to the child before the slot was recycled. */
            assert_eq!(child.tcp, pcb);
            assert_eq!(T_APPLY_CALLS, 1);
            assert_eq!(T_PEER_ADDR_CALLS, 1);
            assert_eq!(T_LANE_CALLS, 1);
            assert_eq!(T_RELEASE_CALLS, 1);
            assert_eq!((T_TAIL, T_DRAIN_ACTIVE), (1, 0));
            /* Registered, pushed, woken, and the creator's reference kept. */
            assert_eq!((T_REGISTER_CALLS, T_PUSH_CALLS, T_NOTIFY_CALLS), (1, 1, 1));
            assert_eq!(T_FREE_CALLS, 0);
            assert_eq!(T_PERF[PERF_QUEUED as usize], 1);
            /* No FIN and no pipelined payload: nothing to schedule. */
            assert_eq!(T_SCHEDULE_CALLS, 1);
        }
    }

    #[test]
    fn drain_replays_a_fin_and_pipelined_payload_onto_the_child() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new();
            let mut child = Sock::new();
            T_CHILD = &mut child;
            let pcb = 0x4000usize as *mut c_void;
            assert_eq!(publish(&mut listener, pcb), 0);
            let s = slot(0);
            (*s).fin = 1;
            /* One pending pbuf, whose next pointer is the chain terminator. */
            let mut pbuf = ptr::null_mut::<c_void>();
            (*s).pending = &mut pbuf as *mut *mut c_void as *mut c_void;

            assert_eq!(a20_net_accept_drain(listener.p(), ptr::null_mut(), STAGE, 4), 0);
            assert_eq!(T_PAYLOAD_CALLS, 1);
            assert_eq!(T_PBUF_FREED, 1);
            assert_eq!(child.peer_closed, 1);
            assert_eq!(T_BH_CLOSED_CALLS, 1);
            /* The FIN and the payload both predate registration, so the child's
             * bottom half is asked to look at them. */
            assert_eq!(T_SCHEDULE_CALLS, 2);
        }
    }

    #[test]
    fn drain_refuses_when_the_backlog_is_full() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new();
            listener.accept_count = 4;
            let mut child = Sock::new();
            T_CHILD = &mut child;
            assert_eq!(publish(&mut listener, 0x5000usize as *mut c_void), 0);

            assert_eq!(a20_net_accept_drain(listener.p(), ptr::null_mut(), STAGE, 4), 0);
            /* No child was allocated, and the staged pcb was aborted rather
             * than left parked. */
            assert_eq!(T_ALLOC_CALLS, 0);
            assert_eq!(T_ABORT_PCB_CALLS, 1);
            assert_eq!(T_REGISTER_CALLS, 0);
            assert_eq!(T_PERF[PERF_DROP as usize], 1);
            assert_eq!(T_TAIL, 1);
        }
    }

    #[test]
    fn drain_rolls_the_child_back_when_the_push_fails() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new();
            let mut child = Sock::new();
            T_CHILD = &mut child;
            T_PUSH_RC = -1;
            assert_eq!(publish(&mut listener, 0x6000usize as *mut c_void), 0);

            assert_eq!(a20_net_accept_drain(listener.p(), ptr::null_mut(), STAGE, 4), 0);
            assert_eq!(child.closed, 1);
            /* The registration added a reference and a slot; both go back. */
            assert_eq!((T_REGISTER_CALLS, T_UNREGISTER_CALLS), (1, 1));
            assert_eq!(T_ABORT_PCB_CALLS, 1);
            assert_eq!(T_FREE_CALLS, 2);
            assert_eq!(T_PERF[PERF_DROP as usize], 1);
        }
    }

    #[test]
    fn drain_rolls_back_registration_failure_without_a_slot() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new();
            let mut child = Sock::new();
            T_CHILD = &mut child;
            T_REGISTER_RC = -1;
            assert_eq!(publish(&mut listener, 0x7000usize as *mut c_void), 0);

            assert_eq!(a20_net_accept_drain(listener.p(), ptr::null_mut(), STAGE, 4), 0);
            /* No registration happened, so the child carries only the creator's
             * reference and is freed once. */
            assert_eq!(T_UNREGISTER_CALLS, 0);
            assert_eq!(T_FREE_CALLS, 1);
            assert_eq!(T_PUSH_CALLS, 0);
            assert_eq!(T_PERF[PERF_ALLOC as usize], 1);
        }
    }

    #[test]
    fn drain_neither_adopts_nor_aborts_a_dead_pcb() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new();
            let mut child = Sock::new();
            T_CHILD = &mut child;
            assert_eq!(publish(&mut listener, 0x8000usize as *mut c_void), 0);
            slot(0).as_mut().unwrap().dead = 1;

            assert_eq!(a20_net_accept_drain(listener.p(), ptr::null_mut(), STAGE, 4), 0);
            /* lwIP already freed the pcb, so touching it would be the double
             * free this path exists to avoid. */
            assert_eq!(T_ABORT_PCB_CALLS, 0);
            assert_eq!(T_APPLY_CALLS, 0);
            assert_eq!(T_FREE_CALLS, 1);
            assert_eq!(T_PERF[PERF_DROP as usize], 1);
        }
    }

    #[test]
    fn drain_wakes_the_accept_waiter_and_reports_it() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new();
            let mut child = Sock::new();
            T_CHILD = &mut child;
            T_NOTIFY_WOKE = 1;
            assert_eq!(publish(&mut listener, 0x9000usize as *mut c_void), 0);
            assert_eq!(a20_net_accept_drain(listener.p(), ptr::null_mut(), STAGE, 4), 1);

            /* A listener that died between the two visits drops the child. */
            reset();
            let mut listener = Sock::new();
            let mut child = Sock::new();
            T_CHILD = &mut child;
            listener.live = 0;
            assert_eq!(publish(&mut listener, 0xa000usize as *mut c_void), 0);
            assert_eq!(a20_net_accept_drain(listener.p(), ptr::null_mut(), STAGE, 4), 0);
            assert_eq!(T_PUSH_CALLS, 0);
            assert_eq!(T_UNREGISTER_CALLS, 1);
            assert_eq!(T_FREE_CALLS, 2);
            assert_eq!(T_PERF[PERF_DROP as usize], 1);
        }
    }

    #[test]
    fn drain_is_single_consumer_and_an_empty_ring_is_a_no_op() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new();
            /* Empty: no claim is left behind. */
            assert_eq!(a20_net_accept_drain(listener.p(), ptr::null_mut(), STAGE, 4), 0);
            assert_eq!((T_DRAIN_ACTIVE, T_LOCK_CALLS), (0, 2));

            /* A second drain arriving while one is in flight returns at once
             * and leaves the ring for the one that owns it. */
            reset();
            let mut listener = Sock::new();
            let mut child = Sock::new();
            T_CHILD = &mut child;
            T_DRAIN_ACTIVE = 1;
            assert_eq!(publish(&mut listener, 0xb000usize as *mut c_void), 0);
            assert_eq!(a20_net_accept_drain(listener.p(), ptr::null_mut(), STAGE, 4), 0);
            assert_eq!((T_TAIL, T_ALLOC_CALLS), (0, 0));
            assert_eq!(T_DRAIN_ACTIVE, 1);
        }
    }

    /* ---------------------------------------------------------- purge */

    #[test]
    fn purge_releases_and_aborts_only_live_pcbs() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new();
            assert_eq!(publish(&mut listener, 0x1000usize as *mut c_void), 0);
            assert_eq!(publish(&mut listener, 0x2000usize as *mut c_void), 0);
            assert_eq!(publish(&mut listener, 0x3000usize as *mut c_void), 0);
            slot(1).as_mut().unwrap().dead = 1;

            a20_net_accept_purge(listener.p(), STAGE);
            /* Two live pcbs aborted, the dead one left alone; every slot
             * released and the tail moved to head. */
            assert_eq!(T_ABORT_PCB_CALLS, 2);
            assert_eq!(T_RELEASE_CALLS, 3);
            assert_eq!((T_TAIL, T_HEAD), (3, 3));

            /* Idempotent on an empty stage. */
            a20_net_accept_purge(listener.p(), STAGE);
            assert_eq!(T_ABORT_PCB_CALLS, 2);
        }
    }

    #[test]
    fn purge_frees_a_pending_chain_through_the_slot_release() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new();
            assert_eq!(publish(&mut listener, 0x1000usize as *mut c_void), 0);
            let mut pbuf = ptr::null_mut::<c_void>();
            slot(0).as_mut().unwrap().pending = &mut pbuf as *mut *mut c_void as *mut c_void;
            a20_net_accept_purge(listener.p(), STAGE);
            assert_eq!(T_RELEASE_CALLS, 1);
            assert!(slot(0).as_ref().unwrap().pending.is_null());
        }
    }
}
