#![no_std]

//! The bottom-half event ring and its per-socket consumer: the bounded ring the
//! lwIP callbacks stage inbound events into, the producer policy of the UDP, raw
//! and TCP receive callbacks, and the consumer that turns staged events into
//! queued messages with the deferred FIN/reset ordering.
//!
//! The C side keeps the primitives this module calls: the ring's storage and its
//! indices, the event fields (Rust never touches the profile-sized payload
//! area -- C copies into it and the enqueue takes the event as its meta), the
//! pbuf copy/ref/free, the socket-address and pktinfo fill from lwIP, the
//! receive-queue enqueue (inline and spill), the wait-queue collect/wake, the
//! event notify, the socket lock, the registry slot pending mark and the perf
//! counters.
//!
//! Everything that is a *decision* is here: the all-or-nothing capacity
//! reservation and the chunking of a TCP segment (staging part of it and
//! returning ERR_MEM would replay the segment on the retry and duplicate the
//! part already staged); the spill-versus-copy choice for a datagram; what a
//! full ring costs in each callback (drop, or leave the pbuf with lwIP); the
//! order in which the connected/error/closed flags are exchanged and applied --
//! the ring is drained against the *pre-existing* local close, and the peer's
//! FIN and reset are applied only afterwards, so buffered bytes reach the reader
//! before its EOF or error; the enqueue choice per event; and the wake-batch
//! fallback when the deferred wake queue fills.

use core::ffi::c_void;
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

/* Event types the consumer acts on only through the flags; the ring itself
 * carries NET_BH_RECV for staged payload. */
const A20_EVENT_READABLE: u32 = 0;
const A20_EVENT_WRITABLE: u32 = 1;
const A20_EVENT_ERROR: u32 = 2;
const A20_EVENT_CLOSED: u32 = 3;
const A20_EVENT_CONNECTION: u32 = 4;

const DRAIN_READ: u32 = 1;
const DRAIN_WRITE: u32 = 2;
const DRAIN_ACCEPT: u32 = 4;

/* PROC_WAKE_Q_CAPACITY; past it the consumer falls back to waking the queue
 * after the flush instead of batching another waiter. */
const WAKE_Q_CAPACITY: i32 = 64;

const PERF_OVERFLOW: i32 = 0;

/// Borrowed view of one event's fixed fields.
#[repr(C)]
pub struct EventView {
    pub event: *mut c_void,
    pub event_type: *mut i32,
    pub err: *mut i32,
    pub len: *mut usize,
    pub addr: *mut u8,
    pub addrlen: *mut usize,
    pub has_pktinfo: *mut u8,
    pub has_hoplimit: *mut u8,
    pub has_tclass: *mut u8,
    pub pktinfo_ifindex: *mut u32,
    pub pktinfo_addr: *mut u8,
    pub hoplimit: *mut u8,
    pub tclass: *mut u8,
    pub spill: *mut *mut c_void,
    pub spill_off: *mut u32,
}

/// Borrowed view of the socket fields the consumer transacts over.
#[repr(C)]
pub struct BhSocketView {
    pub s: *mut c_void,
    pub closed: *mut i32,
    pub peer_closed: *mut i32,
    pub tcp_connecting: *mut i32,
    pub tcp_err: *mut i32,
    pub connected: *mut i32,
    pub ever_connected: *mut i32,
    pub listening: *mut i32,
    pub reg_idx: *mut i32,
    pub bh_connected: *mut i32,
    pub bh_closed: *mut i32,
    pub bh_error: *mut i32,
    pub bh_err_code: *mut i32,
    pub bh_tx_wake: *mut i32,
    pub bh_pending: *mut i32,
}

unsafe extern "C" {
    fn a20_net_bh_event_view(event: *mut c_void, out: *mut EventView);
    fn a20_net_bh_socket_view_init(s: *mut c_void, out: *mut BhSocketView);
    fn a20_net_bh_reserve(s: *mut c_void, n: u32) -> i32;
    fn a20_net_bh_prepare(s: *mut c_void, event_out: *mut *mut c_void) -> i32;
    fn a20_net_bh_commit(s: *mut c_void);
    fn a20_net_bh_consume(s: *mut c_void) -> *mut c_void;
    fn a20_net_bh_consume_commit(s: *mut c_void);
    fn a20_net_bh_stage_payload(s: *mut c_void, event: *mut c_void, p: *mut c_void,
                                off: u32, len: usize);
    fn a20_net_bh_pbuf_len(p: *mut c_void) -> u32;
    fn a20_net_bh_pbuf_free(p: *mut c_void);
    fn a20_net_bh_addr_from_ip(event: *mut c_void, ip: *const c_void, port: u16);
    fn a20_net_bh_pktinfo(event: *mut c_void);
    fn a20_net_bh_raw_icmp_echo(s: *mut c_void, p: *mut c_void) -> i32;
    fn a20_net_bh_enqueue_spill(s: *mut c_void, event: *mut c_void) -> i32;
    fn a20_net_bh_enqueue_inline(s: *mut c_void, event: *mut c_void) -> i32;
    fn a20_net_bh_collect_all(s: *mut c_void, which: i32, wake_q: *mut c_void) -> i32;
    fn a20_net_bh_collect_one(s: *mut c_void, wake_q: *mut c_void) -> i32;
    fn a20_net_bh_wakeq_count(wake_q: *mut c_void) -> i32;
    fn a20_net_bh_notify(s: *mut c_void, event: u32, d0: u64, d1: u64);
    fn a20_net_bh_socket_lock(s: *mut c_void, flags: *mut u64);
    fn a20_net_bh_socket_unlock(s: *mut c_void, flags: u64);
    fn a20_net_bh_is_live(s: *mut c_void) -> i32;
    fn a20_net_bh_socket_ref(s: *mut c_void);
    fn a20_net_bh_socket_free(s: *mut c_void);
    fn a20_net_bh_slot_clear(s: *mut c_void);
    fn a20_net_bh_publish_pending(s: *mut c_void);
    fn a20_net_bh_perf(which: i32);
    fn a20_net_bh_accept_drain(s: *mut c_void, wake_q: *mut c_void) -> i32;
    fn a20_net_bh_wakeq_init(wake_q: *mut c_void);
    fn a20_net_bh_wakeq_flush(wake_q: *mut c_void);
    fn a20_net_bh_wake_all(s: *mut c_void, which: i32);
    fn a20_net_bh_inline_payload() -> u32;
    fn a20_net_bh_max_sockets() -> i32;
    fn a20_net_bh_accept_pending(s: *mut c_void) -> i32;
}

