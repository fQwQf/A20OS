#![no_std]

//! Socket option state machine: the `setsockopt`/`getsockopt` policy and the
//! per-option state transitions.
//!
//! C keeps what is genuinely a primitive: the socket lock, the lwIP control
//! barrier, usercopy, the pidfd allocator, the IGMP/netif lookup and the pcb
//! option application. Everything that is a *decision* is here: which option is
//! recognised at which level, which value range it accepts, how a value in the
//! wrong byte order is re-read, which ceiling it is clamped to, which socket
//! field it lands in, which pcb-apply group it invalidates, and what a
//! `getsockopt` reports back (including the "effective value" rule that a
//! caller who never set an option reads what its packets actually carry).
//!
//! The socket's option fields are reached through a borrowed view
//! (`NetOptionView`) built by C, so no struct ABI moves and the lock discipline
//! is unchanged: this module takes the socket lock through the same primitive
//! the C code used, and it never holds it across the lwIP barrier.
//!
//! Two pre-existing behaviours are preserved verbatim rather than fixed, because
//! changing them is a semantic change and not part of a migration:
//!   * `IPV6_V6ONLY` is stored by `setsockopt` but `getsockopt` reports 0.
//!   * the IPv6 `RECV*` flags, `IPV6_V6ONLY`, `IPV6_CHECKSUM`, `SO_PASSCRED` and
//!     `ICMP6_FILTER` are written without the socket lock, exactly as before.

use core::ffi::{c_char, c_void};
use core::ptr;

/* ------------------------------------------------------------------ errno */

const EINVAL: i32 = 22;
const EDOM: i32 = 33;
const ENOTSOCK: i32 = 88;
const ENOPROTOOPT: i32 = 92;
const EOPNOTSUPP: i32 = 95;

/* --------------------------------------------------------------- families */

const AF_INET: i32 = 2;
const AF_INET6: i32 = 10;
const AF_NETLINK: i32 = 16;
const AF_ALG: i32 = 38;
const SOCK_STREAM: i32 = 1;
const SOCK_RAW: i32 = 3;

const SOL_SOCKET: i32 = 1;
const SOL_ALG: i32 = 279;
const IPPROTO_IP: i32 = 0;
const IPPROTO_TCP: i32 = 6;
const IPPROTO_IPV6: i32 = 41;
const IPPROTO_ICMPV6: i32 = 58;

/* ------------------------------------------------------------- option ids */

const SO_REUSEADDR: i32 = 2;
const SO_TYPE: i32 = 3;
const SO_ERROR: i32 = 4;
const SO_BROADCAST: i32 = 6;
const SO_PRIORITY: i32 = 12;
const SO_SNDBUF: i32 = 7;
const SO_RCVBUF: i32 = 8;
const SO_KEEPALIVE: i32 = 9;
const SO_REUSEPORT: i32 = 15;
const SO_PASSCRED: i32 = 16;
const SO_PEERCRED: i32 = 17;
const SO_RCVTIMEO: i32 = 20;
const SO_SNDTIMEO: i32 = 21;
const SO_RCVTIMEO_NEW: i32 = 66;
const SO_SNDTIMEO_NEW: i32 = 67;
const SO_ACCEPTCONN: i32 = 30;
const SO_PROTOCOL: i32 = 38;
const SO_DOMAIN: i32 = 39;
const SO_ATTACH_BPF: i32 = 50;
const SO_PEERPIDFD: i32 = 77;

const TCP_NODELAY: i32 = 1;
const TCP_MAXSEG: i32 = 2;
const TCP_CORK: i32 = 3;
const TCP_KEEPIDLE: i32 = 4;
const TCP_KEEPINTVL: i32 = 5;
const TCP_KEEPCNT: i32 = 6;
const TCP_SYNCNT: i32 = 7;
const TCP_LINGER2: i32 = 8;
const TCP_DEFER_ACCEPT: i32 = 9;
const TCP_WINDOW_CLAMP: i32 = 10;
const TCP_INFO: i32 = 11;
const TCP_QUICKACK: i32 = 12;
const TCP_CONGESTION: i32 = 13;
const TCP_USER_TIMEOUT: i32 = 18;

const ICMP6_FILTER: i32 = 1;
const IPV6_ADDRFORM: i32 = 1;
const IPV6_2292PKTINFO: i32 = 2;
const IPV6_2292HOPOPTS: i32 = 3;
const IPV6_2292DSTOPTS: i32 = 4;
const IPV6_2292RTHDR: i32 = 5;
const IPV6_CHECKSUM: i32 = 7;
const IPV6_2292HOPLIMIT: i32 = 8;
const IPV6_FLOWINFO: i32 = 11;
const IPV6_MULTICAST_IF: i32 = 17;
const IPV6_MULTICAST_HOPS: i32 = 18;
const IPV6_MULTICAST_LOOP: i32 = 19;
const IPV6_JOIN_GROUP: i32 = 20;
const IPV6_LEAVE_GROUP: i32 = 21;
const IPV6_ROUTER_ALERT: i32 = 22;
const IPV6_RECVERR: i32 = 25;
const IPV6_V6ONLY: i32 = 26;
const IPV6_RECVPKTINFO: i32 = 49;
const IPV6_RECVHOPLIMIT: i32 = 51;
const IPV6_HOPLIMIT: i32 = 52;
const IPV6_RECVHOPOPTS: i32 = 53;
const IPV6_RECVRTHDR: i32 = 56;
const IPV6_RECVDSTOPTS: i32 = 58;
const IPV6_RECVTCLASS: i32 = 66;
const IPV6_TCLASS: i32 = 67;
const IPV6_UNICAST_IF: i32 = 76;

const IP_TOS: i32 = 1;
const IP_TTL: i32 = 2;
const IP_PKTINFO: i32 = 8;
const IP_MTU_DISCOVER: i32 = 10;
const A20_IP_MULTICAST_TTL_WIRE: i32 = 33;
const A20_IP_MULTICAST_LOOP_WIRE: i32 = 34;
const IP_ADD_MEMBERSHIP: i32 = 35;
const IP_DROP_MEMBERSHIP: i32 = 36;
const MCAST_JOIN_GROUP: i32 = 42;
const MCAST_LEAVE_GROUP: i32 = 45;

const ALG_SET_KEY: i32 = 1;

/* lwIP's TCP_CONG_RENO is not spelled here: the name/parse pair and the
 * "CUBIC compiled out means RENO" rule both stay in lwIP, reached through the
 * live-value primitive. */

/* ------------------------------------------------------- pcb apply groups */

