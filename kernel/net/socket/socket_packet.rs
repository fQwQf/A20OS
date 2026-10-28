#![no_std]

//! AF_PACKET: the capture ring protocol, the bound-socket census, the delivery
//! filter and the bind and send transactions.
//!
//! The C side keeps the primitives this module calls: the ring spinlock and the
//! ring's storage (the slot array and the single drain slot, whose profile-sized
//! .bss footprint `/proc/a20/netmem` reports), the socket lock and the bucket
//! lock, the registry slot lookup, the liveness test, the initial-namespace
//! test, the receive-queue enqueue and the wakeup collect, the wake-queue
//! init/flush, and the lwIP interface queries and transmit op.
//!
//! Everything that is a *decision* is here: whether a frame is captured at all
//! (the census) and how much of it is kept; whether the ring has room, which
//! slot the frame lands in, when `pending` is published and cleared, and when a
//! full ring is a drop; which sockets a captured frame matches and what payload
//! and ancillary address each of them gets; and the whole bind filter
//! publication and send frame construction.
//!
//! The ring is *described* rather than owned: C passes a `net_packet_ring_t`
//! with the storage and the indices, so the profile's static accounting keeps
//! describing the real layout while every index decision is Rust's.

use core::ffi::c_void;
use core::ptr;
use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};

const AF_PACKET: i32 = 17;
const SOCK_RAW: i32 = 3;

const ETH_HLEN: usize = 14;
const ETH_ALEN: usize = 6;
const ETH_P_ALL: u16 = 0x0003;
const ARPHRD_ETHER: u16 = 1;
const PACKET_HOST: u8 = 0;

/* sizeof(net_sockaddr_ll_t): 2 + 2 + 4 + 2 + 1 + 1 + 8, 4-byte aligned. */
const SOCKADDR_LL_SIZE: usize = 20;

/* Largest per-profile packet frame (NET_PROFILE_PACKET_FRAME_SIZE is 1536 at
 * its largest rung); the effective limit is passed in and asserted in C. */
const FRAME_BUF: usize = 1536;

const EIO: i32 = 5;
const ENODEV: i32 = 19;
const EINVAL: i32 = 22;
const ENOTSOCK: i32 = 88;
const EDESTADDRREQ: i32 = 89;
const EMSGSIZE: i32 = 90;
const EAFNOSUPPORT: i32 = 97;

/// Borrowed view of the AF_PACKET fields these transactions read and write.
#[repr(C)]
pub struct PacketView {
    pub s: *mut c_void,
    pub net_ns: *mut c_void,
    pub domain: *mut i32,
    pub sock_type: *mut i32,
    pub protocol: *mut i32,
    pub closed: *mut i32,
    pub rx_count: *mut i32,
    pub bound: *mut i32,
    pub local: *mut u8,
    pub local_len: *mut usize,
    pub pkt_ifindex: *mut i32,
    pub pkt_protocol: *mut u16,
    pub pkt_hatype: *mut u16,
    pub pkt_halen: *mut u8,
    pub pkt_haddr: *mut u8,
    pub pkt_bound: *mut i32,
    pub pkt_bound_marked: *mut i32,
}

/// The capture ring, described by C: storage, indices and the two constants the
/// delivery walk needs.
#[repr(C)]
pub struct PacketRing {
    pub head: *mut u32,
    pub tail: *mut u32,
    pub pending: *mut i32,
    pub drops: *mut u32,
    pub slots: *mut u8,
    pub drain: *mut u8,
    pub depth: u32,
    pub slot_bytes: u32,
    /// The profile's NET_MAX_QUEUE: the delivery walk refuses a socket whose
    /// receive queue is already at the cap, exactly as the C version did.
    pub max_queue: i32,
}

/// `net_sockaddr_ll_t`, the ancillary address an AF_PACKET receive delivers.
#[repr(C)]
struct SockaddrLl {
    family: u16,
    protocol: u16,
    ifindex: i32,
    hatype: u16,
    pkttype: u8,
    halen: u8,
    addr: [u8; 8],
}

unsafe extern "C" {
    fn a20_net_packet_view_init(s: *mut c_void, out: *mut PacketView);
    fn a20_net_packet_ring_lock(flags: *mut u64);
    fn a20_net_packet_ring_unlock(flags: u64);
    fn a20_net_packet_ns_initial(ns: *mut c_void) -> i32;
    fn a20_net_packet_buckets() -> i32;
    fn a20_net_packet_slots_per_bucket() -> i32;
    fn a20_net_packet_bucket_lock(bucket: i32, flags: *mut u64);
    fn a20_net_packet_bucket_unlock(bucket: i32, flags: u64);
    fn a20_net_packet_slot_socket(bucket: i32, i: i32) -> *mut c_void;
    fn a20_net_packet_lock(s: *mut c_void, flags: *mut u64);
    fn a20_net_packet_unlock(s: *mut c_void, flags: u64);
    fn a20_net_packet_is_live(s: *mut c_void) -> i32;
    fn a20_net_packet_enqueue(s: *mut c_void, payload: *const u8, len: usize,
                              ll: *const c_void, lllen: usize) -> i32;
    fn a20_net_packet_collect_read(s: *mut c_void, wake_q: *mut c_void) -> i32;
    fn a20_net_packet_wakeq_init(wake_q: *mut c_void);
    fn a20_net_packet_wakeq_flush(wake_q: *mut c_void);
    fn a20_net_packet_if_up(ifindex: i32) -> i32;
    fn a20_net_packet_if_default_index() -> i32;
    fn a20_net_packet_if_hwaddr(ifindex: i32, out: *mut u8) -> i32;
    fn a20_net_packet_tx(ifindex: i32, frame: *const u8, len: usize) -> i32;
}