unsafe fn event_view(event: *mut c_void) -> EventView {
    let mut v = EventView {
        event: ptr::null_mut(),
        event_type: ptr::null_mut(),
        err: ptr::null_mut(),
        len: ptr::null_mut(),
        addr: ptr::null_mut(),
        addrlen: ptr::null_mut(),
        has_pktinfo: ptr::null_mut(),
        has_hoplimit: ptr::null_mut(),
        has_tclass: ptr::null_mut(),
        pktinfo_ifindex: ptr::null_mut(),
        pktinfo_addr: ptr::null_mut(),
        hoplimit: ptr::null_mut(),
        tclass: ptr::null_mut(),
        spill: ptr::null_mut(),
        spill_off: ptr::null_mut(),
    };
    a20_net_bh_event_view(event, &mut v);
    v
}

unsafe fn socket_view(s: *mut c_void) -> BhSocketView {
    let mut v = BhSocketView {
        s: ptr::null_mut(),
        closed: ptr::null_mut(),
        peer_closed: ptr::null_mut(),
        tcp_connecting: ptr::null_mut(),
        tcp_err: ptr::null_mut(),
        connected: ptr::null_mut(),
        ever_connected: ptr::null_mut(),
        listening: ptr::null_mut(),
        reg_idx: ptr::null_mut(),
        bh_connected: ptr::null_mut(),
        bh_closed: ptr::null_mut(),
        bh_error: ptr::null_mut(),
        bh_err_code: ptr::null_mut(),
        bh_tx_wake: ptr::null_mut(),
        bh_pending: ptr::null_mut(),
    };
    a20_net_bh_socket_view_init(s, &mut v);
    v
}

unsafe fn exchange(flag: *mut i32, order: Ordering) -> i32 {
    (*(flag as *const AtomicI32)).swap(0, order)
}

unsafe fn load(flag: *mut i32, order: Ordering) -> i32 {
    (*(flag as *const AtomicI32)).load(order)
}

/* ------------------------------------------------------------- scheduling */

/// Schedule the per-socket bottom half.  Called from lwIP callback context with
/// the lwIP lock held; the pending flag array is accessed atomically so no
/// second lock is taken here.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bh_schedule(s: *mut c_void) -> i32 {
    if s.is_null() {
        return 0;
    }
    let v = socket_view(s);
    let idx = *v.reg_idx;
    if idx < 0 || idx >= a20_net_bh_max_sockets() {
        return 0;
    }
    a20_net_bh_publish_pending(s);
    1
}

/* --------------------------------------------------------------- producer */

/// Stage a whole received TCP segment, splitting it across as many events as
/// that takes.
///
/// All or nothing, deliberately: an lwIP recv callback that returns anything
/// other than ERR_OK is telling lwIP it did *not* take the pbuf, which parks it
/// in `refused_data` and hands it back later.  Staging part of a segment and
/// then returning ERR_MEM would therefore replay the whole segment on the retry
/// and duplicate the part already staged, so the capacity for every event this
/// needs is reserved up front.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bh_stage_tcp(s: *mut c_void, p: *mut c_void) -> i32 {
    if s.is_null() || p.is_null() {
        return 0;
    }
    let total = a20_net_bh_pbuf_len(p);
    let inline_payload = a20_net_bh_inline_payload();
    let mut need = (total + inline_payload - 1) / inline_payload;
    if need == 0 {
        need = 1;
    }
    if a20_net_bh_reserve(s, need) == 0 {
        return 0;
    }
    let mut off: u32 = 0;
    while off < total {
        let mut event: *mut c_void = ptr::null_mut();
        if a20_net_bh_prepare(s, &mut event) < 0 {
            /* Unreachable after a successful reservation; keeping the check
             * means a ring that shrank under us fails closed rather than
             * overrunning. */
            return 0;
        }
        let mut n = total - off;
        if n > inline_payload {
            n = inline_payload;
        }
        a20_net_bh_stage_payload(s, event, p, off, n as usize);
        a20_net_bh_commit(s);
        off += n;
    }
    1
}

/// The UDP receive callback's staging policy: a datagram is one event, spilled
/// by reference when it is larger than the inline area, and dropped (with the
/// pbuf freed here) when the ring is full.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bh_udp_recv(s: *mut c_void, p: *mut c_void,
                                             ip: *const c_void, port: u16) {
    if s.is_null() || p.is_null() {
        return;
    }
    let mut event: *mut c_void = ptr::null_mut();
    if a20_net_bh_prepare(s, &mut event) < 0 {
        a20_net_bh_pbuf_free(p);
        return;
    }
    a20_net_bh_addr_from_ip(event, ip, port);
    a20_net_bh_pktinfo(event);
    let ev = event_view(event);
    let mut len = a20_net_bh_pbuf_len(p) as usize;
    if len > NET_MAX_PAYLOAD {
        len = NET_MAX_PAYLOAD;
    }
    a20_net_bh_stage_payload(s, event, p, 0, len);
    let _ = ev;
    a20_net_bh_commit(s);
    a20_net_bh_schedule(s);
    a20_net_bh_pbuf_free(p);
}