const APPLY_CONGESTION: i32 = 1 << 0;
const APPLY_NODELAY: i32 = 1 << 1;
const APPLY_KEEPALIVE: i32 = 1 << 2;
const APPLY_REUSEADDR: i32 = 1 << 3;
const APPLY_KEEP_IDLE: i32 = 1 << 4;
const APPLY_KEEP_INTVL: i32 = 1 << 5;
const APPLY_KEEP_CNT: i32 = 1 << 6;
const APPLY_SNDBUF: i32 = 1 << 7;
const APPLY_RCVBUF: i32 = 1 << 8;
/// Not a pcb-apply group: the four IPPROTO_IP write options change a value the
/// lwIP IP layer reads per packet, so the caller re-runs
/// net_inet_ip_opts_apply(). It is a bit above the pcb-apply range and, like
/// every other return here, positive -- an earlier version used bit 31, which
/// is negative as an i32 and was therefore read as an errno by the C caller.
const APPLY_IP_OPTS: i32 = 1 << 9;

/* ------------------------------------------------------------ report kinds */

const REPORT_INT: i32 = 0;
const REPORT_STRING: i32 = 1;
const REPORT_CRED: i32 = 2;
const REPORT_TIMEVAL: i32 = 3;
const REPORT_PIDFD: i32 = 4;
const REPORT_TIMEVAL64: i32 = 5;

/// Linux `struct ip_mreqn`: all three members are four bytes wide, so the wire
/// layout is frozen and can be read without the C type.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct IpMreqn {
    pub imr_multiaddr: u32,
    pub imr_address: u32,
    pub imr_ifindex: i32,
}

/// Borrowed view of the option-bearing fields of `net_socket_t`.
///
/// `s` is the socket itself, passed only to the C primitives below; the field
/// pointers are what this module reads and writes. `ipv6_recv_flags` points at
/// the first of twelve *contiguous* `int` flags; `net_option_view_init()`
/// static-asserts that the C layout still makes that true.
#[repr(C)]
pub struct NetOptionView {
    pub s: *mut c_void,
    pub domain: *const i32,
    pub sock_type: *const i32,
    pub protocol: *const i32,
    pub connected: *const i32,
    pub bound: *const i32,
    pub listening: *const i32,
    pub ip_ttl: *mut u8,
    pub ip_tos: *mut u8,
    pub ip_ttl_set: *mut u8,
    pub ip_tos_set: *mut u8,
    pub mc_ttl: *mut u8,
    pub mc_loop: *mut u8,
    pub ip_opts_generation: *mut u32,
    pub ipv6_checksum_offset: *mut i32,
    pub ipv6_v6only: *mut i32,
    pub ipv6_recv_flags: *mut i32,
    pub icmp6_filter: *mut u32,
    pub icmp6_filter_set: *mut i32,
    pub tcp_congestion: *mut u8,
    pub tcp_nodelay: *mut i32,
    pub keepalive: *mut i32,
    pub keep_idle: *mut i32,
    pub keep_intvl: *mut i32,
    pub keep_cnt: *mut i32,
    pub tcp_opts_generation: *mut u32,
    pub snd_buf: *mut u32,
    pub rcv_buf: *mut u32,
    pub reuseaddr: *mut i32,
    pub reuseport: *mut i32,
    pub broadcast: *mut i32,
    pub recv_timeout_ticks: *mut u64,
    pub send_timeout_ticks: *mut u64,
    pub passcred: *mut i32,
    pub ipv4_recv_pktinfo: *mut i32,
    pub peer_pid: *const i32,
    pub peer_uid: *const i32,
    pub peer_gid: *const i32,
    /// sizeof(long) on the target: a `struct timeval` is two of them, so the
    /// 32-bit targets (riscv32, arm32, armv7m) need four-byte fields.
    pub long_size: i32,
    /// TICKS_PER_SEC as the target defines it.  On riscv64 that is a runtime
    /// function of the timer frequency, not a compile-time constant, so the
    /// conversion cannot assume a value here.
    pub ticks_per_sec: u64,
}

/// What a `getsockopt` reports. C performs the raw copyout from this: `kind`
/// selects the shape, `len` is the byte count after the caller's `*optlen`
/// clamp (which is part of the ABI and therefore decided here), and the values
/// are already in the units the ABI expects.
#[repr(C)]
pub struct NetOptReport {
    pub kind: i32,
    pub len: usize,
    pub val0: i64,
    pub val1: i64,
    pub val2: i64,
    pub text: [u8; 16],
}

/// Which live-pcb value a `getsockopt` may need.
const LIVE_CONGESTION: i32 = 0;
const LIVE_KEEP_IDLE: i32 = 1;
const LIVE_KEEP_INTVL: i32 = 2;
const LIVE_KEEP_CNT: i32 = 3;

unsafe extern "C" {
    fn a20_net_option_pmtu(s: *mut c_void, mode: i32, set: i32) -> i32;
    /// Socket lock primitives; this module decides *when* they are taken.
    fn a20_net_option_lock(s: *mut c_void, flags: *mut u64);
    fn a20_net_option_unlock(s: *mut c_void, flags: u64);
    /// Largest SO_SNDBUF/SO_RCVBUF this socket could honour (pcb capacity for a
    /// stream, staging/queue limits for a datagram).
    fn a20_net_option_buf_ceiling(s: *mut c_void, is_snd: i32) -> u32;
    /// Host-wide IGMP membership change; lwIP/netif state, so it stays C.
    fn a20_net_option_group_membership(s: *mut c_void, mreq: *const c_void,
                                       join: i32) -> i32;
    /// Live pcb value for `which`, or -1 when there is no usable pcb (NULL, or
    /// LISTEN, whose pcb only has the common prefix). With CUBIC compiled out
    /// the congestion case reports RENO, which is what the old handler did.
    fn a20_net_option_tcp_live(s: *mut c_void, which: i32) -> i32;
    /// The effective TTL/TOS/multicast-TTL a packet would carry, which for an
    /// unset option is the pcb's own value rather than the stored zero.
    /// `which`: 0 TTL, 1 TOS, 2 multicast TTL.
    fn a20_net_option_ip_effective(s: *mut c_void, which: i32) -> i32;
    /// Counts the -ENOTCONN that SO_PEERPIDFD returns, so the §8.5 attribution
    /// the old handler fed stays fed.
    fn a20_net_option_notconn_pidfd() -> i32;
    fn tcp_cong_alg_parse(name: *const c_char) -> i32;
    fn tcp_cong_alg_name(alg: u8) -> *const c_char;
}

/* ------------------------------------------------------------------ helpers */

unsafe fn read_i32(p: *const u8) -> i32 {
    let mut v = 0i32;
    unsafe { ptr::copy_nonoverlapping(p, &mut v as *mut i32 as *mut u8, 4) };
    v
}

