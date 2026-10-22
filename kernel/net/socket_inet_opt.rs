#![no_std]

//! The INET socket option and buffer machinery: the congestion-algorithm switch,
//! the per-type buffer ceilings and the value in force, SO_SNDBUF/SO_RCVBUF
//! application with its clamps and the shrink-announce guard, the full TCP option
//! application and callback wiring, and the IP-option snapshot, retry loop,
//! effective values and reapply.
//!
//! The C side keeps the primitives this module calls: the socket and lane locks,
//! the per-protocol shared guards, the configured constants, the pcb field
//! access and the lwIP calls.

use core::ffi::c_void;
use core::ptr;
use core::sync::atomic::{AtomicU32, Ordering};

const SOCK_STREAM: i32 = 1;
const SOCK_SEQPACKET: i32 = 5;

const TCP_CONG_RENO: i32 = 0;
const TCP_CONG_CUBIC: i32 = 2;

const SHARED_UDP: i32 = 0;
const SHARED_RAW: i32 = 1;

const NET_IP_TTL_DEFAULT: u8 = 64;

/// `net_inet_ip_options_snapshot_t` as Rust owns it now.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Snapshot {
    pub ttl: u8,
    pub tos: u8,
    pub mc_ttl: u8,
    pub mc_loop: u8,
    pub generation: u32,
}

#[repr(C)]
pub struct OptView {
    pub s: *mut c_void,
    pub sock_type: *mut i32,
    pub tcp_nodelay: *mut i32,
    pub tcp_congestion: *mut u8,
    pub snd_buf: *mut u32,
    pub rcv_buf: *mut u32,
    pub keepalive: *mut i32,
    pub keep_idle: *mut i32,
    pub keep_intvl: *mut i32,
    pub keep_cnt: *mut i32,
    pub reuseaddr: *mut i32,
    pub ip_ttl_set: *mut u8,
    pub ip_ttl: *mut u8,
    pub ip_tos_set: *mut u8,
    pub ip_tos: *mut u8,
    pub mc_ttl: *mut u8,
    pub mc_loop: *mut u8,
    pub ip_opts_generation: *mut u32,
    pub udp: *mut *mut c_void,
    pub raw: *mut *mut c_void,
    pub tcp: *mut *mut c_void,
}

unsafe extern "C" {
    fn a20_net_inet_opt_view_init(s: *mut c_void, out: *mut OptView);
    fn a20_net_inet_opt_sock_lock(s: *mut c_void, flags: *mut u64);
    fn a20_net_inet_opt_sock_unlock(s: *mut c_void, flags: u64);
    fn a20_net_inet_opt_lane_lock(s: *mut c_void) -> u64;
    fn a20_net_inet_opt_lane_unlock(flags: u64);
    fn a20_net_inet_opt_shared_lock(pcb: *mut c_void, kind: i32) -> u64;
    fn a20_net_inet_opt_shared_unlock(pcb: *mut c_void, kind: i32, flags: u64);
    fn a20_net_inet_opt_tcp_snd_buf() -> u32;
    fn a20_net_inet_opt_tcp_wnd() -> u32;
    fn a20_net_inet_opt_tcp_rcv_scale() -> u32;
    fn a20_net_inet_opt_max_payload() -> u32;
    fn a20_net_inet_opt_dgram_rcv_default() -> u32;
    fn a20_net_inet_opt_pcb_is_listen(pcb: *mut c_void) -> i32;
    fn a20_net_inet_opt_set_snd_buf(pcb: *mut c_void, v: u32);
    fn a20_net_inet_opt_set_wnd_limit(pcb: *mut c_void, v: u32);
    fn a20_net_inet_opt_get_rcv_wnd(pcb: *mut c_void) -> u32;
    fn a20_net_inet_opt_set_rcv_wnd(pcb: *mut c_void, v: u32);
    fn a20_net_inet_opt_set_rcv_ann_wnd(pcb: *mut c_void, v: u32);
    fn a20_net_inet_opt_pcb_established(pcb: *mut c_void) -> i32;
    fn a20_net_inet_opt_get_rcv_ann_wnd(pcb: *mut c_void) -> u32;
    fn a20_net_inet_opt_nagle_disable(pcb: *mut c_void);
    fn a20_net_inet_opt_cong_get(pcb: *mut c_void) -> i32;
    fn a20_net_inet_opt_cong_set(pcb: *mut c_void, alg: i32);
    fn a20_net_inet_opt_cubic_init(pcb: *mut c_void);
    fn a20_net_inet_opt_set_keepalive(pcb: *mut c_void);
    fn a20_net_inet_opt_set_reuseaddr(pcb: *mut c_void);
    fn a20_net_inet_opt_set_keep_idle(pcb: *mut c_void, v: u32);
    fn a20_net_inet_opt_set_keep_intvl(pcb: *mut c_void, v: u32);
    fn a20_net_inet_opt_set_keep_cnt(pcb: *mut c_void, v: u32);
    fn a20_net_inet_opt_set_callbacks(s: *mut c_void, pcb: *mut c_void);
    fn a20_net_inet_opt_apply_udp(pcb: *mut c_void, ttl: u8, tos: u8, mc_ttl: u8,
                                  mc_loop: u8);
    fn a20_net_inet_opt_apply_raw(pcb: *mut c_void, ttl: u8, tos: u8, mc_ttl: u8,
                                  mc_loop: u8);
    fn a20_net_inet_opt_apply_tcp(pcb: *mut c_void, ttl: u8, tos: u8);
}