/// The raw receive callback's staging policy.  Returns 1 when this callback
/// consumed the pbuf and 0 when lwIP should keep it.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bh_raw_recv(s: *mut c_void, p: *mut c_void,
                                             ip: *const c_void) -> i32 {
    if s.is_null() || p.is_null() {
        return 0;
    }
    /* Do not consume ICMP echo requests on raw ICMP sockets: lwIP's icmp_input()
     * generates the echo reply, which is what the socket wants to receive. */
    if a20_net_bh_raw_icmp_echo(s, p) != 0 {
        return 0;
    }
    let mut event: *mut c_void = ptr::null_mut();
    if a20_net_bh_prepare(s, &mut event) < 0 {
        /* The ring is full, so the payload is dropped -- but this callback has
         * already taken ownership by freeing, and lwIP reads a non-zero return
         * as "I ate it" and leaves the pbuf alone.  Returning 0 here while
         * freeing made the caller free the same pbuf a second time. */
        a20_net_bh_perf(PERF_OVERFLOW);
        a20_net_bh_pbuf_free(p);
        return 1;
    }
    a20_net_bh_addr_from_ip(event, ip, 0);
    a20_net_bh_pktinfo(event);
    let mut len = a20_net_bh_pbuf_len(p) as usize;
    if len > NET_MAX_PAYLOAD {
        len = NET_MAX_PAYLOAD;
    }
    a20_net_bh_stage_payload(s, event, p, 0, len);
    a20_net_bh_commit(s);
    a20_net_bh_schedule(s);
    a20_net_bh_pbuf_free(p);
    1
}

/// The TCP receive callback's policy.  Returns 0 for ERR_OK and -1 for ERR_MEM.
/// A null pbuf is a FIN, not a teardown: the connection is still the
/// application's to read, so it is recorded as the peer's half-close.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bh_tcp_recv(s: *mut c_void, p: *mut c_void) -> i32 {
    if s.is_null() {
        return 0;
    }
    let v = socket_view(s);
    if p.is_null() {
        (*(v.bh_closed as *const AtomicI32)).store(1, Ordering::Release);
        a20_net_bh_schedule(s);
        return 0;
    }
    if a20_net_bh_stage_tcp(s, p) == 0 {
        /* The ring is full.  ERR_MEM leaves the pbuf with lwIP, which parks it
         * in refused_data and offers it again once the application has caught
         * up, so it must not be freed here. */
        return -1;
    }
    a20_net_bh_schedule(s);
    a20_net_bh_pbuf_free(p);
    0
}

/* --------------------------------------------------------------- consumer */

/// The per-socket bottom-half transaction.  Runs under the socket's own lock and
/// returns the drain bits the caller's tail must wake after flushing the
/// deferred wake queue.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bh_process_locked(s: *mut c_void,
                                                   wake_q: *mut c_void,
                                                   accept_pending: *mut i32) -> u32 {
    if s.is_null() {
        return 0;
    }
    let v = socket_view(s);
    let mut drain: u32 = 0;
    *accept_pending = a20_net_bh_accept_pending(s);

    if exchange(v.bh_connected, Ordering::Acquire) != 0 {
        let err = load(v.bh_err_code, Ordering::Relaxed);
        *v.tcp_connecting = 0;
        *v.tcp_err = err;
        if err == 0 {
            *v.connected = 1;
            *v.ever_connected = 1;
            a20_net_bh_notify(s, A20_EVENT_CONNECTION, 0, 0);
            a20_net_bh_notify(s, A20_EVENT_WRITABLE, 0, 0);
        }
        if a20_net_bh_collect_all(s, 0, wake_q) != 0 {
            drain |= DRAIN_READ;
        }
        if a20_net_bh_collect_all(s, 1, wake_q) != 0 {
            drain |= DRAIN_WRITE;
        }
    }

    /*
     * Take the peer-EOF and peer-error flags but do not act on them yet.  They
     * are applied *after* the ring has been drained, and that ordering is the
     * fix for a whole class of "the transfer never finishes": TCP delivers data
     * and the FIN that follows it in the same bottom-half pass whenever both
     * arrived together, and the ring's `if (!closed)` guard then threw away
     * bytes lwIP had already handed over.  The same happened on reset, which is
     * worse: an RST arriving behind the peer's final data set the error flag and
     * the data went with it.
     *
     * So the flags are captured here and the ring is drained against the
     * *pre-existing* local close, which is the local close the guard was written
     * for.  A reader still sees the buffered bytes first and only then the EOF
     * or the error, which is what read(2) promises.
     */
    let bh_error = exchange(v.bh_error, Ordering::Acquire);
    let bh_error_code = if bh_error != 0 {
        load(v.bh_err_code, Ordering::Relaxed)
    } else {
        0
    };
    let bh_closed = exchange(v.bh_closed, Ordering::Acquire);

    loop {
        let event = a20_net_bh_consume(s);
        if event.is_null() {
            break;
        }
        if *v.closed == 0 {
            let ev = event_view(event);
            let queued = if !(*ev.spill).is_null() {
                a20_net_bh_enqueue_spill(s, event)
            } else {
                a20_net_bh_enqueue_inline(s, event)
            };
            if queued >= 0 {
                a20_net_bh_notify(s, A20_EVENT_READABLE, 0, 0);
                if a20_net_bh_wakeq_count(wake_q) >= WAKE_Q_CAPACITY {
                    drain |= DRAIN_READ;
                } else {
                    a20_net_bh_collect_one(s, wake_q);
                }
            }
        }
        a20_net_bh_consume_commit(s);
    }

    if bh_error != 0 {
        *v.tcp_connecting = 0;
        *v.tcp_err = bh_error_code;
        *v.closed = 1;
        a20_net_bh_notify(s, A20_EVENT_ERROR, bh_error_code as u64, 0);
        a20_net_bh_notify(s, A20_EVENT_CLOSED, 0, 0);
        if a20_net_bh_collect_all(s, 0, wake_q) != 0 {
            drain |= DRAIN_READ;
        }
        if a20_net_bh_collect_all(s, 1, wake_q) != 0 {
            drain |= DRAIN_WRITE;
        }
    }

    if bh_closed != 0 {
        /* The peer sent a FIN.  That is the peer half-closing, not this socket
         * dying, so it belongs in `peer_closed` -- the flag every read path
         * already treats as end-of-file.  Setting `closed` here instead made
         * net_socket_is_live() report a healthy socket as dead, and it ran
         * before the staging loop, whose guard then discarded every payload
         * still in the ring. */
        *v.peer_closed = 1;
        a20_net_bh_notify(s, A20_EVENT_CLOSED, 0, 0);
        if a20_net_bh_collect_all(s, 0, wake_q) != 0 {
            drain |= DRAIN_READ;
        }
        if a20_net_bh_collect_all(s, 1, wake_q) != 0 {
            drain |= DRAIN_WRITE;
        }
    }

    if exchange(v.bh_tx_wake, Ordering::Acquire) != 0 {
        a20_net_bh_notify(s, A20_EVENT_WRITABLE, 0, 0);
        if a20_net_bh_collect_all(s, 1, wake_q) != 0 {
            drain |= DRAIN_WRITE;
        }
    }
    drain
}