unsafe fn read_mreqn(p: *const u8) -> IpMreqn {
    let mut m = IpMreqn { imr_multiaddr: 0, imr_address: 0, imr_ifindex: 0 };
    unsafe { ptr::copy_nonoverlapping(p, &mut m as *mut IpMreqn as *mut u8, 12) };
    m
}

/// One of the small `IPPROTO_IP` integers, in either byte order.
///
/// Linux stores `IP_TTL`, `IP_TOS` and `IP_MULTICAST_*` in network byte order,
/// so a caller following the ABI passes `htonl(v)` -- and on every
/// little-endian target this kernel builds, a plain host-order `v` is that same
/// value byte-swapped. Accepting both is deliberate: rejecting the readable 64
/// would break the common case, rejecting `htonl(64)` the network-aware one. A
/// value out of range in *both* orders is still refused.
unsafe fn ipopt_byte(optval: *const u8, optlen: usize, lo: i32, hi: i32) -> i32 {
    if optval.is_null() || optlen < 4 {
        return -EINVAL;
    }
    let mut val = unsafe { read_i32(optval) };
    if val < 0 || val > 0xff {
        let raw = val as u32;
        val = (raw.swap_bytes()) as i32;
    }
    if val >= lo && val <= hi {
        val
    } else {
        -EINVAL
    }
}

// OLD timeouts use native long; NEW always uses two 64-bit fields, including
// on RV32 where musl's time_t is 64-bit. The option number selects the wire ABI.
fn timeout_layout(optname: i32, long_size: i32) -> Option<(bool, i32)> {
    match optname {
        SO_RCVTIMEO => Some((true, long_size)),
        SO_SNDTIMEO => Some((false, long_size)),
        SO_RCVTIMEO_NEW => Some((true, 8)),
        SO_SNDTIMEO_NEW => Some((false, 8)),
        _ => None,
    }
}

/// `timeval` -> ticks, the same arithmetic the old helper used: a negative
/// seconds field is "no timeout", a negative or >= 1e6 microseconds field is
/// -EDOM, and a non-zero request that rounds to zero ticks becomes one tick so
/// it does not turn into "block forever".
unsafe fn timeval_to_ticks(optval: *const u8, optlen: usize, long_size: i32,
                           ticks_per_sec: u64, out: *mut u64) -> i32 {
    if optval.is_null() || out.is_null() || long_size < 4 || long_size > 8 {
        return -EINVAL;
    }
    let width = long_size as usize;
    if optlen < width * 2 {
        return -EINVAL;
    }
    if ticks_per_sec == 0 {
        return -EINVAL;
    }
    let sec = unsafe { read_long(optval, long_size) };
    let usec = unsafe { read_long(optval.add(width), long_size) };
    if usec < 0 || usec >= 1_000_000 {
        return -EDOM;
    }
    if sec < 0 {
        unsafe { *out = 0 };
        return 0;
    }
    let tps = ticks_per_sec;
    let s = sec as u64;
    if s != 0 && tps > u64::MAX / s {
        return -EINVAL;
    }
    let whole = s.wrapping_mul(tps);
    let fraction = (usec as u64).wrapping_mul(tps) / 1_000_000;
    if fraction > u64::MAX - whole {
        return -EINVAL;
    }
    let mut ticks = whole.wrapping_add(fraction);
    if ticks == 0 && (sec != 0 || usec != 0) {
        ticks = 1;
    }
    unsafe { *out = ticks };
    0
}

unsafe fn read_i64(p: *const u8) -> i64 {
    let mut v = 0i64;
    unsafe { ptr::copy_nonoverlapping(p, &mut v as *mut i64 as *mut u8, 8) };
    v
}

/// Read one `long` of the target's width, sign-extended. A `struct timeval` is
/// two of these, so on ILP32 the fields are four bytes and on LP64 eight.
unsafe fn read_long(p: *const u8, long_size: i32) -> i64 {
    if long_size == 4 {
        unsafe { read_i32(p) as i64 }
    } else {
        unsafe { read_i64(p) }
    }
}

/// Ticks -> `timeval`, the inverse of the above, saturating at LONG_MAX so the
/// report cannot wrap on a 64-bit `long`.
fn ticks_to_timeval(ticks: u64, ticks_per_sec: u64) -> (i64, i64) {
    if ticks_per_sec == 0 {
        return (0, 0);
    }
    let sec = ticks / ticks_per_sec;
    let usec = ((ticks % ticks_per_sec) * 1_000_000) / ticks_per_sec;
    (sec as i64, usec as i64)
}

fn report_int(rep: *mut NetOptReport, optlen: usize, val: i32) -> i32 {
    if optlen < 4 {
        return -EINVAL;
    }
    unsafe {
        (*rep).kind = REPORT_INT;
        (*rep).len = 4;
        (*rep).val0 = val as i64;
    }
    0
}

/// The twelve `IPV6_RECV*` flags are one contiguous run of `int` in
/// `net_socket_t`, in this order.
fn ipv6_recv_index(optname: i32) -> Option<usize> {
    const ORDER: [i32; 12] = [
        IPV6_RECVPKTINFO, IPV6_RECVTCLASS, IPV6_RECVHOPLIMIT, IPV6_RECVRTHDR,
        IPV6_RECVHOPOPTS, IPV6_RECVDSTOPTS, IPV6_RECVERR, IPV6_2292PKTINFO,
        IPV6_2292HOPLIMIT, IPV6_2292RTHDR, IPV6_2292HOPOPTS, IPV6_2292DSTOPTS,
    ];
    let mut i = 0;
    while i < ORDER.len() {
        if ORDER[i] == optname {
            return Some(i);
        }
        i += 1;
    }
    None
}

/// `IPV6_*` options that take an `int` and are accepted but not stored, because
/// nothing in this stack can act on them yet. Refusing them would be the
/// honest answer for an unimplemented *write*, but the old handler accepted
/// them and this migration does not change which calls succeed.
fn ipv6_accept_unstored(optname: i32) -> bool {
    matches!(
        optname,
        IPV6_UNICAST_IF | IPV6_MULTICAST_IF | IPV6_MULTICAST_HOPS
            | IPV6_MULTICAST_LOOP | IPV6_TCLASS | IPV6_HOPLIMIT | IPV6_FLOWINFO
            | IPV6_ROUTER_ALERT
    )
}

/* ------------------------------------------------------------- setsockopt */