fn zero_view() -> OptView {
    OptView {
        s: ptr::null_mut(),
        sock_type: ptr::null_mut(),
        tcp_nodelay: ptr::null_mut(),
        tcp_congestion: ptr::null_mut(),
        snd_buf: ptr::null_mut(),
        rcv_buf: ptr::null_mut(),
        keepalive: ptr::null_mut(),
        keep_idle: ptr::null_mut(),
        keep_intvl: ptr::null_mut(),
        keep_cnt: ptr::null_mut(),
        reuseaddr: ptr::null_mut(),
        ip_ttl_set: ptr::null_mut(),
        ip_ttl: ptr::null_mut(),
        ip_tos_set: ptr::null_mut(),
        ip_tos: ptr::null_mut(),
        mc_ttl: ptr::null_mut(),
        mc_loop: ptr::null_mut(),
        ip_opts_generation: ptr::null_mut(),
        udp: ptr::null_mut(),
        raw: ptr::null_mut(),
        tcp: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> OptView {
    let mut v = zero_view();
    a20_net_inet_opt_view_init(s, &mut v);
    v
}

/* ---------------------------------------------------------- congestion */

/// CUBIC is switched on by changing the algorithm and initialising its state
/// from the current window, without resetting the window; Reno is just the
/// field, because the cubic hooks are gated on it and the stale state is never
/// read again.
#[no_mangle]
pub unsafe extern "C" fn a20_net_cong_apply_impl(pcb: *mut c_void, alg: u8) {
    if pcb.is_null() {
        return;
    }
    if alg as i32 == TCP_CONG_CUBIC {
        if a20_net_inet_opt_cong_get(pcb) != TCP_CONG_CUBIC {
            a20_net_inet_opt_cong_set(pcb, TCP_CONG_CUBIC);
            a20_net_inet_opt_cubic_init(pcb);
        }
    } else {
        a20_net_inet_opt_cong_set(pcb, TCP_CONG_RENO);
    }
}

/* ------------------------------------------------------------- ceilings */

#[no_mangle]
pub unsafe extern "C" fn a20_net_socket_buf_ceiling(s: *mut c_void, is_snd: i32) -> u32 {
    if s.is_null() {
        return 0;
    }
    let v = view_of(s);
    let t = *v.sock_type;
    if t == SOCK_STREAM || t == SOCK_SEQPACKET {
        if is_snd != 0 {
            /* The pcb's real capacity: tcp_write will not be handed more. */
            return a20_net_inet_opt_tcp_snd_buf();
        }
        /* min(TCP_WND, what window scaling can put on the wire). */
        let wnd = a20_net_inet_opt_tcp_wnd();
        let scale = 0xffffu32 << a20_net_inet_opt_tcp_rcv_scale();
        return if scale < wnd { scale } else { wnd };
    }
    if is_snd != 0 {
        return a20_net_inet_opt_max_payload();
    }
    a20_net_inet_opt_dgram_rcv_default()
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_socket_buf_in_force(s: *mut c_void, is_snd: i32) -> u32 {
    if s.is_null() {
        return 0;
    }
    let ceiling = a20_net_socket_buf_ceiling(s, is_snd);
    let v = view_of(s);
    let mut flags = 0u64;
    a20_net_inet_opt_sock_lock(s, &mut flags);
    let have = if is_snd != 0 { *v.snd_buf } else { *v.rcv_buf };
    a20_net_inet_opt_sock_unlock(s, flags);
    /* A zero stored value means "never set"; the ceiling is the honest answer. */
    if have != 0 && have < ceiling { have } else { ceiling }
}

/* ---------------------------------------------------------- buffer apply */

#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_tcp_buf_apply(snd_buf: u32, rcv_buf: u32,
                                                    pcb: *mut c_void) {
    if pcb.is_null() {
        return;
    }
    /* A LISTEN pcb has no receive window of its own; lwIP creates the real one
     * in tcp_accept(), and tcp_recved() asserts against it. */
    if a20_net_inet_opt_pcb_is_listen(pcb) != 0 {
        return;
    }
    if snd_buf != 0 {
        let max = a20_net_inet_opt_tcp_snd_buf();
        /* Assigned outright in both directions so a raised ceiling reaches a
         * live connection.  snd_buf is the available space, so writing the
         * ceiling here grants exactly "this much may be queued" and never
         * mints room beyond it. */
        a20_net_inet_opt_set_snd_buf(pcb, if snd_buf > max { max } else { snd_buf });
    }
    if rcv_buf != 0 {
        /* The ceiling is the configured TCP_WND and the configured scale, not
         * the pcb's current state: before the handshake the pcb-relative forms
         * answer 64 KiB and would pin every socket there. */
        let wnd = a20_net_inet_opt_tcp_wnd();
        let scale = 0xffffu32 << a20_net_inet_opt_tcp_rcv_scale();
        let ceiling = if scale < wnd { scale } else { wnd };
        let rcv = if rcv_buf > ceiling { ceiling } else { rcv_buf };
        /* At the configured ceiling there is nothing to cap: leaving
         * wnd_limit at 0 keeps tcp_recved() on the upstream path exactly. */
        if rcv < ceiling {
            a20_net_inet_opt_set_wnd_limit(pcb, rcv);
        }
        if a20_net_inet_opt_get_rcv_wnd(pcb) > rcv &&
            a20_net_inet_opt_pcb_established(pcb) == 0 {
            /* Before the handshake there is no data and no announcement in
             * flight, so the SYN can carry the ceiling directly.  On a live
             * connection wnd_limit is a soft ceiling: tcp_recved() stops
             * reopening above it but never shrinks an already-larger window,
             * because that would drop data the peer was told it could send. */
            a20_net_inet_opt_set_rcv_wnd(pcb, rcv);
            if a20_net_inet_opt_get_rcv_ann_wnd(pcb) != 0 {
                a20_net_inet_opt_set_rcv_ann_wnd(pcb, rcv);
            }
        }
    }
}

/* ------------------------------------------------------- option application */

#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_tcp_apply_options(s: *mut c_void, pcb: *mut c_void) {
    let v = view_of(s);
    if *v.tcp_nodelay != 0 {
        a20_net_inet_opt_nagle_disable(pcb);
    }
    a20_net_cong_apply_impl(pcb, *v.tcp_congestion);
    a20_net_inet_tcp_buf_apply(*v.snd_buf, *v.rcv_buf, pcb);
    if *v.keepalive != 0 {
        a20_net_inet_opt_set_keepalive(pcb);
    }
    /* SO_REUSEADDR has to reach the pcb: lwIP asks the pcb in tcp_bind and
     * tcp_listen, so a socket-record flag alone leaves it inert. */
    if *v.reuseaddr != 0 {
        a20_net_inet_opt_set_reuseaddr(pcb);
    }
    if *v.keep_idle > 0 {
        a20_net_inet_opt_set_keep_idle(pcb, (*v.keep_idle as u32) * 1000);
    }
    if *v.keep_intvl > 0 {
        a20_net_inet_opt_set_keep_intvl(pcb, (*v.keep_intvl as u32) * 1000);
    }
    if *v.keep_cnt > 0 {
        a20_net_inet_opt_set_keep_cnt(pcb, *v.keep_cnt as u32);
    }
    a20_net_inet_opt_set_callbacks(s, pcb);
}

/* ------------------------------------------------------------ IP options */

unsafe fn snapshot_into(v: &OptView, generation: u32) -> Snapshot {
    Snapshot {
        /* "Never set" and "set to 0" are different requests; only the effective
         * value is observable on the wire. */
        ttl: if *v.ip_ttl_set != 0 { *v.ip_ttl } else { NET_IP_TTL_DEFAULT },
        tos: if *v.ip_tos_set != 0 { *v.ip_tos } else { 0 },
        mc_ttl: if *v.mc_ttl != 0 { *v.mc_ttl } else { 1 },
        mc_loop: *v.mc_loop,
        generation,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_ip_options_snapshot(s: *mut c_void, out: *mut c_void) {
    let v = view_of(s);
    let mut flags = 0u64;
    a20_net_inet_opt_sock_lock(s, &mut flags);
    let snap = snapshot_into(&v, (*(v.ip_opts_generation as *const AtomicU32))
        .load(Ordering::Relaxed));
    a20_net_inet_opt_sock_unlock(s, flags);
    ptr::write_unaligned(out as *mut Snapshot, snap);
}

/// Snapshot and lane-lock, retrying until the generation the snapshot saw is
/// still the current one: a `setsockopt` racing the lock would otherwise have
/// its change silently dropped from the options this send applies.
#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_socket_ip_lane_lock(s: *mut c_void,
                                                          out: *mut c_void) -> u64 {
    let v = view_of(s);
    loop {
        let mut flags = 0u64;
        a20_net_inet_opt_sock_lock(s, &mut flags);
        let snap = snapshot_into(&v, (*(v.ip_opts_generation as *const AtomicU32))
            .load(Ordering::Relaxed));
        a20_net_inet_opt_sock_unlock(s, flags);
        ptr::write_unaligned(out as *mut Snapshot, snap);
        let lane_flags = a20_net_inet_opt_lane_lock(s);
        let now = (*(v.ip_opts_generation as *const AtomicU32)).load(Ordering::Acquire);
        if now == snap.generation {
            return lane_flags;
        }
        a20_net_inet_opt_lane_unlock(lane_flags);
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_ip_effective(s: *mut c_void, ttl: *mut u8,
                                                   tos: *mut u8, mc_ttl: *mut u8) {
    let v = view_of(s);
    let mut flags = 0u64;
    a20_net_inet_opt_sock_lock(s, &mut flags);
    let snap = snapshot_into(&v, 0);
    a20_net_inet_opt_sock_unlock(s, flags);
    if !ttl.is_null() {
        *ttl = snap.ttl;
    }
    if !tos.is_null() {
        *tos = snap.tos;
    }
    if !mc_ttl.is_null() {
        *mc_ttl = snap.mc_ttl;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_ip_opts_apply_snapshot_locked(
    s: *mut c_void, opts: *const c_void,
) {
    let v = view_of(s);
    let o = ptr::read_unaligned(opts as *const Snapshot);
    let udp = *v.udp;
    if !udp.is_null() {
        a20_net_inet_opt_apply_udp(udp, o.ttl, o.tos, o.mc_ttl, o.mc_loop);
    }
    let raw = *v.raw;
    if !raw.is_null() {
        a20_net_inet_opt_apply_raw(raw, o.ttl, o.tos, o.mc_ttl, o.mc_loop);
    }
    let tcp = *v.tcp;
    if !tcp.is_null() {
        a20_net_inet_opt_apply_tcp(tcp, o.ttl, o.tos);
    }
}

/// The lock-taking wrapper for the setsockopt path; UDP and raw also take their
/// per-protocol shared guard because receive callbacks read the same fields.
#[no_mangle]
pub unsafe extern "C" fn a20_net_inet_ip_opts_apply(s: *mut c_void) {
    if s.is_null() {
        return;
    }
    let v = view_of(s);
    let mut snap = Snapshot { ttl: 0, tos: 0, mc_ttl: 0, mc_loop: 0, generation: 0 };
    let flags = a20_net_inet_socket_ip_lane_lock(
        s, &mut snap as *mut Snapshot as *mut c_void);
    let udp = *v.udp;
    if !udp.is_null() {
        let shared = a20_net_inet_opt_shared_lock(udp, SHARED_UDP);
        a20_net_inet_opt_apply_udp(udp, snap.ttl, snap.tos, snap.mc_ttl,
                                   snap.mc_loop);
        a20_net_inet_opt_shared_unlock(udp, SHARED_UDP, shared);
        a20_net_inet_opt_lane_unlock(flags);
        return;
    }
    let raw = *v.raw;
    if !raw.is_null() {
        let shared = a20_net_inet_opt_shared_lock(raw, SHARED_RAW);
        a20_net_inet_opt_apply_raw(raw, snap.ttl, snap.tos, snap.mc_ttl,
                                   snap.mc_loop);
        a20_net_inet_opt_shared_unlock(raw, SHARED_RAW, shared);
        a20_net_inet_opt_lane_unlock(flags);
        return;
    }
    let tcp = *v.tcp;
    if !tcp.is_null() {
        a20_net_inet_opt_apply_tcp(tcp, snap.ttl, snap.tos);
    }
    a20_net_inet_opt_lane_unlock(flags);
}

#[cfg(test)]
#[allow(static_mut_refs)]
mod tests {
    use super::*;

    #[repr(C)]
    struct Sock {
        sock_type: i32,
        tcp_nodelay: i32,
        tcp_congestion: u8,
        snd_buf: u32,
        rcv_buf: u32,
        keepalive: i32,
        keep_idle: i32,
        keep_intvl: i32,
        keep_cnt: i32,
        reuseaddr: i32,
        ip_ttl_set: u8,
        ip_ttl: u8,
        ip_tos_set: u8,
        ip_tos: u8,
        mc_ttl: u8,
        mc_loop: u8,
        generation: u32,
        udp: *mut c_void,
        raw: *mut c_void,
        tcp: *mut c_void,
    }

    impl Sock {
        const fn new() -> Sock {
            Sock {
                sock_type: SOCK_STREAM,
                tcp_nodelay: 0,
                tcp_congestion: 0,
                snd_buf: 0,
                rcv_buf: 0,
                keepalive: 0,
                keep_idle: 0,
                keep_intvl: 0,
                keep_cnt: 0,
                reuseaddr: 0,
                ip_ttl_set: 0,
                ip_ttl: 0,
                ip_tos_set: 0,
                ip_tos: 0,
                mc_ttl: 0,
                mc_loop: 0,
                generation: 7,
                udp: ptr::null_mut(),
                raw: ptr::null_mut(),
                tcp: ptr::null_mut(),
            }
        }
    }

    static mut S_SOCK: Sock = Sock::new();
    static mut T_SND_MAX: u32 = 93440;
    static mut T_WND: u32 = 65535;
    static mut T_RCV_SCALE: u32 = 0;
    static mut T_MAX_PAYLOAD: u32 = 65535;
    static mut T_DGRAM_RCV: u32 = 262144;
    static mut T_IS_LISTEN: i32 = 0;
    static mut T_SND: u32 = 0;
    static mut T_WND_LIMIT: u32 = 0;
    static mut T_RCV_WND: u32 = 0;
    static mut T_RCV_ANN: u32 = 0;
    static mut T_ESTABLISHED: i32 = 1;
    static mut T_RECVED: i32 = 0;
    static mut T_NAGLE: i32 = 0;
    static mut T_CONG: i32 = 0;
    static mut T_CONG_SET: i32 = -1;
    static mut T_CUBIC_INIT: i32 = 0;
    static mut T_KEEPALIVE: i32 = 0;
    static mut T_REUSE: i32 = 0;
    static mut T_KEEP_IDLE: u32 = 0;
    static mut T_KEEP_INTVL: u32 = 0;
    static mut T_KEEP_CNT: u32 = 0;
    static mut T_CALLBACKS: i32 = 0;
    static mut T_UDP_APPLY: [i32; 4] = [0; 4];
    static mut T_RAW_APPLY: [i32; 4] = [0; 4];
    static mut T_TCP_APPLY: [i32; 2] = [0; 2];
    static mut T_LANE_LOCK: i32 = 0;
    static mut T_LANE_UNLOCK: i32 = 0;
    static mut T_GEN_SCRIPT: [u32; 2] = [0; 2];
    static mut T_GEN_AT: usize = 0;
    static mut T_SHARED: [i32; 2] = [0; 2];
    static mut T_SOCK_LOCK: i32 = 0;

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
        T_SND_MAX = 93440;
        T_WND = 65535;
        T_RCV_SCALE = 0;
        T_MAX_PAYLOAD = 65535;
        T_DGRAM_RCV = 262144;
        T_IS_LISTEN = 0;
        T_SND = 0;
        T_WND_LIMIT = 0;
        T_RCV_WND = 0;
        T_RCV_ANN = 0;
        T_ESTABLISHED = 1;
        T_RECVED = 0;
        T_NAGLE = 0;
        T_CONG = 0;
        T_CONG_SET = -1;
        T_CUBIC_INIT = 0;
        T_KEEPALIVE = 0;
        T_REUSE = 0;
        T_KEEP_IDLE = 0;
        T_KEEP_INTVL = 0;
        T_KEEP_CNT = 0;
        T_CALLBACKS = 0;
        T_UDP_APPLY = [0; 4];
        T_RAW_APPLY = [0; 4];
        T_TCP_APPLY = [0; 2];
        T_LANE_LOCK = 0;
        T_LANE_UNLOCK = 0;
        T_GEN_SCRIPT = [0; 2];
        T_GEN_AT = 0;
        T_SHARED = [0; 2];
        T_SOCK_LOCK = 0;
    }

    unsafe fn sock() -> *mut c_void {
        ptr::addr_of_mut!(S_SOCK) as *mut c_void
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_view_init(s: *mut c_void,
                                                    out: *mut OptView) {
        let k = &mut *(s as *mut Sock);
        *out = OptView {
            s,
            sock_type: &mut k.sock_type,
            tcp_nodelay: &mut k.tcp_nodelay,
            tcp_congestion: &mut k.tcp_congestion,
            snd_buf: &mut k.snd_buf,
            rcv_buf: &mut k.rcv_buf,
            keepalive: &mut k.keepalive,
            keep_idle: &mut k.keep_idle,
            keep_intvl: &mut k.keep_intvl,
            keep_cnt: &mut k.keep_cnt,
            reuseaddr: &mut k.reuseaddr,
            ip_ttl_set: &mut k.ip_ttl_set,
            ip_ttl: &mut k.ip_ttl,
            ip_tos_set: &mut k.ip_tos_set,
            ip_tos: &mut k.ip_tos,
            mc_ttl: &mut k.mc_ttl,
            mc_loop: &mut k.mc_loop,
            ip_opts_generation: &mut k.generation,
            udp: &mut k.udp,
            raw: &mut k.raw,
            tcp: &mut k.tcp,
        };
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_sock_lock(_s: *mut c_void, _f: *mut u64) {
        T_SOCK_LOCK += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_sock_unlock(_s: *mut c_void, _f: u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_lane_lock(_s: *mut c_void) -> u64 {
        T_LANE_LOCK += 1;
        let at = T_GEN_AT;
        if at < 2 && T_GEN_SCRIPT[at] != 0 {
            T_GEN_AT += 1;
            (*ptr::addr_of_mut!(S_SOCK)).generation = T_GEN_SCRIPT[at];
        }
        3
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_lane_unlock(flags: u64) {
        assert_eq!(flags, 3);
        T_LANE_UNLOCK += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_shared_lock(_p: *mut c_void,
                                                      kind: i32) -> u64 {
        T_SHARED[kind as usize] += 1;
        5
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_shared_unlock(_p: *mut c_void, kind: i32,
                                                        flags: u64) {
        assert_eq!(flags, 5);
        assert!(kind == 0 || kind == 1);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_tcp_snd_buf() -> u32 {
        T_SND_MAX
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_tcp_wnd() -> u32 {
        T_WND
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_tcp_rcv_scale() -> u32 {
        T_RCV_SCALE
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_max_payload() -> u32 {
        T_MAX_PAYLOAD
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_dgram_rcv_default() -> u32 {
        T_DGRAM_RCV
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_pcb_is_listen(_p: *mut c_void) -> i32 {
        T_IS_LISTEN
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_set_snd_buf(_p: *mut c_void, v: u32) {
        T_SND = v;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_set_wnd_limit(_p: *mut c_void, v: u32) {
        T_WND_LIMIT = v;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_get_rcv_wnd(_p: *mut c_void) -> u32 {
        T_RCV_WND
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_set_rcv_wnd(_p: *mut c_void, v: u32) {
        T_RCV_WND = v;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_get_rcv_ann_wnd(_p: *mut c_void) -> u32 {
        T_RCV_ANN
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_set_rcv_ann_wnd(_p: *mut c_void, v: u32) {
        T_RCV_ANN = v;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_pcb_established(_p: *mut c_void) -> i32 {
        T_ESTABLISHED
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_nagle_disable(_p: *mut c_void) {
        T_NAGLE += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_cong_get(_p: *mut c_void) -> i32 {
        T_CONG
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_cong_set(_p: *mut c_void, alg: i32) {
        T_CONG_SET = alg;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_cubic_init(_p: *mut c_void) {
        T_CUBIC_INIT += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_set_keepalive(_p: *mut c_void) {
        T_KEEPALIVE += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_set_reuseaddr(_p: *mut c_void) {
        T_REUSE += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_set_keep_idle(_p: *mut c_void, v: u32) {
        T_KEEP_IDLE = v;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_set_keep_intvl(_p: *mut c_void, v: u32) {
        T_KEEP_INTVL = v;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_set_keep_cnt(_p: *mut c_void, v: u32) {
        T_KEEP_CNT = v;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_set_callbacks(_s: *mut c_void,
                                                         _p: *mut c_void) {
        T_CALLBACKS += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_apply_udp(_p: *mut c_void, ttl: u8,
                                                    tos: u8, mc_ttl: u8,
                                                    mc_loop: u8) {
        T_UDP_APPLY = [ttl as i32, tos as i32, mc_ttl as i32, mc_loop as i32];
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_apply_raw(_p: *mut c_void, ttl: u8,
                                                    tos: u8, mc_ttl: u8,
                                                    mc_loop: u8) {
        T_RAW_APPLY = [ttl as i32, tos as i32, mc_ttl as i32, mc_loop as i32];
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_inet_opt_apply_tcp(_p: *mut c_void, ttl: u8,
                                                    tos: u8) {
        T_TCP_APPLY = [ttl as i32, tos as i32];
    }

    #[test]
    fn congestion_switches_initialize_only_on_a_change() {
        unsafe {
            let _serial = serial();
            reset();
            /* CUBIC from Reno: set and initialise. */
            a20_net_cong_apply_impl(8usize as *mut c_void, TCP_CONG_CUBIC as u8);
            assert_eq!(T_CONG_SET, TCP_CONG_CUBIC);
            assert_eq!(T_CUBIC_INIT, 1);
            /* Already CUBIC: nothing to do. */
            reset();
            T_CONG = TCP_CONG_CUBIC;
            a20_net_cong_apply_impl(8usize as *mut c_void, TCP_CONG_CUBIC as u8);
            assert_eq!((T_CONG_SET, T_CUBIC_INIT), (-1, 0));
            /* Reno: the field only. */
            reset();
            T_CONG = TCP_CONG_CUBIC;
            a20_net_cong_apply_impl(8usize as *mut c_void, 0);
            assert_eq!((T_CONG_SET, T_CUBIC_INIT), (TCP_CONG_RENO, 0));
            /* A null pcb is a no-op. */
            reset();
            a20_net_cong_apply_impl(ptr::null_mut(), TCP_CONG_CUBIC as u8);
            assert_eq!((T_CONG_SET, T_CUBIC_INIT), (-1, 0));
        }
    }

    #[test]
    fn the_ceilings_follow_the_type_and_direction() {
        unsafe {
            let _serial = serial();
            reset();
            assert_eq!(a20_net_socket_buf_ceiling(sock(), 1), 93440);
            assert_eq!(a20_net_socket_buf_ceiling(sock(), 0), 65535);
            /* The scale bound wins when it is smaller. */
            T_WND = 2000000;
            T_RCV_SCALE = 4;
            assert_eq!(a20_net_socket_buf_ceiling(sock(), 0), 0xffff << 4);
            /* Datagram ceilings. */
            (*sock().cast::<Sock>()).sock_type = 2;
            assert_eq!(a20_net_socket_buf_ceiling(sock(), 1), 65535);
            assert_eq!(a20_net_socket_buf_ceiling(sock(), 0), 262144);
            assert_eq!(a20_net_socket_buf_ceiling(ptr::null_mut(), 1), 0);
        }
    }

    #[test]
    fn the_value_in_force_prefers_a_lower_stored_value() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            assert_eq!(a20_net_socket_buf_in_force(sock(), 1), 93440);
            (*s).snd_buf = 16384;
            assert_eq!(a20_net_socket_buf_in_force(sock(), 1), 16384);
            (*s).snd_buf = 1000000;
            assert_eq!(a20_net_socket_buf_in_force(sock(), 1), 93440);
            assert!(T_SOCK_LOCK >= 3);
        }
    }

    #[test]
    fn buf_apply_clamps_and_announces_only_a_shrink() {
        unsafe {
            let _serial = serial();
            reset();
            /* LISTEN: nothing at all. */
            T_IS_LISTEN = 1;
            a20_net_inet_tcp_buf_apply(1000, 1000, 8usize as *mut c_void);
            assert_eq!((T_SND, T_WND_LIMIT), (0, 0));
            /* A null pcb: nothing. */
            reset();
            a20_net_inet_tcp_buf_apply(1000, 1000, ptr::null_mut());
            assert_eq!((T_SND, T_WND_LIMIT), (0, 0));

            /* The send ceiling clamps, and is assigned outright. */
            reset();
            a20_net_inet_tcp_buf_apply(1000000, 0, 8usize as *mut c_void);
            assert_eq!(T_SND, 93440);

            /* A request clamped to the configured ceiling needs no cap. */
            reset();
            a20_net_inet_tcp_buf_apply(0, 1000000, 8usize as *mut c_void);
            assert_eq!(T_WND_LIMIT, 0);
            /* Raising rcv_wnd above the ceiling does not announce. */
            T_RCV_WND = 100;
            a20_net_inet_tcp_buf_apply(0, 1000, 8usize as *mut c_void);
            assert_eq!((T_RCV_WND, T_RECVED), (100, 0));

            /* A live connection keeps its already-advertised window: only
             * wnd_limit moves, and tcp_recved() caps it as data is consumed. */
            reset();
            T_RCV_WND = 60000;
            T_RCV_ANN = 30000;
            a20_net_inet_tcp_buf_apply(0, 1000, 8usize as *mut c_void);
            assert_eq!((T_RCV_WND, T_RCV_ANN), (60000, 30000));
            assert_eq!(T_WND_LIMIT, 1000);

            /* A pre-handshake pcb gets the value written, not announced: no
             * tcp_output() on SYN_SENT. */
            reset();
            T_RCV_WND = 60000;
            T_RCV_ANN = 60000;
            T_ESTABLISHED = 0;
            a20_net_inet_tcp_buf_apply(0, 1000, 8usize as *mut c_void);
            assert_eq!((T_RCV_WND, T_RCV_ANN), (1000, 1000));
        }
    }

    #[test]
    fn apply_options_covers_every_field_and_the_callbacks() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).tcp_nodelay = 1;
            (*s).tcp_congestion = TCP_CONG_CUBIC as u8;
            (*s).snd_buf = 8192;
            (*s).keepalive = 1;
            (*s).reuseaddr = 1;
            (*s).keep_idle = 10;
            (*s).keep_intvl = 5;
            (*s).keep_cnt = 3;
            a20_net_inet_tcp_apply_options(sock(), 8usize as *mut c_void);
            assert_eq!(T_NAGLE, 1);
            assert_eq!((T_CONG_SET, T_CUBIC_INIT), (TCP_CONG_CUBIC, 1));
            assert_eq!((T_SND, T_KEEPALIVE, T_REUSE), (8192, 1, 1));
            /* The keepalive fields are seconds in the socket, milliseconds in
             * the pcb. */
            assert_eq!((T_KEEP_IDLE, T_KEEP_INTVL, T_KEEP_CNT), (10000, 5000, 3));
            assert_eq!(T_CALLBACKS, 1);

            /* Unset keep timers are left alone. */
            reset();
            a20_net_inet_tcp_apply_options(sock(), 8usize as *mut c_void);
            assert_eq!((T_KEEP_IDLE, T_KEEP_INTVL, T_KEEP_CNT), (0, 0, 0));
            assert_eq!((T_NAGLE, T_KEEPALIVE, T_REUSE), (0, 0, 0));
        }
    }

    #[test]
    fn the_snapshot_reports_effective_values() {
        unsafe {
            let _serial = serial();
            reset();
            let mut snap = Snapshot { ttl: 0, tos: 0, mc_ttl: 0, mc_loop: 0, generation: 0 };
            a20_net_inet_ip_options_snapshot(sock(),
                &mut snap as *mut Snapshot as *mut c_void);
            /* Never set: the Linux TTL default, TOS 0, multicast TTL 1. */
            assert_eq!((snap.ttl, snap.tos, snap.mc_ttl, snap.mc_loop), (64, 0, 1, 0));
            assert_eq!(snap.generation, 7);
            assert_eq!(T_SOCK_LOCK, 1);

            /* Set values win, including an explicit zero TOS. */
            let s = sock().cast::<Sock>();
            (*s).ip_ttl_set = 1;
            (*s).ip_ttl = 32;
            (*s).ip_tos_set = 1;
            (*s).ip_tos = 0x10;
            (*s).mc_ttl = 4;
            (*s).mc_loop = 1;
            a20_net_inet_ip_options_snapshot(sock(),
                &mut snap as *mut Snapshot as *mut c_void);
            assert_eq!((snap.ttl, snap.tos, snap.mc_ttl, snap.mc_loop),
                       (32, 0x10, 4, 1));

            /* The effective getter tolerates null outputs. */
            let mut ttl = 0u8;
            let mut tos = 0u8;
            let mut mc = 0u8;
            a20_net_inet_ip_effective(sock(), &mut ttl, &mut tos, &mut mc);
            assert_eq!((ttl, tos, mc), (32, 0x10, 4));
            a20_net_inet_ip_effective(sock(), &mut ttl, ptr::null_mut(),
                                      ptr::null_mut());
            assert_eq!(ttl, 32);
        }
    }

    #[test]
    fn the_lane_lock_retries_on_a_stale_generation() {
        unsafe {
            let _serial = serial();
            reset();
            /* The first lock sees generation 9, the snapshot saw 7: retry. */
            T_GEN_SCRIPT[0] = 9;
            let mut snap = Snapshot { ttl: 0, tos: 0, mc_ttl: 0, mc_loop: 0, generation: 0 };
            let flags = a20_net_inet_socket_ip_lane_lock(
                sock(), &mut snap as *mut Snapshot as *mut c_void);
            assert_eq!(flags, 3);
            assert_eq!(T_LANE_LOCK, 2);
            assert_eq!(T_LANE_UNLOCK, 1);
            assert_eq!(snap.generation, 9);

            /* A stable generation takes one lock. */
            reset();
            let mut snap = Snapshot { ttl: 0, tos: 0, mc_ttl: 0, mc_loop: 0, generation: 0 };
            a20_net_inet_socket_ip_lane_lock(sock(),
                &mut snap as *mut Snapshot as *mut c_void);
            assert_eq!((T_LANE_LOCK, T_LANE_UNLOCK), (1, 0));
        }
    }

    #[test]
    fn the_reapply_targets_each_pcb_kind() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).udp = 8usize as *mut c_void;
            (*s).raw = 9usize as *mut c_void;
            (*s).tcp = 10usize as *mut c_void;
            let snap = Snapshot { ttl: 32, tos: 0x10, mc_ttl: 4, mc_loop: 1,
                                  generation: 0 };
            a20_net_inet_ip_opts_apply_snapshot_locked(
                sock(), &snap as *const Snapshot as *const c_void);
            assert_eq!(T_UDP_APPLY, [32, 0x10, 4, 1]);
            assert_eq!(T_RAW_APPLY, [32, 0x10, 4, 1]);
            assert_eq!(T_TCP_APPLY, [32, 0x10]);
        }
    }

    #[test]
    fn the_lock_taking_apply_uses_the_shared_guard_only_for_datagrams() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sock().cast::<Sock>();
            (*s).udp = 8usize as *mut c_void;
            a20_net_inet_ip_opts_apply(sock());
            assert_eq!(T_SHARED, [1, 0]);
            assert_eq!(T_UDP_APPLY[0], 64);
            assert_eq!((T_LANE_LOCK, T_LANE_UNLOCK), (1, 1));

            reset();
            let s = sock().cast::<Sock>();
            (*s).raw = 9usize as *mut c_void;
            a20_net_inet_ip_opts_apply(sock());
            assert_eq!(T_SHARED, [0, 1]);
            assert_eq!(T_RAW_APPLY[0], 64);

            /* A TCP pcb takes no shared guard. */
            reset();
            let s = sock().cast::<Sock>();
            (*s).tcp = 10usize as *mut c_void;
            a20_net_inet_ip_opts_apply(sock());
            assert_eq!(T_SHARED, [0, 0]);
            assert_eq!(T_TCP_APPLY[0], 64);
            assert_eq!(T_LANE_UNLOCK, 1);
        }
    }
}