/* -------------------------------------------------------- per-socket wrapper */

/// Finish one socket's bottom half: run the transaction under the socket lock,
/// drop the pending mark, and wake whatever the drain bits asked for after the
/// deferred queue is flushed.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bh_process_socket(s: *mut c_void) {
    if s.is_null() {
        return;
    }
    let v = socket_view(s);
    let mut wake_q = WakeQ::new();
    a20_net_bh_wakeq_init(wake_q.as_mut_ptr());
    let mut drain: u32 = 0;
    let mut accept_pending: i32 = 0;
    let mut irq = 0u64;
    a20_net_bh_socket_lock(s, &mut irq);
    if a20_net_bh_is_live(s) != 0 {
        drain = a20_net_bh_process_locked(s, wake_q.as_mut_ptr(), &mut accept_pending);
        /* The accept drain below runs with no lock held, so the socket needs a
         * reference of our own: the registry's can be handed back by a
         * concurrent close() in the window between here and there. */
        if accept_pending != 0 {
            a20_net_bh_socket_ref(s);
        }
    }
    (*(v.bh_pending as *const AtomicI32)).store(0, Ordering::Release);
    a20_net_bh_slot_clear(s);
    a20_net_bh_socket_unlock(s, irq);
    if accept_pending != 0 && a20_net_bh_accept_drain(s, wake_q.as_mut_ptr()) != 0 {
        drain |= DRAIN_ACCEPT;
    }
    if accept_pending != 0 {
        a20_net_bh_socket_free(s);
    }
    a20_net_bh_wakeq_flush(wake_q.as_mut_ptr());
    if drain & DRAIN_READ != 0 {
        a20_net_bh_wake_all(s, 0);
    }
    if drain & DRAIN_WRITE != 0 {
        a20_net_bh_wake_all(s, 1);
    }
    if drain & DRAIN_ACCEPT != 0 {
        a20_net_bh_wake_all(s, 2);
    }
}

/* --------------------------------------------------------------- helpers */