fn zero_view() -> PacketView {
    PacketView {
        s: ptr::null_mut(),
        net_ns: ptr::null_mut(),
        domain: ptr::null_mut(),
        sock_type: ptr::null_mut(),
        protocol: ptr::null_mut(),
        closed: ptr::null_mut(),
        rx_count: ptr::null_mut(),
        bound: ptr::null_mut(),
        local: ptr::null_mut(),
        local_len: ptr::null_mut(),
        pkt_ifindex: ptr::null_mut(),
        pkt_protocol: ptr::null_mut(),
        pkt_hatype: ptr::null_mut(),
        pkt_halen: ptr::null_mut(),
        pkt_haddr: ptr::null_mut(),
        pkt_bound: ptr::null_mut(),
        pkt_bound_marked: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> PacketView {
    let mut v = zero_view();
    a20_net_packet_view_init(s, &mut v);
    v
}

/// Slot layout: `{ uint16_t len; uint16_t ifindex; uint8_t frame[]; }`.
unsafe fn slot_len(slot: *const u8) -> usize {
    ptr::read_unaligned(slot as *const u16) as usize
}

unsafe fn slot_ifindex(slot: *const u8) -> u16 {
    ptr::read_unaligned(slot.add(2) as *const u16)
}

unsafe fn slot_frame(slot: *const u8) -> *const u8 {
    slot.add(4)
}

/* ------------------------------------------------------------------ census */

/*
 * Number of AF_PACKET sockets currently holding a bind filter.  The receive
 * path reads it with the lwIP lock held and is the only lock-free reader, so it
 * is published with a release and sampled with an acquire: a bind that has
 * returned is visible to every later receive.  A receive that raced a close
 * either captured a frame whose socket is now gone -- delivered to nobody, the
 * same outcome as the drop it replaces -- or missed a frame that arrived before
 * the bind completed.  Neither loses a frame the socket was entitled to.
 */
static PKT_BOUND_COUNT: AtomicI32 = AtomicI32::new(0);

#[no_mangle]
pub unsafe extern "C" fn a20_net_packet_bound_count() -> i32 {
    PKT_BOUND_COUNT.load(Ordering::Acquire)
}

/*
 * Both of these run under the socket's own lock, so the idempotence marker needs
 * no lock of its own.  It exists to keep the count balanced when the release
 * side cannot tell whether the acquire side ran.
 */
#[no_mangle]
pub unsafe extern "C" fn a20_net_packet_bound_acquire(v: *const PacketView) {
    if v.is_null() {
        return;
    }
    let marked = &mut *(*v).pkt_bound_marked;
    if *marked != 0 {
        return;
    }
    *marked = 1;
    PKT_BOUND_COUNT.fetch_add(1, Ordering::AcqRel);
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_packet_bound_release(v: *const PacketView) {
    if v.is_null() {
        return;
    }
    let marked = &mut *(*v).pkt_bound_marked;
    if *marked == 0 {
        return;
    }
    *marked = 0;
    PKT_BOUND_COUNT.fetch_sub(1, Ordering::AcqRel);
}

/* --------------------------------------------------------------- capture */

#[no_mangle]
pub unsafe extern "C" fn a20_net_packet_rx_pending(r: *const PacketRing) -> i32 {
    if r.is_null() {
        return 0;
    }
    let pending = &*((*r).pending as *const AtomicI32);
    if pending.load(Ordering::Acquire) != 0 { 1 } else { 0 }
}

/*
 * Capture one frame into the ring.  The census gate and the length clamp are
 * here; the bounded ring's full/empty handling and the index arithmetic are
 * here too, while the slot array, the drain slot and the spinlock stay in C.
 */
#[no_mangle]
pub unsafe extern "C" fn a20_net_packet_rx_defer(r: *const PacketRing,
                                                 ifindex: u32,
                                                 frame: *const u8,
                                                 len: usize) {
    if r.is_null() || frame.is_null() || len == 0 {
        return;
    }
    if a20_net_packet_bound_count() == 0 {
        return;
    }
    let slot_bytes = (*r).slot_bytes as usize;
    let frame_max = slot_bytes - 4;
    let n = if len > frame_max { frame_max } else { len };

    let mut flags = 0u64;
    a20_net_packet_ring_lock(&mut flags);
    let head = ptr::read((*r).head);
    let tail = ptr::read((*r).tail);
    let next = (head + 1) % (*r).depth;
    if next == tail {
        let drops = &*((*r).drops as *const AtomicU32);
        drops.fetch_add(1, Ordering::Relaxed);
        a20_net_packet_ring_unlock(flags);
        return;
    }
    let slot = (*r).slots.add((head as usize) * slot_bytes);
    ptr::write_unaligned(slot as *mut u16, n as u16);
    ptr::write_unaligned(slot.add(2) as *mut u16, ifindex as u16);
    ptr::copy_nonoverlapping(frame, slot.add(4), n);
    ptr::write((*r).head, next);
    let pending = &*((*r).pending as *const AtomicI32);
    pending.store(1, Ordering::Release);
    a20_net_packet_ring_unlock(flags);
}

/*
 * Deliver one captured frame to every AF_PACKET socket it matches.
 *
 * The table is walked one bucket at a time: taking the whole set of shard locks
 * with interrupts disabled is the livelock fs/vfs/dcache.c records, and a frame
 * delivery that livelocks the interrupt trying to deliver the next frame is the
 * worst possible place to find out.  Each bucket is taken, drained and released
 * before the next is taken, so at most one shard lock is ever held.
 */
unsafe fn deliver(r: *const PacketRing, slot: *const u8, wake_q: *mut c_void) {
    let len = slot_len(slot);
    if len < ETH_HLEN {
        return;
    }
    /* The ring can still hold frames captured for a socket that has since
     * closed, so the census is re-read here rather than trusted from the
     * capture side. */
    if a20_net_packet_bound_count() == 0 {
        return;
    }
    let frame = slot_frame(slot);
    let ethertype = ((*frame.add(12) as u16) << 8) | (*frame.add(13) as u16);
    let ifindex = slot_ifindex(slot) as u32;
    let max_queue = (*r).max_queue;

    let buckets = a20_net_packet_buckets();
    let per_bucket = a20_net_packet_slots_per_bucket();
    for bucket in 0..buckets {
        let mut bf = 0u64;
        a20_net_packet_bucket_lock(bucket, &mut bf);
        for i in 0..per_bucket {
            let sp = a20_net_packet_slot_socket(bucket, i);
            if sp.is_null() {
                continue;
            }
            /* Bucket outer, socket inner: the same nesting net_bucket_scan()
             * does, and the only order a bucket lock is ever taken in. */
            let v = view_of(sp);
            let mut sf = 0u64;
            a20_net_packet_lock(sp, &mut sf);
            let skip = a20_net_packet_ns_initial(v.net_ns) == 0
                || *v.domain != AF_PACKET
                || *v.closed != 0
                || *v.pkt_bound == 0
                || (*v.pkt_ifindex > 0 && *v.pkt_ifindex as u32 != ifindex)
                || (*v.pkt_protocol != ETH_P_ALL && *v.pkt_protocol != ethertype)
                || *v.rx_count >= max_queue;
            if !skip {
                let mut ll = SockaddrLl {
                    family: AF_PACKET as u16,
                    protocol: ethertype.swap_bytes(),
                    ifindex: ifindex as i32,
                    hatype: ARPHRD_ETHER,
                    pkttype: PACKET_HOST,
                    halen: ETH_ALEN as u8,
                    addr: [0u8; 8],
                };
                ptr::copy_nonoverlapping(frame.add(ETH_ALEN), ll.addr.as_mut_ptr(),
                                         ETH_ALEN);

                let raw = *v.sock_type == SOCK_RAW;
                let payload = if raw { frame } else { frame.add(ETH_HLEN) };
                let payload_len = if raw { len } else { len - ETH_HLEN };
                if a20_net_packet_enqueue(sp, payload, payload_len,
                                          &ll as *const SockaddrLl as *const c_void,
                                          SOCKADDR_LL_SIZE) >= 0 {
                    a20_net_packet_collect_read(sp, wake_q);
                }
            }
            a20_net_packet_unlock(sp, sf);
        }
        a20_net_packet_bucket_unlock(bucket, bf);
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_packet_bottom_half_process(r: *const PacketRing) {
    if r.is_null() {
        return;
    }
    if a20_net_packet_rx_pending(r) == 0 {
        return;
    }

    let mut wake_q = WakeQ::new();
    a20_net_packet_wakeq_init(wake_q.as_mut_ptr());
    let mut delivered = 0;

    loop {
        let mut flags = 0u64;
        a20_net_packet_ring_lock(&mut flags);
        let head = ptr::read((*r).head);
        let tail = ptr::read((*r).tail);
        if tail == head {
            let pending = &*((*r).pending as *const AtomicI32);
            pending.store(0, Ordering::Release);
            a20_net_packet_ring_unlock(flags);
            break;
        }
        let slot_bytes = (*r).slot_bytes as usize;
        let src = (*r).slots.add((tail as usize) * slot_bytes);
        ptr::copy_nonoverlapping(src, (*r).drain, slot_bytes);
        ptr::write((*r).tail, (tail + 1) % (*r).depth);
        a20_net_packet_ring_unlock(flags);

        deliver(r, (*r).drain, wake_q.as_mut_ptr());
        delivered += 1;
    }
    if delivered != 0 {
        a20_net_packet_wakeq_flush(wake_q.as_mut_ptr());
    }
}

/* -------------------------------------------------------------------- bind */

#[no_mangle]
pub unsafe extern "C" fn a20_net_packet_socket_bind(s: *mut c_void,
                                                    addr: *const c_void,
                                                    addrlen: usize) -> i32 {
    let v = view_of(s);
    if a20_net_packet_ns_initial(v.net_ns) == 0 {
        return -ENODEV;
    }
    if s.is_null() || *v.domain != AF_PACKET {
        return -EAFNOSUPPORT;
    }
    if addr.is_null() || addrlen < SOCKADDR_LL_SIZE {
        return -EINVAL;
    }
    let ll = addr as *const SockaddrLl;
    if ptr::read_unaligned(ptr::addr_of!((*ll).family)) != AF_PACKET as u16 {
        return -EAFNOSUPPORT;
    }
    let ifindex = ptr::read_unaligned(ptr::addr_of!((*ll).ifindex));
    if ifindex < 0 {
        return -EINVAL;
    }
    if ifindex > 0 && a20_net_packet_if_up(ifindex) < 0 {
        return -ENODEV;
    }

    let ll_proto = ptr::read_unaligned(ptr::addr_of!((*ll).protocol));
    let mut proto = if ll_proto != 0 {
        ll_proto.swap_bytes()
    } else {
        (*v.protocol as u16).swap_bytes()
    };
    if proto == 0 {
        proto = ETH_P_ALL;
    }

    let mut flags = 0u64;
    a20_net_packet_lock(s, &mut flags);
    if a20_net_packet_is_live(s) == 0 {
        a20_net_packet_unlock(s, flags);
        return -ENOTSOCK;
    }
    *v.pkt_ifindex = ifindex;
    *v.pkt_protocol = proto;
    *v.pkt_hatype = ARPHRD_ETHER;
    let halen = ptr::read_unaligned(ptr::addr_of!((*ll).halen));
    *v.pkt_halen = if halen as usize > 8 { ETH_ALEN as u8 } else { halen };
    ptr::copy_nonoverlapping(ptr::addr_of!((*ll).addr) as *const u8, v.pkt_haddr, 8);
    *v.pkt_bound = 1;
    /* Published last, under the same lock the release side takes, so a bind
     * that returns is always counted and a rebind cannot double-count. */
    a20_net_packet_bound_acquire(&v);
    ptr::copy_nonoverlapping(addr as *const u8, v.local, addrlen);
    *v.local_len = addrlen;
    *v.bound = 1;
    a20_net_packet_unlock(s, flags);
    0
}

/* -------------------------------------------------------------------- send */

#[no_mangle]
pub unsafe extern "C" fn a20_net_packet_socket_send(s: *mut c_void, buf: *const c_void,
                                                    len: usize, addr: *const c_void,
                                                    addrlen: usize,
                                                    frame_max: usize) -> i32 {
    let v = view_of(s);
    if a20_net_packet_ns_initial(v.net_ns) == 0 {
        return -ENODEV;
    }
    if s.is_null() || *v.domain != AF_PACKET {
        return -EAFNOSUPPORT;
    }
    if buf.is_null() || len == 0 {
        return -EINVAL;
    }
    if *v.pkt_bound == 0 {
        return -EDESTADDRREQ;
    }
    let frame_max = if frame_max > FRAME_BUF { FRAME_BUF } else { frame_max };

    let mut ll: *const SockaddrLl = ptr::null();
    let mut ifindex = *v.pkt_ifindex;
    if !addr.is_null() && addrlen >= SOCKADDR_LL_SIZE {
        ll = addr as *const SockaddrLl;
        if ptr::read_unaligned(ptr::addr_of!((*ll).family)) != AF_PACKET as u16 {
            return -EAFNOSUPPORT;
        }
        let ll_ifindex = ptr::read_unaligned(ptr::addr_of!((*ll).ifindex));
        if ll_ifindex > 0 {
            ifindex = ll_ifindex;
        }
    }
    if ifindex <= 0 {
        ifindex = a20_net_packet_if_default_index();
    }
    if ifindex <= 0 {
        return -ENODEV;
    }

    let mut frame = [0u8; FRAME_BUF];
    let frame_len: usize;

    if *v.sock_type == SOCK_RAW {
        if len > frame_max {
            return -EMSGSIZE;
        }
        ptr::copy_nonoverlapping(buf as *const u8, frame.as_mut_ptr(), len);
        frame_len = len;
    } else {
        if len + ETH_HLEN > frame_max {
            return -EMSGSIZE;
        }

        let mut src = [0u8; 8];
        if a20_net_packet_if_hwaddr(ifindex, src.as_mut_ptr()) < 0 {
            return -ENODEV;
        }

        let mut dst = [0xffu8; ETH_ALEN];
        if !ll.is_null()
            && ptr::read_unaligned(ptr::addr_of!((*ll).halen)) as usize == ETH_ALEN
        {
            ptr::copy_nonoverlapping(ptr::addr_of!((*ll).addr) as *const u8,
                                     dst.as_mut_ptr(), ETH_ALEN);
        } else if *v.pkt_halen as usize == ETH_ALEN {
            ptr::copy_nonoverlapping(v.pkt_haddr, dst.as_mut_ptr(), ETH_ALEN);
        }

        let mut proto = *v.pkt_protocol;
        if !ll.is_null() {
            let ll_proto = ptr::read_unaligned(ptr::addr_of!((*ll).protocol));
            if ll_proto != 0 {
                proto = ll_proto.swap_bytes();
            }
        }

        ptr::copy_nonoverlapping(dst.as_ptr(), frame.as_mut_ptr(), ETH_ALEN);
        ptr::copy_nonoverlapping(src.as_ptr(), frame.as_mut_ptr().add(ETH_ALEN),
                                 ETH_ALEN);
        frame[12] = (proto >> 8) as u8;
        frame[13] = (proto & 0xff) as u8;
        ptr::copy_nonoverlapping(buf as *const u8, frame.as_mut_ptr().add(ETH_HLEN),
                                 len);
        frame_len = len + ETH_HLEN;
    }

    if a20_net_packet_tx(ifindex, frame.as_ptr(), frame_len) < 0 {
        return -EIO;
    }
    len as i32
}

/* ------------------------------------------------------------- wake queue */

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
        closed: i32,
        rx_count: i32,
        bound: i32,
        local: [u8; 128],
        local_len: usize,
        pkt_ifindex: i32,
        pkt_protocol: u16,
        pkt_hatype: u16,
        pkt_halen: u8,
        pkt_haddr: [u8; 8],
        pkt_bound: i32,
        pkt_bound_marked: i32,
        live: i32,
        ns: usize,
    }

    impl Sock {
        fn new() -> Sock {
            Sock {
                domain: AF_PACKET,
                sock_type: SOCK_RAW,
                protocol: 0,
                closed: 0,
                rx_count: 0,
                bound: 0,
                local: [0; 128],
                local_len: 0,
                pkt_ifindex: 0,
                pkt_protocol: 0,
                pkt_hatype: 0,
                pkt_halen: 0,
                pkt_haddr: [0; 8],
                pkt_bound: 0,
                pkt_bound_marked: 0,
                live: 1,
                ns: 1,
            }
        }

        fn p(&mut self) -> *mut c_void {
            self as *mut Sock as *mut c_void
        }
    }

    const RING_DEPTH: u32 = 4;
    const SLOT_BYTES: u32 = 4 + 64;
    const BUCKETS: i32 = 2;
    const PER_BUCKET: i32 = 4;

    static mut T_HEAD: u32 = 0;
    static mut T_TAIL: u32 = 0;
    static mut T_PENDING: i32 = 0;
    static mut T_DROPS: u32 = 0;
    static mut T_SLOTS: [u8; (RING_DEPTH * SLOT_BYTES) as usize] = [0; 272];
    static mut T_DRAIN: [u8; SLOT_BYTES as usize] = [0; 68];
    static mut T_SOCKETS: [*mut Sock; 8] = [ptr::null_mut(); 8];
    static mut T_NS_INITIAL: i32 = 1;
    static mut T_IF_UP: i32 = 0;
    static mut T_DEFAULT_IF: i32 = 1;
    static mut T_HWADDR: [u8; 8] = [0; 8];
    static mut T_HWADDR_RC: i32 = 0;
    static mut T_TX_RC: i32 = 0;
    static mut T_TX_CALLS: i32 = 0;
    static mut T_TX_LEN: usize = 0;
    static mut T_TX_IFINDEX: i32 = 0;
    static mut T_TX_FRAME: [u8; 64] = [0; 64];
    static mut T_ENQ_CALLS: i32 = 0;
    static mut T_ENQ_LEN: [usize; 8] = [0; 8];
    static mut T_ENQ_LL: [u8; 16 * 8] = [0; 128];
    static mut T_ENQ_PAYLOAD0: [u8; 8] = [0; 8];
    static mut T_ENQ_RC: i32 = 0;
    static mut T_COLLECT: i32 = 0;
    static mut T_FLUSH: i32 = 0;
    static mut T_BUCKET_LOCKS: i32 = 0;

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
        T_HEAD = 0;
        T_TAIL = 0;
        T_PENDING = 0;
        T_DROPS = 0;
        T_SLOTS = [0; 272];
        T_DRAIN = [0; 68];
        T_SOCKETS = [ptr::null_mut(); 8];
        T_NS_INITIAL = 1;
        T_IF_UP = 0;
        T_DEFAULT_IF = 1;
        T_HWADDR = [0; 8];
        T_HWADDR_RC = 0;
        T_TX_RC = 0;
        T_TX_CALLS = 0;
        T_TX_LEN = 0;
        T_TX_IFINDEX = 0;
        T_TX_FRAME = [0; 64];
        T_ENQ_CALLS = 0;
        T_ENQ_LEN = [0; 8];
        T_ENQ_LL = [0; 128];
        T_ENQ_PAYLOAD0 = [0; 8];
        T_ENQ_RC = 0;
        T_COLLECT = 0;
        T_FLUSH = 0;
        T_BUCKET_LOCKS = 0;
        PKT_BOUND_COUNT.store(0, Ordering::Release);
    }

    unsafe fn ring() -> PacketRing {
        PacketRing {
            head: ptr::addr_of_mut!(T_HEAD),
            tail: ptr::addr_of_mut!(T_TAIL),
            pending: ptr::addr_of_mut!(T_PENDING),
            drops: ptr::addr_of_mut!(T_DROPS),
            slots: T_SLOTS.as_mut_ptr(),
            drain: T_DRAIN.as_mut_ptr(),
            depth: RING_DEPTH,
            slot_bytes: SLOT_BYTES,
            max_queue: 4,
        }
    }

    /// A `net_sockaddr_ll_t` as userspace builds it: the protocol and the
    /// hardware address are in network byte order, everything else is host
    /// order.  Offsets: family 0, protocol 2, ifindex 4, hatype 8, pkttype 10,
    /// halen 11, addr 12.
    fn sa_ll(proto: u16, ifindex: i32, halen: u8, addr: [u8; 6]) -> [u8; 20] {
        let mut a = [0u8; 20];
        a[0] = AF_PACKET as u8;
        a[1] = 0;
        a[2] = (proto >> 8) as u8;
        a[3] = (proto & 0xff) as u8;
        a[4..8].copy_from_slice(&ifindex.to_ne_bytes());
        a[11] = halen;
        a[12..18].copy_from_slice(&addr);
        a
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_view_init(s: *mut c_void, out: *mut PacketView) {
        let k = &mut *(s as *mut Sock);
        *out = PacketView {
            s,
            net_ns: k.ns as *mut c_void,
            domain: &mut k.domain,
            sock_type: &mut k.sock_type,
            protocol: &mut k.protocol,
            closed: &mut k.closed,
            rx_count: &mut k.rx_count,
            bound: &mut k.bound,
            local: k.local.as_mut_ptr(),
            local_len: &mut k.local_len,
            pkt_ifindex: &mut k.pkt_ifindex,
            pkt_protocol: &mut k.pkt_protocol,
            pkt_hatype: &mut k.pkt_hatype,
            pkt_halen: &mut k.pkt_halen,
            pkt_haddr: k.pkt_haddr.as_mut_ptr(),
            pkt_bound: &mut k.pkt_bound,
            pkt_bound_marked: &mut k.pkt_bound_marked,
        };
    }


    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_ring_lock(_flags: *mut u64) {}

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_ring_unlock(_flags: u64) {}

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_ns_initial(ns: *mut c_void) -> i32 {
        if ns.is_null() {
            return 0;
        }
        T_NS_INITIAL
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_buckets() -> i32 {
        BUCKETS
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_slots_per_bucket() -> i32 {
        PER_BUCKET
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_bucket_lock(_b: i32, _flags: *mut u64) {
        T_BUCKET_LOCKS += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_bucket_unlock(_b: i32, _flags: u64) {}

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_slot_socket(bucket: i32, i: i32) -> *mut c_void {
        let idx = (bucket * PER_BUCKET + i) as usize;
        if idx >= 8 { ptr::null_mut() } else { T_SOCKETS[idx] as *mut c_void }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_lock(_s: *mut c_void, _flags: *mut u64) {}

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_unlock(_s: *mut c_void, _flags: u64) {}

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_is_live(s: *mut c_void) -> i32 {
        (*(s as *const Sock)).live
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_enqueue(_s: *mut c_void, payload: *const u8,
                                                len: usize, ll: *const c_void,
                                                lllen: usize) -> i32 {
        let at = T_ENQ_CALLS as usize;
        if at < 8 {
            T_ENQ_LEN[at] = len;
            T_ENQ_LL[at * 16..at * 16 + lllen.min(16)]
                .copy_from_slice(core::slice::from_raw_parts(ll as *const u8,
                                                             lllen.min(16)));
        }
        T_ENQ_PAYLOAD0 = [0; 8];
        if !payload.is_null() && len > 0 {
            let n = if len < 8 { len } else { 8 };
            T_ENQ_PAYLOAD0[..n]
                .copy_from_slice(core::slice::from_raw_parts(payload, n));
        }
        T_ENQ_CALLS += 1;
        T_ENQ_RC
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_collect_read(_s: *mut c_void,
                                                     _w: *mut c_void) -> i32 {
        T_COLLECT += 1;
        0
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_wakeq_init(_w: *mut c_void) {}

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_wakeq_flush(_w: *mut c_void) {
        T_FLUSH += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_if_up(_ifindex: i32) -> i32 {
        T_IF_UP
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_if_default_index() -> i32 {
        T_DEFAULT_IF
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_if_hwaddr(_ifindex: i32, out: *mut u8) -> i32 {
        ptr::copy_nonoverlapping(T_HWADDR.as_ptr(), out, 8);
        T_HWADDR_RC
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_packet_tx(ifindex: i32, frame: *const u8,
                                           len: usize) -> i32 {
        T_TX_CALLS += 1;
        T_TX_IFINDEX = ifindex;
        T_TX_LEN = len;
        let n = if len < 64 { len } else { 64 };
        ptr::copy_nonoverlapping(frame, T_TX_FRAME.as_mut_ptr(), n);
        T_TX_RC
    }

    /* ------------------------------------------------------- census */

    #[test]
    fn census_counts_once_per_socket_and_balances_on_release() {
        unsafe {
            let _serial = serial();
            reset();
            let mut a = Sock::new();
            let mut b = Sock::new();
            assert_eq!(a20_net_packet_bound_count(), 0);
            a20_net_packet_bound_acquire(&view_of(a.p()));
            a20_net_packet_bound_acquire(&view_of(a.p())); /* idempotent */
            assert_eq!(a20_net_packet_bound_count(), 1);
            assert_eq!(a.pkt_bound_marked, 1);
            a20_net_packet_bound_acquire(&view_of(b.p()));
            assert_eq!(a20_net_packet_bound_count(), 2);
            a20_net_packet_bound_release(&view_of(a.p()));
            assert_eq!(a20_net_packet_bound_count(), 1);
            a20_net_packet_bound_release(&view_of(a.p())); /* idempotent */
            assert_eq!(a20_net_packet_bound_count(), 1);
            a20_net_packet_bound_release(&view_of(b.p()));
            assert_eq!(a20_net_packet_bound_count(), 0);
        }
    }

    /* ------------------------------------------------------- capture */

    #[test]
    fn capture_is_gated_on_the_census_and_clamps_the_length() {
        unsafe {
            let _serial = serial();
            reset();
            let r = ring();
            let mut frame = [0u8; 80];
            frame[12] = 0x08;
            frame[13] = 0x00;
            /* Nothing bound: not even a length is recorded. */
            a20_net_packet_rx_defer(&r, 1, frame.as_ptr(), 60);
            assert_eq!((T_HEAD, T_PENDING), (0, 0));

            let mut s = Sock::new();
            a20_net_packet_bound_acquire(&view_of(s.p()));
            a20_net_packet_rx_defer(&r, 1, frame.as_ptr(), 60);
            assert_eq!((T_HEAD, T_PENDING), (1, 1));
            assert_eq!(slot_len(T_SLOTS.as_ptr()), 60);
            assert_eq!(slot_ifindex(T_SLOTS.as_ptr()), 1);
            assert_eq!(T_DROPS, 0);

            /* Longer than a slot's frame area: clamped to it. */
            a20_net_packet_rx_defer(&r, 2, frame.as_ptr(), 4096);
            assert_eq!(slot_len(T_SLOTS.as_ptr().add(SLOT_BYTES as usize)),
                       (SLOT_BYTES - 4) as usize);
            assert_eq!(slot_ifindex(T_SLOTS.as_ptr().add(SLOT_BYTES as usize)), 2);
        }
    }

    #[test]
    fn a_full_ring_drops_and_counts_without_advancing() {
        unsafe {
            let _serial = serial();
            reset();
            let r = ring();
            let mut s = Sock::new();
            a20_net_packet_bound_acquire(&view_of(s.p()));
            let frame = [0u8; 32];
            /* The ring holds depth - 1 frames. */
            for i in 0..(RING_DEPTH - 1) {
                a20_net_packet_rx_defer(&r, 1, frame.as_ptr(), 32);
                assert_eq!(T_DROPS, 0, "push {} dropped", i);
            }
            assert_eq!(T_HEAD, RING_DEPTH - 1);
            a20_net_packet_rx_defer(&r, 1, frame.as_ptr(), 32);
            assert_eq!(T_HEAD, RING_DEPTH - 1);
            assert_eq!(T_DROPS, 1);
        }
    }

    #[test]
    fn drain_delivers_in_order_and_clears_pending_when_empty() {
        unsafe {
            let _serial = serial();
            reset();
            let r = ring();
            let mut s = Sock::new();
            s.sock_type = 3; /* SOCK_RAW */
            s.pkt_bound = 1;
            s.pkt_protocol = ETH_P_ALL;
            T_SOCKETS[0] = &mut s;
            a20_net_packet_bound_acquire(&view_of(s.p()));

            /* Nothing pending: the drain returns without flushing. */
            a20_net_packet_bottom_half_process(&r);
            assert_eq!((T_FLUSH, T_ENQ_CALLS), (0, 0));

            let mut frame = [0u8; 40];
            frame[12] = 0x08;
            frame[13] = 0x06;
            a20_net_packet_rx_defer(&r, 3, frame.as_ptr(), 40);
            a20_net_packet_bottom_half_process(&r);
            assert_eq!(T_ENQ_CALLS, 1);
            assert_eq!(T_ENQ_LEN[0], 40);
            assert_eq!(T_COLLECT, 1);
            assert_eq!(T_FLUSH, 1);
            assert_eq!((T_TAIL, T_PENDING), (1, 0));
            /* The delivered ancillary address names the capture interface. */
            assert_eq!(T_ENQ_LL[0], AF_PACKET as u8);
            assert_eq!(&T_ENQ_LL[4..8], &3i32.to_ne_bytes());
            assert_eq!(T_ENQ_LL[2..4], [0x08, 0x06]); /* ethertype as the wire bytes */
        }
    }

    #[test]
    fn delivery_skips_foreign_filters_and_slices_the_payload() {
        unsafe {
            let _serial = serial();
            reset();
            let r = ring();
            let mut raw = Sock::new();
            raw.sock_type = 3;
            raw.pkt_bound = 1;
            raw.pkt_protocol = 0x0806;
            let mut cooked = Sock::new();
            cooked.sock_type = 2; /* SOCK_DGRAM */
            cooked.pkt_bound = 1;
            cooked.pkt_protocol = ETH_P_ALL;
            let mut other = Sock::new();
            other.sock_type = 3;
            other.pkt_bound = 1;
            other.pkt_protocol = 0x0800; /* does not match */
            let mut unbound = Sock::new();
            unbound.sock_type = 3;
            unbound.pkt_bound = 0; /* never bound */
            let mut wrong_if = Sock::new();
            wrong_if.sock_type = 3;
            wrong_if.pkt_bound = 1;
            wrong_if.pkt_protocol = ETH_P_ALL;
            wrong_if.pkt_ifindex = 9; /* a different interface */
            let mut full = Sock::new();
            full.sock_type = 3;
            full.pkt_bound = 1;
            full.pkt_protocol = ETH_P_ALL;
            full.rx_count = 4; /* at the cap */
            let mut foreign = Sock::new();
            foreign.sock_type = 3;
            foreign.pkt_bound = 1;
            foreign.pkt_protocol = ETH_P_ALL;
            foreign.ns = 0; /* a different network namespace */
            T_SOCKETS[0] = &mut raw;
            T_SOCKETS[1] = &mut cooked;
            T_SOCKETS[2] = &mut other;
            T_SOCKETS[3] = &mut unbound;
            T_SOCKETS[4] = &mut wrong_if;
            T_SOCKETS[5] = &mut full;
            T_SOCKETS[6] = &mut foreign;

            /* One census entry per socket that can actually be delivered to,
             * so the gate opens; the foreign-namespace case is exercised below
             * through the same count. */
            for k in [&mut raw, &mut cooked, &mut other, &mut unbound,
                      &mut wrong_if, &mut full, &mut foreign] {
                a20_net_packet_bound_acquire(&view_of(k.p()));
            }

            let mut frame = [0u8; 46];
            for (i, b) in frame.iter_mut().enumerate() {
                *b = i as u8;
            }
            frame[12] = 0x08;
            frame[13] = 0x06;
            a20_net_packet_rx_defer(&r, 3, frame.as_ptr(), 46);
            a20_net_packet_bottom_half_process(&r);

            /* raw (SOCK_RAW, protocol match) and cooked (ETH_P_ALL) only. */
            assert_eq!(T_ENQ_CALLS, 2);
            assert_eq!(T_ENQ_LEN[0], 46);            /* raw keeps the header */
            assert_eq!(T_ENQ_LEN[1], 46 - ETH_HLEN); /* cooked drops it */
            assert_eq!(T_ENQ_PAYLOAD0[0], ETH_HLEN as u8);
            assert_eq!(T_BUCKET_LOCKS, BUCKETS);

            /* A foreign namespace is skipped even with a matching filter. */
            reset();
            let r = ring();
            let mut s = Sock::new();
            s.sock_type = 3;
            s.pkt_bound = 1;
            s.pkt_protocol = ETH_P_ALL;
            T_SOCKETS[0] = &mut s;
            a20_net_packet_bound_acquire(&view_of(s.p()));
            T_NS_INITIAL = 0;
            a20_net_packet_rx_defer(&r, 3, frame.as_ptr(), 46);
            a20_net_packet_bottom_half_process(&r);
            assert_eq!(T_ENQ_CALLS, 0);

            /* A short frame never reaches the filter walk. */
            reset();
            let r = ring();
            let mut s = Sock::new();
            s.sock_type = 3;
            s.pkt_bound = 1;
            s.pkt_protocol = ETH_P_ALL;
            T_SOCKETS[0] = &mut s;
            a20_net_packet_bound_acquire(&view_of(s.p()));
            a20_net_packet_rx_defer(&r, 3, frame.as_ptr(), ETH_HLEN - 1);
            a20_net_packet_bottom_half_process(&r);
            assert_eq!(T_ENQ_CALLS, 0);
        }
    }

    /* ----------------------------------------------------------- bind */

    #[test]
    fn bind_publishes_the_filter_and_defaults_the_protocol() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            /* No protocol in the sockaddr and none on the socket: ETH_P_ALL. */
            let a = sa_ll(0, 0, 6, [1, 2, 3, 4, 5, 6]);
            assert_eq!(a20_net_packet_socket_bind(s.p(), a.as_ptr() as *const c_void,
                                                  20), 0);
            assert_eq!((s.pkt_protocol, s.pkt_ifindex, s.pkt_hatype),
                       (ETH_P_ALL, 0, ARPHRD_ETHER));
            assert_eq!((s.pkt_bound, s.bound, s.pkt_bound_marked), (1, 1, 1));
            assert_eq!(s.pkt_halen, 6);
            assert_eq!(s.pkt_haddr, [1, 2, 3, 4, 5, 6, 0, 0]);
            assert_eq!(s.local_len, 20);
            assert_eq!(a20_net_packet_bound_count(), 1);

            /* A sockaddr protocol is byte-swapped into host order. */
            reset();
            let mut s = Sock::new();
            let a = sa_ll(0x0800, 1, 6, [0; 6]);
            assert_eq!(a20_net_packet_socket_bind(s.p(), a.as_ptr() as *const c_void,
                                                  20), 0);
            assert_eq!((s.pkt_protocol, s.pkt_ifindex), (0x0800, 1));

            /* No sockaddr protocol: the socket's own protocol, swapped. */
            reset();
            let mut s = Sock::new();
            s.protocol = 0x0003;
            let a = sa_ll(0, 0, 0, [0; 6]);
            assert_eq!(a20_net_packet_socket_bind(s.p(), a.as_ptr() as *const c_void,
                                                  20), 0);
            assert_eq!(s.pkt_protocol, 0x0300);
            assert_eq!(s.pkt_halen, 0);
        }
    }

    #[test]
    fn bind_refuses_bad_filters_and_a_down_interface() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            let a = sa_ll(0x0800, 0, 6, [0; 6]);

            /* Short address. */
            assert_eq!(a20_net_packet_socket_bind(s.p(), a.as_ptr() as *const c_void,
                                                  19), -EINVAL);
            /* Null address. */
            assert_eq!(a20_net_packet_socket_bind(s.p(), ptr::null(), 20), -EINVAL);
            /* Wrong family in the sockaddr. */
            let mut bad = a;
            bad[0] = 2;
            assert_eq!(a20_net_packet_socket_bind(s.p(), bad.as_ptr() as *const c_void,
                                                  20), -EAFNOSUPPORT);
            /* Negative ifindex. */
            let mut neg = a;
            neg[4..8].copy_from_slice(&(-1i32).to_ne_bytes());
            assert_eq!(a20_net_packet_socket_bind(s.p(), neg.as_ptr() as *const c_void,
                                                  16), -EINVAL);
            /* Interface exists but is down. */
            T_IF_UP = -1;
            let up = sa_ll(0x0800, 1, 6, [0; 6]);
            assert_eq!(a20_net_packet_socket_bind(s.p(), up.as_ptr() as *const c_void,
                                                  20), -ENODEV);
            assert_eq!(s.pkt_bound, 0);
            /* A socket of another family. */
            T_IF_UP = 0;
            reset();
            let mut s = Sock::new();
            s.domain = 2;
            assert_eq!(a20_net_packet_socket_bind(s.p(), a.as_ptr() as *const c_void,
                                                  20), -EAFNOSUPPORT);
            /* A foreign namespace. */
            reset();
            let mut s = Sock::new();
            T_NS_INITIAL = 0;
            assert_eq!(a20_net_packet_socket_bind(s.p(), a.as_ptr() as *const c_void,
                                                  20), -ENODEV);
            /* A dead socket. */
            reset();
            let mut s = Sock::new();
            s.live = 0;
            assert_eq!(a20_net_packet_socket_bind(s.p(), a.as_ptr() as *const c_void,
                                                  20), -ENOTSOCK);
            assert_eq!(a20_net_packet_bound_count(), 0);
        }
    }

    #[test]
    fn a_second_bind_does_not_double_count_the_census() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            let a = sa_ll(0x0800, 0, 6, [0; 6]);
            assert_eq!(a20_net_packet_socket_bind(s.p(), a.as_ptr() as *const c_void,
                                                  20), 0);
            assert_eq!(a20_net_packet_socket_bind(s.p(), a.as_ptr() as *const c_void,
                                                  20), 0);
            assert_eq!(a20_net_packet_bound_count(), 1);
            a20_net_packet_bound_release(&view_of(s.p()));
            assert_eq!(a20_net_packet_bound_count(), 0);
        }
    }

    /* ----------------------------------------------------------- send */

    #[test]
    fn send_builds_a_cooked_frame_from_the_filter_and_the_sockaddr() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            s.sock_type = 2; /* SOCK_DGRAM */
            s.pkt_bound = 1;
            s.pkt_ifindex = 1;
            s.pkt_protocol = 0x0800;
            s.pkt_halen = 6;
            s.pkt_haddr = [0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0, 0];
            T_HWADDR = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0, 0];
            let payload = [9u8; 4];

            assert_eq!(a20_net_packet_socket_send(s.p(), payload.as_ptr() as *const c_void,
                        4, ptr::null(), 0, 1536), 4);
            assert_eq!(T_TX_CALLS, 1);
            assert_eq!(T_TX_IFINDEX, 1);
            assert_eq!(T_TX_LEN, 4 + ETH_HLEN);
            /* dst from the bound hardware address, src from the interface. */
            assert_eq!(&T_TX_FRAME[0..6], &[0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff]);
            assert_eq!(&T_TX_FRAME[6..12], &[0x11, 0x22, 0x33, 0x44, 0x55, 0x66]);
            assert_eq!(&T_TX_FRAME[12..14], &[0x08, 0x00]);
            assert_eq!(&T_TX_FRAME[14..18], &[9, 9, 9, 9]);

            /* A sockaddr's address, protocol and interface win. */
            reset();
            let mut s = Sock::new();
            s.sock_type = 2;
            s.pkt_bound = 1;
            s.pkt_ifindex = 1;
            s.pkt_protocol = 0x0800;
            s.pkt_halen = 6;
            s.pkt_haddr = [0xaa; 8];
            T_HWADDR = [0x11; 8];
            let a = sa_ll(0x86dd, 7, 6, [1, 2, 3, 4, 5, 6]);
            assert_eq!(a20_net_packet_socket_send(s.p(), payload.as_ptr() as *const c_void,
                        4, a.as_ptr() as *const c_void, 20, 1536), 4);
            assert_eq!(T_TX_IFINDEX, 7);
            assert_eq!(&T_TX_FRAME[0..6], &[1, 2, 3, 4, 5, 6]);
            assert_eq!(&T_TX_FRAME[12..14], &[0x86, 0xdd]);

            /* No hardware address anywhere: broadcast, and the default
             * interface when the socket has none. */
            reset();
            let mut s = Sock::new();
            s.sock_type = 2;
            s.pkt_bound = 1;
            s.pkt_protocol = 0x0800;
            T_HWADDR = [0x11; 8];
            T_DEFAULT_IF = 3;
            assert_eq!(a20_net_packet_socket_send(s.p(), payload.as_ptr() as *const c_void,
                        4, ptr::null(), 0, 1536), 4);
            assert_eq!(T_TX_IFINDEX, 3);
            assert_eq!(&T_TX_FRAME[0..6], &[0xff; 6]);
        }
    }

    #[test]
    fn send_a_raw_frame_is_passed_through_whole() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            s.sock_type = 3; /* SOCK_RAW */
            s.pkt_bound = 1;
            s.pkt_ifindex = 2;
            let mut frame = [0u8; 20];
            frame[12] = 0x08;
            frame[13] = 0x06;
            frame[19] = 0x42;
            assert_eq!(a20_net_packet_socket_send(s.p(), frame.as_ptr() as *const c_void,
                        20, ptr::null(), 0, 1536), 20);
            assert_eq!(T_TX_LEN, 20);
            assert_eq!(T_TX_FRAME[19], 0x42);
            assert_eq!(&T_TX_FRAME[12..14], &[0x08, 0x06]);
        }
    }

    #[test]
    fn send_refuses_what_it_cannot_carry() {
        unsafe {
            let _serial = serial();
            reset();
            let mut s = Sock::new();
            s.sock_type = 3;
            s.pkt_bound = 1;
            s.pkt_ifindex = 1;
            let payload = [1u8; 8];

            /* Not bound. */
            s.pkt_bound = 0;
            assert_eq!(a20_net_packet_socket_send(s.p(), payload.as_ptr() as *const c_void,
                        8, ptr::null(), 0, 1536), -EDESTADDRREQ);
            s.pkt_bound = 1;
            /* Empty payload, and a null buffer. */
            assert_eq!(a20_net_packet_socket_send(s.p(), payload.as_ptr() as *const c_void,
                        0, ptr::null(), 0, 1536), -EINVAL);
            assert_eq!(a20_net_packet_socket_send(s.p(), ptr::null(), 8, ptr::null(), 0,
                        1536), -EINVAL);
            /* Oversized for the profile's frame. */
            assert_eq!(a20_net_packet_socket_send(s.p(), payload.as_ptr() as *const c_void,
                        2048, ptr::null(), 0, 512), -EMSGSIZE);
            /* A cooked frame needs the header's room too. */
            s.sock_type = 2;
            assert_eq!(a20_net_packet_socket_send(s.p(), payload.as_ptr() as *const c_void,
                        512, ptr::null(), 0, 512), -EMSGSIZE);
            /* No usable interface. */
            s.sock_type = 3;
            s.pkt_ifindex = 0;
            T_DEFAULT_IF = -1;
            assert_eq!(a20_net_packet_socket_send(s.p(), payload.as_ptr() as *const c_void,
                        8, ptr::null(), 0, 1536), -ENODEV);
            /* The interface has no hardware address. */
            reset();
            let mut s = Sock::new();
            s.sock_type = 2;
            s.pkt_bound = 1;
            s.pkt_ifindex = 1;
            T_HWADDR_RC = -1;
            assert_eq!(a20_net_packet_socket_send(s.p(), payload.as_ptr() as *const c_void,
                        8, ptr::null(), 0, 1536), -ENODEV);
            /* The transmit op fails. */
            T_HWADDR_RC = 0;
            T_TX_RC = -1;
            assert_eq!(a20_net_packet_socket_send(s.p(), payload.as_ptr() as *const c_void,
                        8, ptr::null(), 0, 1536), -EIO);
            /* A foreign namespace. */
            reset();
            let mut s = Sock::new();
            s.pkt_bound = 1;
            T_NS_INITIAL = 0;
            assert_eq!(a20_net_packet_socket_send(s.p(), payload.as_ptr() as *const c_void,
                        8, ptr::null(), 0, 1536), -ENODEV);
        }
    }
}