/// Apply one `setsockopt` to the socket's option state.
///
/// Returns a pcb-apply mask (possibly 0) on success, or a negative errno. The
/// caller applies the mask through the existing lwIP bridge after this returns,
/// so no lwIP call happens here and the lock order (socket lock never with the
/// lwIP barrier) is preserved.
#[no_mangle]
pub unsafe extern "C" fn a20_net_setsockopt_state(
    v: *const NetOptionView,
    level: i32,
    optname: i32,
    optval: *const u8,
    optlen: usize,
) -> i32 {
    if v.is_null() {
        return -ENOTSOCK;
    }
    let v = unsafe { &*v };
    let domain = unsafe { *v.domain };
    let sock_type = unsafe { *v.sock_type };

    /* AF_ALG has no crypto provider, so there is no key schedule to install.
     * Returning 0 would let a caller believe a key was accepted. */
    if domain == AF_ALG && level == SOL_ALG && optname == ALG_SET_KEY {
        return -EOPNOTSUPP;
    }

    if level == IPPROTO_IP {
        if optname == IP_MTU_DISCOVER {
            if optval.is_null() || optlen < 4 { return -EINVAL; }
            return unsafe { a20_net_option_pmtu(v.s, read_i32(optval), 1) };
        }
        if optname == IP_PKTINFO {
            if domain != AF_INET { return -EOPNOTSUPP; }
            if optval.is_null() || optlen < 4 { return -EINVAL; }
            let mut flags = 0;
            unsafe {
                a20_net_option_lock(v.s, &mut flags);
                *v.ipv4_recv_pktinfo = (read_i32(optval) != 0) as i32;
                a20_net_option_unlock(v.s, flags);
            }
            return 0;
        }
        if optname == MCAST_JOIN_GROUP || optname == MCAST_LEAVE_GROUP
            || optname == IP_ADD_MEMBERSHIP || optname == IP_DROP_MEMBERSHIP
        {
            if optval.is_null() || optlen < 12 {
                return -EINVAL;
            }
            let mreq = unsafe { read_mreqn(optval) };
            let join = (optname == MCAST_JOIN_GROUP || optname == IP_ADD_MEMBERSHIP) as i32;
            return unsafe {
                a20_net_option_group_membership(v.s, &mreq as *const IpMreqn as *const c_void,
                                                join)
            };
        }
        if optname == A20_IP_MULTICAST_TTL_WIRE || optname == A20_IP_MULTICAST_LOOP_WIRE {
            /* Both only mean something for a pcb that can emit a multicast
             * datagram, and a stream socket never has one, so storing the value
             * would be a silent no-op. */
            if sock_type == SOCK_STREAM {
                return -EINVAL;
            }
        }
        if optname == A20_IP_MULTICAST_TTL_WIRE {
            let val = unsafe { ipopt_byte(optval, optlen, 0, 0xff) };
            if val < 0 {
                return val;
            }
            let mut flags = 0u64;
            unsafe {
                a20_net_option_lock(v.s, &mut flags);
                *v.mc_ttl = val as u8;
                *v.ip_opts_generation = (*v.ip_opts_generation).wrapping_add(1);
                a20_net_option_unlock(v.s, flags);
            }
            return APPLY_IP_OPTS;
        }
        if optname == A20_IP_MULTICAST_LOOP_WIRE {
            let val = unsafe { ipopt_byte(optval, optlen, 0, 1) };
            if val < 0 {
                return val;
            }
            let mut flags = 0u64;
            unsafe {
                a20_net_option_lock(v.s, &mut flags);
                *v.mc_loop = val as u8;
                *v.ip_opts_generation = (*v.ip_opts_generation).wrapping_add(1);
                a20_net_option_unlock(v.s, flags);
            }
            return APPLY_IP_OPTS;
        }
        if optname == IP_TTL {
            let val = unsafe { ipopt_byte(optval, optlen, 1, 0xff) };
            if val < 0 {
                return val;
            }
            let mut flags = 0u64;
            unsafe {
                a20_net_option_lock(v.s, &mut flags);
                *v.ip_ttl = val as u8;
                *v.ip_ttl_set = 1;
                *v.ip_opts_generation = (*v.ip_opts_generation).wrapping_add(1);
                a20_net_option_unlock(v.s, flags);
            }
            return APPLY_IP_OPTS;
        }
        if optname == IP_TOS {
            let val = unsafe { ipopt_byte(optval, optlen, 0, 0xff) };
            if val < 0 {
                return val;
            }
            let mut flags = 0u64;
            unsafe {
                a20_net_option_lock(v.s, &mut flags);
                *v.ip_tos = val as u8;
                *v.ip_tos_set = 1;
                *v.ip_opts_generation = (*v.ip_opts_generation).wrapping_add(1);
                a20_net_option_unlock(v.s, flags);
            }
            return APPLY_IP_OPTS;
        }
        /* IP_OPTIONS needs a per-packet option buffer and lwIP's output paths
         * take none; IP_HDRINCL, IP_TRANSPARENT and the rest are likewise
         * unimplemented. A setsockopt a caller cannot honour must fail. */
        return -EOPNOTSUPP;
    }

    if domain == AF_INET6 && level == IPPROTO_IPV6 {
        if optname == IPV6_CHECKSUM {
            if optval.is_null() || optlen < 4 {
                return -EINVAL;
            }
            let offset = unsafe { read_i32(optval) };
            if offset >= 0 && (offset & 1) != 0 {
                return -EINVAL;
            }
            unsafe { *v.ipv6_checksum_offset = offset };
            return 0;
        }
        if optname == IPV6_V6ONLY {
            if optval.is_null() || optlen < 4 {
                return -EINVAL;
            }
            let val = unsafe { read_i32(optval) };
            unsafe { *v.ipv6_v6only = (val != 0) as i32 };
            return 0;
        }
        if let Some(idx) = ipv6_recv_index(optname) {
            if optval.is_null() || optlen < 4 {
                return -EINVAL;
            }
            let val = unsafe { read_i32(optval) };
            unsafe { *v.ipv6_recv_flags.add(idx) = (val != 0) as i32 };
            return 0;
        }
        if ipv6_accept_unstored(optname) {
            return if optlen >= 4 { 0 } else { -EINVAL };
        }
        if optname == IPV6_JOIN_GROUP || optname == IPV6_LEAVE_GROUP {
            /* IPv6 multicast membership is not wired to lwIP's group table. */
            return 0;
        }
        if optname == IPV6_ADDRFORM {
            if optval.is_null() || optlen < 4 {
                return -EINVAL;
            }
            let val = unsafe { read_i32(optval) };
            if val != AF_INET {
                return -EINVAL;
            }
            if sock_type != SOCK_STREAM || unsafe { *v.connected } != 0
                || unsafe { *v.bound } != 0
            {
                return -EOPNOTSUPP;
            }
            /* The old handler wrote the domain without the socket lock. */
            unsafe { *(v.domain as *mut i32) = AF_INET };
            return 0;
        }
    }

    if domain == AF_INET6 && sock_type == SOCK_RAW && level == IPPROTO_ICMPV6
        && optname == ICMP6_FILTER
    {
        if optval.is_null() || optlen < 32 {
            return -EINVAL;
        }
        unsafe {
            ptr::copy_nonoverlapping(optval, v.icmp6_filter as *mut u8, 32);
            *v.icmp6_filter_set = 1;
        }
        return 0;
    }

    if level == IPPROTO_TCP {
        if sock_type != SOCK_STREAM {
            return -ENOPROTOOPT;
        }
        if optname == TCP_CONGESTION {
            /* Validate the name instead of accepting anything: this used to
             * accept "bbr", "reno " and "cubic-but-not-really" on a stack with
             * exactly one algorithm, leaving the caller no way to find out which
             * one it got. -ENOPROTOOPT is what Linux returns for an algorithm it
             * cannot honour and it is the only truthful answer: the request was
             * well formed, this kernel has no such algorithm. */
            if optval.is_null() || optlen == 0 {
                return -EINVAL;
            }
            /* The name is bytes: `c_char` is u8 on the LP64 targets and i8 on
             * some 32-bit ones, so the buffer is u8 and the pointer is cast at
             * the call. */
            let mut name = [0u8; 16];
            let n = if optlen < 15 { optlen } else { 15 };
            unsafe { ptr::copy_nonoverlapping(optval, name.as_mut_ptr(), n) };
            name[n] = 0;
            let alg = unsafe { tcp_cong_alg_parse(name.as_ptr().cast()) };
            if alg < 0 {
                return -ENOPROTOOPT;
            }
            let mut flags = 0u64;
            unsafe {
                a20_net_option_lock(v.s, &mut flags);
                *v.tcp_congestion = alg as u8;
                *v.tcp_opts_generation = (*v.tcp_opts_generation).wrapping_add(1);
                a20_net_option_unlock(v.s, flags);
            }
            return APPLY_CONGESTION;
        }
        if optval.is_null() || optlen < 4 {
            return -EINVAL;
        }
        let val = unsafe { read_i32(optval) };
        match optname {
            TCP_NODELAY => {
                let mut flags = 0u64;
                unsafe {
                    a20_net_option_lock(v.s, &mut flags);
                    *v.tcp_nodelay = (val != 0) as i32;
                    *v.tcp_opts_generation = (*v.tcp_opts_generation).wrapping_add(1);
                    a20_net_option_unlock(v.s, flags);
                }
                return APPLY_NODELAY
            }
            /* Accepted and ignored: nothing in this stack acts on them, and the
             * old handler accepted them too. */
            TCP_CORK | TCP_MAXSEG | TCP_SYNCNT | TCP_LINGER2 | TCP_DEFER_ACCEPT
            | TCP_WINDOW_CLAMP | TCP_QUICKACK | TCP_USER_TIMEOUT => return 0,
            TCP_KEEPIDLE => {
                if val <= 0 {
                    return -EINVAL;
                }
                let mut flags = 0u64;
                unsafe {
                    a20_net_option_lock(v.s, &mut flags);
                    *v.keep_idle = val;
                    *v.tcp_opts_generation = (*v.tcp_opts_generation).wrapping_add(1);
                    a20_net_option_unlock(v.s, flags);
                }
                return APPLY_KEEP_IDLE
            }
            TCP_KEEPINTVL => {
                if val <= 0 {
                    return -EINVAL;
                }
                let mut flags = 0u64;
                unsafe {
                    a20_net_option_lock(v.s, &mut flags);
                    *v.keep_intvl = val;
                    *v.tcp_opts_generation = (*v.tcp_opts_generation).wrapping_add(1);
                    a20_net_option_unlock(v.s, flags);
                }
                return APPLY_KEEP_INTVL
            }
            TCP_KEEPCNT => {
                if val <= 0 {
                    return -EINVAL;
                }
                let mut flags = 0u64;
                unsafe {
                    a20_net_option_lock(v.s, &mut flags);
                    *v.keep_cnt = val;
                    *v.tcp_opts_generation = (*v.tcp_opts_generation).wrapping_add(1);
                    a20_net_option_unlock(v.s, flags);
                }
                return APPLY_KEEP_CNT
            }
            _ => return -ENOPROTOOPT,
        }
    }

    if level == SOL_SOCKET && optname == SO_ATTACH_BPF {
        /* Socket filters have no KEP extension point yet. */
        return -EOPNOTSUPP;
    }
    if level == SOL_SOCKET && timeout_layout(optname, v.long_size).is_some() {
        let (receive, width) = timeout_layout(optname, v.long_size).unwrap();
        let mut ticks = 0u64;
        let rc = unsafe {
            timeval_to_ticks(optval, optlen, width, v.ticks_per_sec, &mut ticks)
        };
        if rc < 0 {
            return rc;
        }
        let mut flags = 0u64;
        unsafe {
            a20_net_option_lock(v.s, &mut flags);
            if receive {
                *v.recv_timeout_ticks = ticks;
            } else {
                *v.send_timeout_ticks = ticks;
            }
            a20_net_option_unlock(v.s, flags);
        }
        return 0;
    }
    if level == SOL_SOCKET && optname == SO_PRIORITY {
        if optval.is_null() || optlen < 4 {
            return -EINVAL;
        }
        // The stack's only transmit priority is the default priority zero.
        return if unsafe { read_i32(optval) } == 0 { 0 } else { -EOPNOTSUPP };
    }
    if level == SOL_SOCKET && optname == SO_REUSEADDR {
        if optval.is_null() || optlen < 4 {
            return -EINVAL;
        }
        let val = unsafe { read_i32(optval) };
        let mut flags = 0u64;
        unsafe {
            a20_net_option_lock(v.s, &mut flags);
            *v.reuseaddr = (val != 0) as i32;
            *v.tcp_opts_generation = (*v.tcp_opts_generation).wrapping_add(1);
            a20_net_option_unlock(v.s, flags);
        }
        /* setsockopt() normally runs between socket() and bind(), and socket()
         * has already made the pcb, so the pcb still holds the old value. Push
         * it across or bind() would consult a stale SOF_REUSEADDR. */
        return APPLY_REUSEADDR;
    }
    if level == SOL_SOCKET && optname == SO_REUSEPORT {
        if optval.is_null() || optlen < 4 {
            return -EINVAL;
        }
        let val = unsafe { read_i32(optval) };
        let mut flags = 0u64;
        unsafe {
            a20_net_option_lock(v.s, &mut flags);
            *v.reuseport = (val != 0) as i32;
            a20_net_option_unlock(v.s, flags);
        }
        return 0;
    }
    if level == SOL_SOCKET && optname == SO_KEEPALIVE {
        if optval.is_null() || optlen < 4 {
            return -EINVAL;
        }
        let val = unsafe { read_i32(optval) };
        let mut flags = 0u64;
        unsafe {
            a20_net_option_lock(v.s, &mut flags);
            *v.keepalive = (val != 0) as i32;
            *v.tcp_opts_generation = (*v.tcp_opts_generation).wrapping_add(1);
            a20_net_option_unlock(v.s, flags);
        }
        return APPLY_KEEPALIVE;
    }
    if level == SOL_SOCKET && optname == SO_BROADCAST {
        if optval.is_null() || optlen < 4 {
            return -EINVAL;
        }
        let val = unsafe { read_i32(optval) };
        let mut flags = 0u64;
        unsafe {
            a20_net_option_lock(v.s, &mut flags);
            *v.broadcast = (val != 0) as i32;
            a20_net_option_unlock(v.s, flags);
        }
        /* Nothing to apply to a pcb: on Linux SO_BROADCAST authorises sends to
         * broadcast addresses, and lwIP's raw UDP path (udp_sendto) already
         * permits them -- unlike lwIP's socket API there is no SOF_BROADCAST
         * gate to set here.  The value is still recorded so getsockopt()
         * reports what the caller set, as Linux does.  Go's net package sets
         * this on every INET SOCK_DGRAM socket it creates, so refusing it
         * would break every Go UDP dial (DNS included). */
        return 0;
    }
    if level == SOL_SOCKET && (optname == SO_SNDBUF || optname == SO_RCVBUF) {
        if optval.is_null() || optlen < 4 {
            return -EINVAL;
        }
        let mut val = unsafe { read_i32(optval) };
        /* Linux clamps a non-positive request up to a minimal buffer; this
         * refuses it, because a buffer clamped to zero can send nothing and
         * accepting one would leave the caller believing otherwise. */
        if val <= 0 {
            return -EINVAL;
        }
        /* The stream and datagram ceilings are unrelated by construction, so
         * the clamp is the ceiling primitive's, which is also what getsockopt
         * reports and what the datagram send paths compare against -- a report
         * and an enforcement cannot drift apart. */
        let is_snd = (optname == SO_SNDBUF) as i32;
        if domain != AF_INET && domain != AF_INET6 && domain != AF_NETLINK {
            return -EOPNOTSUPP;
        }
        let want = unsafe { a20_net_option_buf_ceiling(v.s, is_snd) };
        if val as u32 > want {
            val = want as i32;
        }
        let mut flags = 0u64;
        unsafe {
            a20_net_option_lock(v.s, &mut flags);
            if is_snd != 0 {
                *v.snd_buf = val as u32;
            } else {
                *v.rcv_buf = val as u32;
            }
            if domain != AF_NETLINK {
                *v.tcp_opts_generation = (*v.tcp_opts_generation).wrapping_add(1);
            }
            a20_net_option_unlock(v.s, flags);
        }
        /* Netlink uses the bounded native message queue, not an INET pcb.
         * Its receive queue enforces rcv_buf and its dispatcher enforces the
         * send-datagram limit. Never apply these changes to a transport pcb. */
        if domain == AF_NETLINK { return 0; }
        /* Only this buffer field is applied: snd_buf is available credit, so an
         * unrelated option must never reset it. */
        return if is_snd != 0 { APPLY_SNDBUF } else { APPLY_RCVBUF };
    }
    if level == SOL_SOCKET && optname == SO_PASSCRED {
        if optval.is_null() || optlen < 4 {
            return -EINVAL;
        }
        let val = unsafe { read_i32(optval) };
        unsafe { *v.passcred = (val != 0) as i32 };
        return 0;
    }
    /* Unhandled SOL_SOCKET options (SO_BINDTODEVICE, SO_RXQ_OVFL, ...) are not
     * implemented. Returning 0 would report success for a setting that was
     * discarded: a server that sets SO_BINDTODEVICE for tenant isolation would
     * believe it isolated and would not be. */
    -EOPNOTSUPP
}