/// NET_MAX_PAYLOAD: the largest single staged payload.
const NET_MAX_PAYLOAD: usize = 65535;

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
        closed: i32,
        peer_closed: i32,
        tcp_connecting: i32,
        tcp_err: i32,
        connected: i32,
        ever_connected: i32,
        listening: i32,
        reg_idx: i32,
        bh_connected: i32,
        bh_closed: i32,
        bh_error: i32,
        bh_err_code: i32,
        bh_tx_wake: i32,
        bh_pending: i32,
        live: i32,
    }

    impl Sock {
        fn new() -> Sock {
            Sock {
                closed: 0,
                peer_closed: 0,
                tcp_connecting: 0,
                tcp_err: 0,
                connected: 0,
                ever_connected: 0,
                listening: 0,
                reg_idx: 3,
                bh_connected: 0,
                bh_closed: 0,
                bh_error: 0,
                bh_err_code: 0,
                bh_tx_wake: 0,
                bh_pending: 0,
                live: 1,
            }
        }

        fn p(&mut self) -> *mut c_void {
            self as *mut Sock as *mut c_void
        }
    }

    /// A stand-in for net_bh_event_t: the fixed fields plus a spill pointer.
    #[repr(C, align(8))]
    #[derive(Clone, Copy)]
    struct Event {
        event_type: i32,
        err: i32,
        len: usize,
        addr: [u8; 128],
        addrlen: usize,
        has_pktinfo: u8,
        has_hoplimit: u8,
        has_tclass: u8,
        pktinfo_ifindex: u32,
        pktinfo_addr: [u8; 16],
        hoplimit: u8,
        tclass: u8,
        spill: *mut c_void,
        spill_off: u32,
    }

    #[repr(C)]
    struct Pbuf {
        len: u32,
    }

    const RING: usize = 8;
    static mut T_EVENTS: [Event; RING] = [Event {
        event_type: 0,
        err: 0,
        len: 0,
        addr: [0; 128],
        addrlen: 0,
        has_pktinfo: 0,
        has_hoplimit: 0,
        has_tclass: 0,
        pktinfo_ifindex: 0,
        pktinfo_addr: [0; 16],
        hoplimit: 0,
        tclass: 0,
        spill: ptr::null_mut(),
        spill_off: 0,
    }; RING];
    static mut T_HEAD: u32 = 0;
    static mut T_TAIL: u32 = 0;
    static mut T_RESERVE: i32 = 1;
    static mut T_PREPARE_FAIL: i32 = 0;
    static mut T_STAGE_LENS: [usize; 16] = [0; 16];
    static mut T_STAGE_OFFS: [u32; 16] = [0; 16];
    static mut T_STAGE_CALLS: i32 = 0;
    static mut T_SPILL_AT: i32 = -1;
    static mut T_INLINE_PAYLOAD: u32 = 4;
    static mut T_MAX_SOCKETS: i32 = 8;
    static mut T_PUBLISH: i32 = 0;
    static mut T_FREE_CALLS: i32 = 0;
    static mut T_SCHEDULE_READY: i32 = 0;
    static mut T_PENDING_CLEARS: i32 = 0;
    static mut T_SLOT_CLEARS: i32 = 0;
    static mut T_REF_CALLS: i32 = 0;
    static mut T_SOCK_FREE_CALLS: i32 = 0;
    static mut T_ACCEPT_DRAIN: i32 = 0;
    static mut T_ACCEPT_PENDING: i32 = 0;
    static mut T_ICMP_ECHO: i32 = 0;
    static mut T_ENQ_SPILL: i32 = 0;
    static mut T_ENQ_INLINE: i32 = 0;
    static mut T_ENQ_RC: i32 = 0;
    static mut T_COLLECT_ALL_READ: i32 = 0;
    static mut T_COLLECT_ALL_WRITE: i32 = 0;
    static mut T_COLLECT_ONE: i32 = 0;
    static mut T_WAKE_COUNT: i32 = 0;
    static mut T_WAKE_ALL: [i32; 3] = [0; 3];
    static mut T_FLUSH: i32 = 0;
    static mut T_NOTIFY: [i32; 8] = [0; 8];
    static mut T_NOTIFY_N: i32 = 0;
    static mut T_PERF: [i32; 4] = [0; 4];
    static mut T_ADDR_CALLS: i32 = 0;
    static mut T_PKTINFO_CALLS: i32 = 0;
    static mut T_LOCK_CALLS: i32 = 0;
    static mut T_TRACE: [u8; 64] = [0; 64];
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
        T_EVENTS = [Event {
            event_type: 0,
            err: 0,
            len: 0,
            addr: [0; 128],
            addrlen: 0,
            has_pktinfo: 0,
            has_hoplimit: 0,
            has_tclass: 0,
            pktinfo_ifindex: 0,
            pktinfo_addr: [0; 16],
            hoplimit: 0,
            tclass: 0,
            spill: ptr::null_mut(),
            spill_off: 0,
        }; RING];
        T_HEAD = 0;
        T_TAIL = 0;
        T_RESERVE = 1;
        T_PREPARE_FAIL = 0;
        T_STAGE_LENS = [0; 16];
        T_STAGE_OFFS = [0; 16];
        T_STAGE_CALLS = 0;
        T_SPILL_AT = -1;
        T_INLINE_PAYLOAD = 4;
        T_MAX_SOCKETS = 8;
        T_PUBLISH = 0;
        T_FREE_CALLS = 0;
        T_SCHEDULE_READY = 0;
        T_PENDING_CLEARS = 0;
        T_SLOT_CLEARS = 0;
        T_REF_CALLS = 0;
        T_SOCK_FREE_CALLS = 0;
        T_ACCEPT_DRAIN = 0;
        T_ACCEPT_PENDING = 0;
        T_ICMP_ECHO = 0;
        T_ENQ_SPILL = 0;
        T_ENQ_INLINE = 0;
        T_ENQ_RC = 0;
        T_COLLECT_ALL_READ = 0;
        T_COLLECT_ALL_WRITE = 0;
        T_COLLECT_ONE = 0;
        T_WAKE_COUNT = 0;
        T_WAKE_ALL = [0; 3];
        T_FLUSH = 0;
        T_NOTIFY = [0; 8];
        T_NOTIFY_N = 0;
        T_PERF = [0; 4];
        T_ADDR_CALLS = 0;
        T_PKTINFO_CALLS = 0;
        T_LOCK_CALLS = 0;
        T_TRACE = [0; 64];
        T_TRACE_N = 0;
    }

    fn event_ptr(i: usize) -> *mut c_void {
        unsafe { ptr::addr_of_mut!(T_EVENTS[i]) as *mut c_void }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_event_view(event: *mut c_void, out: *mut EventView) {
        let e = &mut *(event as *mut Event);
        *out = EventView {
            event,
            event_type: &mut e.event_type,
            err: &mut e.err,
            len: &mut e.len,
            addr: e.addr.as_mut_ptr(),
            addrlen: &mut e.addrlen,
            has_pktinfo: &mut e.has_pktinfo,
            has_hoplimit: &mut e.has_hoplimit,
            has_tclass: &mut e.has_tclass,
            pktinfo_ifindex: &mut e.pktinfo_ifindex,
            pktinfo_addr: e.pktinfo_addr.as_mut_ptr(),
            hoplimit: &mut e.hoplimit,
            tclass: &mut e.tclass,
            spill: &mut e.spill as *mut *mut c_void,
            spill_off: &mut e.spill_off,
        };
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_socket_view_init(s: *mut c_void,
                                                     out: *mut BhSocketView) {
        let k = &mut *(s as *mut Sock);
        *out = BhSocketView {
            s,
            closed: &mut k.closed,
            peer_closed: &mut k.peer_closed,
            tcp_connecting: &mut k.tcp_connecting,
            tcp_err: &mut k.tcp_err,
            connected: &mut k.connected,
            ever_connected: &mut k.ever_connected,
            listening: &mut k.listening,
            reg_idx: &mut k.reg_idx,
            bh_connected: &mut k.bh_connected,
            bh_closed: &mut k.bh_closed,
            bh_error: &mut k.bh_error,
            bh_err_code: &mut k.bh_err_code,
            bh_tx_wake: &mut k.bh_tx_wake,
            bh_pending: &mut k.bh_pending,
        };
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_inline_payload() -> u32 {
        T_INLINE_PAYLOAD
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_max_sockets() -> i32 {
        T_MAX_SOCKETS
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_accept_pending(_s: *mut c_void) -> i32 {
        T_ACCEPT_PENDING
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_reserve(_s: *mut c_void, _n: u32) -> i32 {
        T_RESERVE
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_prepare(_s: *mut c_void,
                                            event_out: *mut *mut c_void) -> i32 {
        if T_PREPARE_FAIL != 0 || (T_HEAD - T_TAIL) as usize >= RING {
            return -1;
        }
        let idx = (T_HEAD as usize) % RING;
        T_HEAD += 1;
        *event_out = event_ptr(idx);
        idx as i32
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_commit(_s: *mut c_void) {}

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_consume(_s: *mut c_void) -> *mut c_void {
        if T_HEAD == T_TAIL {
            return ptr::null_mut();
        }
        event_ptr((T_TAIL as usize) % RING)
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_consume_commit(_s: *mut c_void) {
        T_TAIL += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_stage_payload(_s: *mut c_void, event: *mut c_void,
                                                  p: *mut c_void, off: u32,
                                                  len: usize) {
        let at = T_STAGE_CALLS as usize;
        if at < 16 {
            T_STAGE_LENS[at] = len;
            T_STAGE_OFFS[at] = off;
        }
        T_STAGE_CALLS += 1;
        let e = &mut *(event as *mut Event);
        e.len = len;
        if T_SPILL_AT >= 0 && at == T_SPILL_AT as usize {
            e.spill = p;
            e.spill_off = off;
        } else {
            e.spill = ptr::null_mut();
            e.spill_off = 0;
        }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_pbuf_len(p: *mut c_void) -> u32 {
        (*(p as *const Pbuf)).len
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_pbuf_free(_p: *mut c_void) {
        T_FREE_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_addr_from_ip(_e: *mut c_void, _ip: *const c_void,
                                                 _port: u16) {
        T_ADDR_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_pktinfo(_e: *mut c_void) {
        T_PKTINFO_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_raw_icmp_echo(_s: *mut c_void,
                                                  _p: *mut c_void) -> i32 {
        T_ICMP_ECHO
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_enqueue_spill(_s: *mut c_void,
                                                  _e: *mut c_void) -> i32 {
        T_ENQ_SPILL += 1;
        trace(b'S');
        T_ENQ_RC
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_enqueue_inline(_s: *mut c_void,
                                                   _e: *mut c_void) -> i32 {
        T_ENQ_INLINE += 1;
        trace(b'I');
        T_ENQ_RC
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_collect_all(_s: *mut c_void, which: i32,
                                                _w: *mut c_void) -> i32 {
        if which == 0 {
            T_COLLECT_ALL_READ
        } else {
            T_COLLECT_ALL_WRITE
        }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_collect_one(_s: *mut c_void, _w: *mut c_void) -> i32 {
        T_COLLECT_ONE += 1;
        0
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_wakeq_count(_w: *mut c_void) -> i32 {
        T_WAKE_COUNT
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_notify(_s: *mut c_void, event: u32, _d0: u64,
                                           _d1: u64) {
        let at = T_NOTIFY_N as usize;
        if at < 8 {
            T_NOTIFY[at] = event as i32;
        }
        T_NOTIFY_N += 1;
        trace(b'N');
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_socket_lock(_s: *mut c_void, _f: *mut u64) {
        T_LOCK_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_socket_unlock(_s: *mut c_void, _f: u64) {}

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_is_live(s: *mut c_void) -> i32 {
        (*(s as *const Sock)).live
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_socket_ref(_s: *mut c_void) {
        T_REF_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_socket_free(_s: *mut c_void) {
        T_SOCK_FREE_CALLS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_slot_clear(_s: *mut c_void) {
        T_SLOT_CLEARS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_publish_pending(s: *mut c_void) {
        T_PUBLISH += 1;
        (*(s as *mut Sock)).bh_pending = 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_perf(which: i32) {
        if which >= 0 && which < 4 {
            T_PERF[which as usize] += 1;
        }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_accept_drain(_s: *mut c_void, _w: *mut c_void) -> i32 {
        T_ACCEPT_DRAIN += 1;
        1
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_wakeq_init(_w: *mut c_void) {}

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_wakeq_flush(_w: *mut c_void) {
        T_FLUSH += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_bh_wake_all(_s: *mut c_void, which: i32) {
        if which >= 0 && which < 3 {
            T_WAKE_ALL[which as usize] += 1;
        }
    }

    /* ------------------------------------------------------ scheduling */

    #[test]
    fn schedule_publishes_only_for_a_registered_slot() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            s.reg_idx = 3;
            assert_eq!(a20_net_bh_schedule(s.p()), 1);
            assert_eq!(T_PUBLISH, 1);
            assert_eq!(s.bh_pending, 1);

            reset();
            let mut s = Sock::new();
            s.reg_idx = -1;
            assert_eq!(a20_net_bh_schedule(s.p()), 0);
            assert_eq!(T_PUBLISH, 0);

            s.reg_idx = 8; /* == max, not a slot */
            assert_eq!(a20_net_bh_schedule(s.p()), 0);
            assert_eq!(T_PUBLISH, 0);
        }
    }

    /* -------------------------------------------------------- producers */

    #[test]
    fn tcp_staging_chunks_a_segment_and_is_all_or_nothing() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            let mut p = Pbuf { len: 10 };

            /* Inline payload is 4: 10 bytes take three events, all reserved up
             * front. */
            assert_eq!(a20_net_bh_stage_tcp(s.p(), &mut p as *mut Pbuf as *mut c_void), 1);
            assert_eq!(T_STAGE_CALLS, 3);
            assert_eq!(T_STAGE_LENS[..3], [4, 4, 2]);
            assert_eq!(T_STAGE_OFFS[..3], [0, 4, 8]);

            /* A failed reservation stages nothing at all. */
            reset();
            let mut s = Sock::new();
            T_RESERVE = 0;
            assert_eq!(a20_net_bh_stage_tcp(s.p(), &mut p as *mut Pbuf as *mut c_void), 0);
            assert_eq!(T_STAGE_CALLS, 0);

            /* An empty segment reserves one event's worth of capacity but has
             * nothing to stage; it still succeeds. */
            reset();
            let mut s = Sock::new();
            let mut empty = Pbuf { len: 0 };
            assert_eq!(a20_net_bh_stage_tcp(s.p(), &mut empty as *mut Pbuf as *mut c_void), 1);
            assert_eq!(T_STAGE_CALLS, 0);
        }
    }

    #[test]
    fn udp_staging_clamps_the_length_and_drops_on_a_full_ring() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            let mut p = Pbuf { len: 70000 };
            a20_net_bh_udp_recv(s.p(), &mut p as *mut Pbuf as *mut c_void,
                                ptr::null(), 53);
            assert_eq!(T_STAGE_CALLS, 1);
            assert_eq!(T_STAGE_LENS[0], NET_MAX_PAYLOAD);
            assert_eq!((T_ADDR_CALLS, T_PKTINFO_CALLS), (1, 1));
            assert_eq!(T_FREE_CALLS, 1);
            assert_eq!(T_PUBLISH, 1);

            /* A full ring frees the pbuf and stages nothing. */
            reset();
            let mut s = Sock::new();
            T_PREPARE_FAIL = 1;
            let mut p = Pbuf { len: 10 };
            a20_net_bh_udp_recv(s.p(), &mut p as *mut Pbuf as *mut c_void,
                                ptr::null(), 53);
            assert_eq!((T_STAGE_CALLS, T_FREE_CALLS, T_PUBLISH), (0, 1, 0));
        }
    }

    #[test]
    fn raw_staging_leaves_echo_requests_and_owns_a_dropped_frame() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            let mut p = Pbuf { len: 20 };

            /* An ICMP echo request is left for lwIP's icmp_input(). */
            T_ICMP_ECHO = 1;
            assert_eq!(a20_net_bh_raw_recv(s.p(), &mut p as *mut Pbuf as *mut c_void,
                                           ptr::null()), 0);
            assert_eq!((T_STAGE_CALLS, T_FREE_CALLS), (0, 0));

            /* Full ring: the callback has taken ownership, so it frees and
             * reports "eaten" (1) -- returning 0 here would double free. */
            reset();
            let mut s = Sock::new();
            T_PREPARE_FAIL = 1;
            assert_eq!(a20_net_bh_raw_recv(s.p(), &mut p as *mut Pbuf as *mut c_void,
                                           ptr::null()), 1);
            assert_eq!(T_FREE_CALLS, 1);
            assert_eq!(T_PERF[PERF_OVERFLOW as usize], 1);

            /* Success. */
            reset();
            let mut s = Sock::new();
            assert_eq!(a20_net_bh_raw_recv(s.p(), &mut p as *mut Pbuf as *mut c_void,
                                           ptr::null()), 1);
            assert_eq!((T_STAGE_CALLS, T_FREE_CALLS, T_PUBLISH), (1, 1, 1));
        }
    }

    #[test]
    fn tcp_recv_distinguishes_fin_full_and_staged() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            /* FIN: recorded as the peer's half-close, nothing freed. */
            assert_eq!(a20_net_bh_tcp_recv(s.p(), ptr::null_mut()), 0);
            assert_eq!((s.bh_closed, T_FREE_CALLS, T_PUBLISH), (1, 0, 1));

            /* Full ring: ERR_MEM and the pbuf stays with lwIP. */
            reset();
            let mut s = Sock::new();
            T_RESERVE = 0;
            let mut p = Pbuf { len: 8 };
            assert_eq!(a20_net_bh_tcp_recv(s.p(), &mut p as *mut Pbuf as *mut c_void), -1);
            assert_eq!((T_FREE_CALLS, T_PUBLISH), (0, 0));

            /* Staged: scheduled and freed. */
            reset();
            let mut s = Sock::new();
            let mut p = Pbuf { len: 8 };
            assert_eq!(a20_net_bh_tcp_recv(s.p(), &mut p as *mut Pbuf as *mut c_void), 0);
            assert_eq!((T_FREE_CALLS, T_PUBLISH, s.bh_closed), (1, 1, 0));
        }
    }

    /* --------------------------------------------------------- consumer */

    #[test]
    fn consumer_applies_the_connection_flags_and_drain_bits() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            s.tcp_connecting = 1;
            s.bh_connected = 1;
            s.bh_err_code = 0;
            T_COLLECT_ALL_READ = 1;
            T_COLLECT_ALL_WRITE = 0;
            let mut ap = 0;
            let drain = a20_net_bh_process_locked(s.p(), ptr::null_mut(), &mut ap);
            assert_eq!((s.tcp_connecting, s.tcp_err), (0, 0));
            assert_eq!((s.connected, s.ever_connected), (1, 1));
            assert_eq!(&T_NOTIFY[..2], [A20_EVENT_CONNECTION as i32,
                                        A20_EVENT_WRITABLE as i32]);
            assert_eq!(drain, DRAIN_READ);

            /* A failed connect publishes the error code but no connection. */
            reset();
            let mut s = Sock::new();
            s.tcp_connecting = 1;
            s.bh_connected = 1;
            s.bh_err_code = -11;
            let drain = a20_net_bh_process_locked(s.p(), ptr::null_mut(), &mut ap);
            assert_eq!((s.tcp_connecting, s.tcp_err), (0, -11));
            assert_eq!((s.connected, s.ever_connected), (0, 0));
            assert_eq!(T_NOTIFY_N, 0);
            assert_eq!(drain, 0);
        }
    }

    #[test]
    fn consumer_drains_before_applying_the_peer_fin_and_error() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            s.bh_closed = 1;
            s.bh_error = 1;
            s.bh_err_code = -104;
            /* One staged inline event. */
            let mut p = Pbuf { len: 4 };
            assert_eq!(a20_net_bh_tcp_recv(s.p(), &mut p as *mut Pbuf as *mut c_void), 0);
            reset_trace_only();

            let mut pending = 0;
            let drain = a20_net_bh_process_locked(s.p(), ptr::null_mut(), &mut pending);
            /* The bytes were queued first (against the pre-existing local
             * close), and only then the reset and FIN were applied. */
            assert_eq!(&T_TRACE[..3], &[b'I', b'N', b'N']);
            assert_eq!(s.closed, 1);
            assert_eq!(s.peer_closed, 1);
            assert_eq!(s.tcp_err, -104);
            assert_eq!(&T_NOTIFY[..3], [A20_EVENT_READABLE as i32,
                                        A20_EVENT_ERROR as i32,
                                        A20_EVENT_CLOSED as i32]);
            assert_eq!(drain, 0);
        }
    }

    unsafe fn reset_trace_only() {
        T_TRACE = [0; 64];
        T_TRACE_N = 0;
    }

    #[test]
    fn consumer_skips_enqueue_for_a_locally_closed_socket() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            s.closed = 1;
            let mut p = Pbuf { len: 4 };
            assert_eq!(a20_net_bh_tcp_recv(s.p(), &mut p as *mut Pbuf as *mut c_void), 0);
            let mut pending = 0;
            let drain = a20_net_bh_process_locked(s.p(), ptr::null_mut(), &mut pending);
            assert_eq!((T_ENQ_INLINE, T_ENQ_SPILL), (0, 0));
            assert_eq!(drain, 0);
            /* The event was still consumed: the ring is drained either way. */
            assert_eq!(T_TAIL, 1);
        }
    }

    #[test]
    fn consumer_chooses_the_spill_and_inline_enqueues_per_event() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            /* Two events: the first spills, the second is inline. */
            let mut e0: *mut c_void = ptr::null_mut();
            let mut e1: *mut c_void = ptr::null_mut();
            a20_net_bh_prepare(s.p(), &mut e0);
            a20_net_bh_prepare(s.p(), &mut e1);
            (*(e0 as *mut Event)).spill = e0;
            (*(e1 as *mut Event)).spill = ptr::null_mut();
            let mut pending = 0;
            a20_net_bh_process_locked(s.p(), ptr::null_mut(), &mut pending);
            assert_eq!((T_ENQ_SPILL, T_ENQ_INLINE), (1, 1));
            assert_eq!(T_COLLECT_ONE, 2);
        }
    }

    #[test]
    fn consumer_falls_back_to_wake_all_when_the_batch_is_full() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            let mut e0: *mut c_void = ptr::null_mut();
            a20_net_bh_prepare(s.p(), &mut e0);
            T_WAKE_COUNT = WAKE_Q_CAPACITY;
            let mut pending = 0;
            let drain = a20_net_bh_process_locked(s.p(), ptr::null_mut(), &mut pending);
            assert_eq!(T_COLLECT_ONE, 0);
            assert_eq!(drain, DRAIN_READ);
        }
    }

    #[test]
    fn consumer_wakes_writers_when_the_tx_flag_is_set() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            s.bh_tx_wake = 1;
            T_COLLECT_ALL_WRITE = 1;
            let mut pending = 0;
            let drain = a20_net_bh_process_locked(s.p(), ptr::null_mut(), &mut pending);
            assert_eq!(&T_NOTIFY[..1], &[A20_EVENT_WRITABLE as i32]);
            assert_eq!(drain, DRAIN_WRITE);
        }
    }

    /* ------------------------------------------------ per-socket wrapper */

    #[test]
    fn wrapper_skips_a_dead_socket_and_always_clears_the_marks() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            s.live = 0;
            s.bh_pending = 1;
            a20_net_bh_process_socket(s.p());
            assert_eq!((s.bh_pending, T_SLOT_CLEARS, T_FLUSH), (0, 1, 1));
            assert_eq!(T_LOCK_CALLS, 1);
            assert_eq!(T_ACCEPT_DRAIN, 0);
        }
    }

    #[test]
    fn wrapper_runs_the_accept_drain_under_its_own_reference() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            T_ACCEPT_PENDING = 1;
            a20_net_bh_process_socket(s.p());
            assert_eq!((T_REF_CALLS, T_ACCEPT_DRAIN, T_SOCK_FREE_CALLS), (1, 1, 1));
            /* The accept drain reported work, so the accept queue is woken. */
            assert_eq!(T_WAKE_ALL[2], 1);
        }
    }
}