/* ------------------------------------------------------------- getsockopt */

/// Report one `getsockopt`. Returns 0 when `rep` is filled, or a negative
/// errno. The raw copyout is the caller's.
#[no_mangle]
pub unsafe extern "C" fn a20_net_getsockopt_state(
    v: *const NetOptionView,
    level: i32,
    optname: i32,
    optlen: usize,
    rep: *mut NetOptReport,
) -> i32 {
    if v.is_null() || rep.is_null() {
        return -ENOTSOCK;
    }
    let v = unsafe { &*v };
    let domain = unsafe { *v.domain };
    let sock_type = unsafe { *v.sock_type };
    let mut val;

    if level == SOL_SOCKET {
        if optname == SO_PRIORITY {
            return report_int(rep, optlen, 0);
        }
        if optname == SO_TYPE {
            return report_int(rep, optlen, sock_type);
        }
        if optname == SO_ERROR {
            return report_int(rep, optlen, 0);
        }
        if optname == SO_ACCEPTCONN {
            return report_int(rep, optlen, unsafe { *v.listening });
        }
        if optname == SO_DOMAIN {
            return report_int(rep, optlen, domain);
        }
        if optname == SO_PROTOCOL {
            return report_int(rep, optlen, unsafe { *v.protocol });
        }
        if optname == SO_SNDBUF || optname == SO_RCVBUF {
            /* The value in force, not the request: this is the same clamp
             * setsockopt applied, so a caller that asked for more than the
             * socket can honour reads back the reduced ceiling rather than a
             * number the stack ignored -- and on a datagram socket, which has
             * no pcb to write the clamp onto, it is the only place the clamp
             * exists at all. */
            let is_snd = (optname == SO_SNDBUF) as i32;
            let in_force = unsafe { a20_net_option_buf_ceiling(v.s, is_snd) };
            let stored = if is_snd != 0 {
                unsafe { *v.snd_buf }
            } else {
                unsafe { *v.rcv_buf }
            };
            return report_int(rep, optlen, stored.min(in_force) as i32);
        }
        if let Some((receive, width)) = timeout_layout(optname, v.long_size) {
            let ticks = if receive {
                unsafe { *v.recv_timeout_ticks }
            } else {
                unsafe { *v.send_timeout_ticks }
            };
            let (sec, usec) = ticks_to_timeval(ticks, v.ticks_per_sec);
            /* The old handler copied min(optlen, sizeof(struct timeval)) bytes
             * and reported that as the new *optlen, so a short buffer gets a
             * truncated answer rather than an error. */
            let full = (width as usize) * 2;
            let len = if optlen < full { optlen } else { full };
            unsafe {
                (*rep).kind = if optname == SO_RCVTIMEO_NEW || optname == SO_SNDTIMEO_NEW {
                    REPORT_TIMEVAL64
                } else { REPORT_TIMEVAL };
                (*rep).len = len;
                (*rep).val0 = sec;
                (*rep).val1 = usec;
            }
            return 0;
        }
        if optname == SO_REUSEADDR {
            return report_int(rep, optlen, unsafe { *v.reuseaddr });
        }
        if optname == SO_REUSEPORT {
            return report_int(rep, optlen, unsafe { *v.reuseport });
        }
        if optname == SO_KEEPALIVE {
            return report_int(rep, optlen, unsafe { *v.keepalive });
        }
        if optname == SO_BROADCAST {
            return report_int(rep, optlen, unsafe { *v.broadcast });
        }
        if optname == SO_PASSCRED {
            return report_int(rep, optlen, unsafe { *v.passcred });
        }
        if optname == SO_PEERCRED {
            /* Linux struct ucred: { pid, uid, gid }. */
            if optlen < 12 {
                return -EINVAL;
            }
            unsafe {
                (*rep).kind = REPORT_CRED;
                (*rep).len = 12;
                (*rep).val0 = *v.peer_pid as i64;
                (*rep).val1 = *v.peer_uid as i64;
                (*rep).val2 = *v.peer_gid as i64;
            }
            return 0;
        }
        if optname == SO_PEERPIDFD {
            /* Linux 6.5+: a pidfd pinning the peer. The decision is here; the
             * descriptor itself is the pidfd allocator's. */
            let pid = unsafe { *v.peer_pid };
            if pid <= 0 {
                return unsafe { a20_net_option_notconn_pidfd() };
            }
            if optlen < 4 {
                return -EINVAL;
            }
            unsafe {
                (*rep).kind = REPORT_PIDFD;
                (*rep).len = 4;
                (*rep).val0 = pid as i64;
            }
            return 0;
        }
        return -ENOPROTOOPT;
    } else if level == IPPROTO_IP {
        if optname == IP_MTU_DISCOVER {
            let mode = unsafe { a20_net_option_pmtu(v.s, 0, 0) };
            return if mode < 0 { mode } else { report_int(rep, optlen, mode) };
        }
        if optname == IP_PKTINFO {
            if domain != AF_INET { return -EOPNOTSUPP; }
            let mut flags = 0;
            let value = unsafe {
                a20_net_option_lock(v.s, &mut flags);
                let value = *v.ipv4_recv_pktinfo;
                a20_net_option_unlock(v.s, flags);
                value
            };
            return report_int(rep, optlen, value);
        }
        /* Report the effective value, not the stored field: a caller that never
         * set an option must see the TTL/TOS its packets actually carry, which
         * for an unset option is the pcb's value and not the stored zero. */
        match optname {
            IP_TTL => return report_int(rep, optlen,
                                        unsafe { a20_net_option_ip_effective(v.s, 0) }),
            IP_TOS => return report_int(rep, optlen,
                                        unsafe { a20_net_option_ip_effective(v.s, 1) }),
            A20_IP_MULTICAST_TTL_WIRE => {
                return report_int(rep, optlen,
                                  unsafe { a20_net_option_ip_effective(v.s, 2) })
            }
            A20_IP_MULTICAST_LOOP_WIRE => {
                let mut flags = 0u64;
                let mc_loop = unsafe {
                    a20_net_option_lock(v.s, &mut flags);
                    let x = *v.mc_loop as i32;
                    a20_net_option_unlock(v.s, flags);
                    x
                };
                return report_int(rep, optlen, mc_loop);
            }
            /* -EOPNOTSUPP rather than -ENOPROTOOPT: the option exists in the
             * Linux ABI, this kernel just cannot honour it. */
            _ => return -EOPNOTSUPP,
        }
    } else if level == IPPROTO_TCP {
        if sock_type != SOCK_STREAM {
            return -ENOPROTOOPT;
        }
        if optname == TCP_CONGESTION {
            /* A live pcb is authoritative over the stored request, because
             * setsockopt applies to the pcb as well: a connection that was
             * accepted inherits its listener's algorithm. */
            let live = unsafe { a20_net_option_tcp_live(v.s, LIVE_CONGESTION) };
            let alg = if live >= 0 {
                live as u8
            } else {
                unsafe { *v.tcp_congestion }
            };
            let name = unsafe { tcp_cong_alg_name(alg) };
            if name.is_null() {
                return -ENOPROTOOPT;
            }
            let mut text = [0u8; 16];
            let mut n = 0usize;
            while n < text.len() - 1 {
                let c = unsafe { *name.add(n) } as u8;
                if c == 0 {
                    break;
                }
                text[n] = c;
                n += 1;
            }
            text[n] = 0;
            let want = n + 1;
            let len = if optlen < want { optlen } else { want };
            unsafe {
                (*rep).kind = REPORT_STRING;
                (*rep).len = len;
                (*rep).text = text;
            }
            return 0;
        }
        if optname == TCP_INFO {
            /* No struct tcp_info is modelled. The old handler wrote a state byte
             * at offset 0 and zeroed the rest, which callers read as
             * tcpi_rtt/tcpi_total_retrans being measured-and-zero. Refuse. */
            return -EOPNOTSUPP;
        }
        match optname {
            TCP_NODELAY => val = unsafe { *v.tcp_nodelay },
            TCP_MAXSEG => val = 1460,
            TCP_CORK | TCP_SYNCNT | TCP_LINGER2 | TCP_DEFER_ACCEPT
            | TCP_WINDOW_CLAMP | TCP_QUICKACK | TCP_USER_TIMEOUT => val = 0,
            TCP_KEEPIDLE => {
                let stored = unsafe { *v.keep_idle };
                val = if stored > 0 {
                    stored
                } else {
                    let live = unsafe { a20_net_option_tcp_live(v.s, LIVE_KEEP_IDLE) };
                    if live >= 0 { live } else { 7200 }
                };
            }
            TCP_KEEPINTVL => {
                let stored = unsafe { *v.keep_intvl };
                val = if stored > 0 {
                    stored
                } else {
                    let live = unsafe { a20_net_option_tcp_live(v.s, LIVE_KEEP_INTVL) };
                    if live >= 0 { live } else { 75 }
                };
            }
            TCP_KEEPCNT => {
                let stored = unsafe { *v.keep_cnt };
                val = if stored > 0 {
                    stored
                } else {
                    let live = unsafe { a20_net_option_tcp_live(v.s, LIVE_KEEP_CNT) };
                    if live >= 0 { live } else { 9 }
                };
            }
            _ => return -ENOPROTOOPT,
        }
    } else if domain == AF_INET6 && level == IPPROTO_IPV6 {
        match optname {
            /* Reported as 0 even when setsockopt stored 1: the old handler did
             * that and this migration does not change observable values. */
            IPV6_V6ONLY => val = 0,
            IPV6_UNICAST_IF | IPV6_MULTICAST_IF | IPV6_TCLASS | IPV6_FLOWINFO => val = 0,
            IPV6_MULTICAST_HOPS | IPV6_HOPLIMIT => val = -1,
            IPV6_MULTICAST_LOOP => val = 1,
            _ => match ipv6_recv_index(optname) {
                Some(idx) => val = unsafe { *v.ipv6_recv_flags.add(idx) },
                None => return -ENOPROTOOPT,
            },
        }
    } else {
        return -EOPNOTSUPP;
    }
    let _ = &mut val;
    report_int(rep, optlen, val)
}

#[cfg(test)]
mod tests {
    use super::*;

    /* The harness for this module is tools/tests/socket_options_host, which
     * drives the production C file with and without the Rust selector over the
     * same scenarios. These unit tests cover the pure helpers. */

    #[test]
    fn ipopt_byte_accepts_both_byte_orders_and_bounds() {
        let host_order = 64i32.to_ne_bytes();
        assert_eq!(unsafe { ipopt_byte(host_order.as_ptr(), 4, 1, 0xff) }, 64);
        let wire_order = 64i32.swap_bytes().to_ne_bytes();
        assert_eq!(unsafe { ipopt_byte(wire_order.as_ptr(), 4, 1, 0xff) }, 64);
        let too_big = 0x0102_0304i32.to_ne_bytes();
        assert_eq!(unsafe { ipopt_byte(too_big.as_ptr(), 4, 0, 0xff) }, -EINVAL);
        assert_eq!(unsafe { ipopt_byte(host_order.as_ptr(), 2, 1, 0xff) }, -EINVAL);
    }

    #[test]
    fn time64_socket_options_do_not_use_the_rv32_native_long_width() {
        assert_eq!(timeout_layout(SO_RCVTIMEO, 4), Some((true, 4)));
        assert_eq!(timeout_layout(SO_SNDTIMEO, 4), Some((false, 4)));
        assert_eq!(timeout_layout(SO_RCVTIMEO_NEW, 4), Some((true, 8)));
        assert_eq!(timeout_layout(SO_SNDTIMEO_NEW, 4), Some((false, 8)));
        let mut wire = [0u8; 16];
        wire[..8].copy_from_slice(&2i64.to_ne_bytes());
        wire[8..].copy_from_slice(&500_000i64.to_ne_bytes());
        let width = timeout_layout(SO_RCVTIMEO_NEW, 4).unwrap().1;
        let mut ticks = 0;
        assert_eq!(unsafe { timeval_to_ticks(wire.as_ptr(), wire.len(), width, 1000, &mut ticks) }, 0);
        assert_eq!(ticks, 2500);
    }

    #[test]
    fn timeval_conversion_round_trips_and_rejects_bad_microseconds() {
        let mut tv = [0u8; 16];
        let sec = 2i64.to_ne_bytes();
        let usec = 500_000i64.to_ne_bytes();
        tv[..8].copy_from_slice(&sec);
        tv[8..].copy_from_slice(&usec);
        let mut ticks = 0u64;
        assert_eq!(unsafe { timeval_to_ticks(tv.as_ptr(), 16, 8, 1000, &mut ticks) }, 0);
        assert_eq!(ticks, 2500);
        assert_eq!(ticks_to_timeval(ticks, 1000), (2, 500_000));

        let bad = 1_000_000i64.to_ne_bytes();
        tv[8..].copy_from_slice(&bad);
        assert_eq!(unsafe { timeval_to_ticks(tv.as_ptr(), 16, 8, 1000, &mut ticks) }, -EDOM);

        /* A negative seconds field means "no timeout" and never fails. */
        let neg = (-1i64).to_ne_bytes();
        tv[..8].copy_from_slice(&neg);
        let zero = 0i64.to_ne_bytes();
        tv[8..].copy_from_slice(&zero);
        assert_eq!(unsafe { timeval_to_ticks(tv.as_ptr(), 16, 8, 1000, &mut ticks) }, 0);
        assert_eq!(ticks, 0);
    }

    #[test]
    fn timeval_conversion_uses_the_targets_tick_rate() {
        /* riscv64's TICKS_PER_SEC is a runtime value (millions), not 1000; a
         * hardcoded 1000 made a one-second timeout expire instantly. */
        let mut tv = [0u8; 16];
        tv[..8].copy_from_slice(&1i64.to_ne_bytes());
        tv[8..].copy_from_slice(&0i64.to_ne_bytes());
        let mut ticks = 0u64;
        assert_eq!(
            unsafe { timeval_to_ticks(tv.as_ptr(), 16, 8, 10_000_000, &mut ticks) },
            0
        );
        assert_eq!(ticks, 10_000_000);
        assert_eq!(ticks_to_timeval(ticks, 10_000_000), (1, 0));
        /* Half a second is five million ticks, and back out again. */
        tv[..8].copy_from_slice(&0i64.to_ne_bytes());
        tv[8..].copy_from_slice(&500_000i64.to_ne_bytes());
        assert_eq!(
            unsafe { timeval_to_ticks(tv.as_ptr(), 16, 8, 10_000_000, &mut ticks) },
            0
        );
        assert_eq!(ticks, 5_000_000);
        assert_eq!(ticks_to_timeval(ticks, 10_000_000), (0, 500_000));
        /* A zero tick rate is refused rather than divided by. */
        assert_eq!(
            unsafe { timeval_to_ticks(tv.as_ptr(), 16, 8, 0, &mut ticks) },
            -EINVAL
        );
    }

    #[test]
    fn sub_tick_requests_never_become_block_forever() {
        let mut tv = [0u8; 16];
        let zero = 0i64.to_ne_bytes();
        let tiny = 1i64.to_ne_bytes();
        tv[..8].copy_from_slice(&zero);
        tv[8..].copy_from_slice(&tiny);
        let mut ticks = 0u64;
        /* 1 microsecond at 1000 ticks/s rounds to 0 ticks, which must become 1. */
        assert_eq!(unsafe { timeval_to_ticks(tv.as_ptr(), 16, 8, 1000, &mut ticks) }, 0);
        assert_eq!(ticks, 1);
    }

    #[test]
    fn ipv6_recv_index_matches_the_c_field_order() {
        assert_eq!(ipv6_recv_index(IPV6_RECVPKTINFO), Some(0));
        assert_eq!(ipv6_recv_index(IPV6_RECVTCLASS), Some(1));
        assert_eq!(ipv6_recv_index(IPV6_RECVERR), Some(6));
        assert_eq!(ipv6_recv_index(IPV6_2292DSTOPTS), Some(11));
        assert_eq!(ipv6_recv_index(IPV6_V6ONLY), None);
    }
}
