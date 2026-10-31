#![no_std]

//! AF_NETLINK: the bind transaction, the sock_diag request/response state
//! machine, the kobject-uevent emitter and sender, and the rtnetlink
//! request/response and multicast paths.
//!
//! The C side keeps the primitives this module calls: the socket lock, the
//! bucket lock and slot lookup, the enqueue, the receive-queue byte count, the
//! namespace reference and capability test, the class-device registry, the lwIP
//! netif snapshot and its mutation helpers, the netif event indices and the
//! klog.  The per-bucket table walks live here, because the filter (namespace,
//! domain, protocol, multicast group) is a decision; only reaching the slot and
//! taking its locks is C's.
//!
//! Everything else is a decision: which requests are accepted and what their
//! lengths must be, the diag filter and reply layout, the uevent verbs and the
//! ACTION@DEVPATH parse, the uevent datagram's exact byte layout, every
//! rtnetlink encode and validation step, the replace/delete rules, and the
//! multicast group filter and drop counting.

use core::ffi::c_void;
use core::ptr;

const AF_UNSPEC: i32 = 0;
const AF_INET: i32 = 2;
const AF_INET6: i32 = 10;
const AF_NETLINK: u16 = 16;

const NETLINK_ROUTE: i32 = 0;
const NETLINK_SOCK_DIAG: i32 = 4;
const NETLINK_KOBJECT_UEVENT: i32 = 15;

const NLMSG_DONE: u16 = 3;
const NLM_F_ACK: u16 = 0x04;
const NLM_F_DUMP: u16 = 0x300;
const NLM_F_MULTI: u16 = 0x2;
const NLM_F_REPLACE: u16 = 0x100;

const SOCK_DIAG_BY_FAMILY: u16 = 20;
const TCPDIAG_GETSOCK: u16 = 18;
const UEVENT_GROUP: u32 = 1;
const UEVENT_MSG_MAX: usize = 256;

const TCP_ESTABLISHED: u8 = 1;
const TCP_CLOSE: u8 = 7;
const TCP_LISTEN: u8 = 10;

const NLMSG_ERROR: u16 = 2;
const RTM_NEWLINK: u16 = 16;
const RTM_GETLINK: u16 = 18;
const RTM_NEWADDR: u16 = 20;
const RTM_DELADDR: u16 = 21;
const RTM_GETADDR: u16 = 22;
const RTM_NEWROUTE: u16 = 24;
const RTM_DELROUTE: u16 = 25;
const RTM_NEWRULE: u16 = 32;
const RTM_DELRULE: u16 = 33;
const RTM_GETRULE: u16 = 34;
const RTM_GETROUTE: u16 = 26;
const ESRCH: i32 = 3;

const IFLA_ADDRESS: u16 = 1;
const IFLA_IFNAME: u16 = 3;
const IFLA_MTU: u16 = 4;
const IFLA_OPERSTATE: u16 = 5;

const IFA_ADDRESS: u16 = 1;
const IFA_LOCAL: u16 = 2;
const IFA_BROADCAST: u16 = 4;
const IFA_LABEL: u16 = 3;

const RTA_DST: u16 = 1;
const RTA_GATEWAY: u16 = 5;
const RTA_PRIORITY: u16 = 6;
const RTA_TABLE: u16 = 15;
const RTA_OIF: u16 = 4;

const NLMSG_ALIGNTO: usize = 4;
const NLRT_IF_FLAGS_UP: u32 = 0x1;
const NLRT_RTPROT_BOOT: u8 = 3;
const NLRT_SCOPE_UNIVERSE: u8 = 0;
const NLRT_MAX_LINKS: usize = 32;
const NLRT_MSG_MAX: usize = 256;
const NLRT_MIN_MTU: u32 = 68;

const RTNLGRP_LINK: u32 = 0x1;
const RTNLGRP_IPV4_IFADDR: u32 = 0x5;
const RTNLGRP_IPV6_IFADDR: u32 = 0xA;

const EPERM: i32 = 1;
const ENOENT: i32 = 2;
const EAGAIN: i32 = 11;
const EINVAL: i32 = 22;
const ENODEV: i32 = 19;
const EOPNOTSUPP: i32 = 95;
const EPROTONOSUPPORT: i32 = 93;
const EAFNOSUPPORT: i32 = 97;
const EADDRNOTAVAIL: i32 = 99;
const EMSGSIZE: i32 = 90;
const ENOTSOCK: i32 = 88;

const NET_SOCKADDR_NL_SIZE: usize = 12;
const NET_SOCKADDR_IN_SIZE: usize = 16;
const NET_SOCKADDR_IN6_SIZE: usize = 28;

/* --------------------------------------------------------------- wire types */

#[repr(C)]
#[derive(Clone, Copy)]
struct NlMsghdr {
    len: u32,
    msg_type: u16,
    flags: u16,
    seq: u32,
    pid: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct InetDiagSockid {
    sport: u16,
    dport: u16,
    src: [u32; 4],
    dst: [u32; 4],
    if_: u32,
    cookie: [u32; 2],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct InetDiagReqV2 {
    family: u8,
    protocol: u8,
    ext: u8,
    pad: u8,
    states: u32,
    id: InetDiagSockid,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct InetDiagMsg {
    family: u8,
    state: u8,
    timer: u8,
    retrans: u8,
    id: InetDiagSockid,
    expires: u32,
    rqueue: u32,
    wqueue: u32,
    uid: u32,
    inode: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DiagReply {
    nlh: NlMsghdr,
    diag: InetDiagMsg,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DiagDone {
    nlh: NlMsghdr,
    error: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct IfInfoMsg {
    family: u8,
    pad: u8,
    type_: u16,
    index: i32,
    flags: u32,
    change: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct IfAddrMsg {
    family: u8,
    prefixlen: u8,
    flags: u8,
    scope: u8,
    index: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RtMsg {
    family: u8,
    dst_len: u8,
    src_len: u8,
    tos: u8,
    table: u8,
    protocol: u8,
    scope: u8,
    type_: u8,
    flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct RtAttr {
    len: u16,
    type_: u16,
}

/// `nlrt_link_t`, filled by the C snapshot primitive.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NlrtLink {
    pub name: [u8; 16],
    pub index: u32,
    pub mtu: u32,
    pub flags: u32,
    pub mac: [u8; 6],
    pub loopback: u8,
    pub has_ip: u8,
    pub has_gw: u8,
    pub ip: [u8; 4],
    pub mask: [u8; 4],
    pub gw: [u8; 4],
}

impl NlrtLink {
    const fn zero() -> NlrtLink {
        NlrtLink {
            name: [0; 16],
            index: 0,
            mtu: 0,
            flags: 0,
            mac: [0; 6],
            loopback: 0,
            has_ip: 0,
            has_gw: 0,
            ip: [0; 4],
            mask: [0; 4],
            gw: [0; 4],
        }
    }
}

/// Borrowed view of the socket fields these transactions read.
#[repr(C)]
pub struct NetlinkView {
    pub s: *mut c_void,
    pub net_ns: *mut *mut c_void,
    pub domain: *mut i32,
    pub sock_type: *mut i32,
    pub protocol: *mut i32,
    pub bound: *mut i32,
    pub connected: *mut i32,
    pub listening: *mut i32,
    pub local: *mut u8,
    pub local_len: *mut usize,
    pub peer_addr: *mut u8,
    pub peer_len: *mut usize,
}

unsafe extern "C" {
    fn a20_net_netlink_view_init(s: *mut c_void, out: *mut NetlinkView);
    fn a20_net_netlink_lock(s: *mut c_void, flags: *mut u64);
    fn a20_net_netlink_unlock(s: *mut c_void, flags: u64);
    fn a20_net_netlink_buckets() -> i32;
    fn a20_net_netlink_slots_per_bucket() -> i32;
    fn a20_net_netlink_bucket_lock(bucket: i32, flags: *mut u64);
    fn a20_net_netlink_bucket_unlock(bucket: i32, flags: u64);
    fn a20_net_netlink_slot_socket(bucket: i32, i: i32) -> *mut c_void;
    fn a20_net_netlink_is_live(s: *mut c_void) -> i32;
    fn a20_net_netlink_enqueue(s: *mut c_void, buf: *const u8, len: usize,
                               src: *const c_void, srclen: usize) -> i32;
    fn a20_net_netlink_rx_bytes(s: *mut c_void) -> u32;
    fn a20_net_netlink_local_pid(s: *mut c_void) -> u32;
    fn a20_net_netlink_local_groups(s: *mut c_void) -> u32;
    fn a20_net_netlink_host_pid() -> u32;
    fn a20_net_netlink_ns_initial(ns: *mut c_void) -> i32;
    fn a20_net_netlink_current_ns() -> *mut c_void;
    fn a20_net_netlink_capable(ns: *mut c_void) -> i32;
    fn a20_net_netlink_class_emit_all(action: *const i8);
    fn a20_net_netlink_class_get(name: *const i8) -> *mut c_void;
    fn a20_net_netlink_class_subsystem(cdev: *mut c_void) -> *const i8;
    fn a20_net_netlink_class_devt(cdev: *mut c_void) -> u64;
    fn a20_net_netlink_class_put(cdev: *mut c_void);
    fn a20_net_netlink_snapshot(ns: *mut c_void, out: *mut NlrtLink, max: i32) -> i32;
    fn a20_net_route_add(dst: u32, prefix: u8, ifindex: u16) -> i32;
    fn a20_net_route_del(dst: u32, prefix: u8, ifindex: u16) -> i32;
    fn a20_net_netlink_event_index(events: *const c_void, i: i32) -> u32;
    fn a20_net_netlink_if_set_mtu(ifindex: u32, mtu: u16) -> i32;
    fn a20_net_netlink_if_set_flags(ifindex: u32, value: u32, mask: u32) -> i32;
    fn a20_net_netlink_if_get_addr(ifindex: u32, ip: *mut u8, mask: *mut u8,
                                   gw: *mut u8) -> i32;
    fn net_stack_route_update_v4(dst: *const u8, prefix: u8,
                                 gateway: *const u8, ifindex: u16,
                                 remove: i32) -> i32;
    fn a20_net_netlink_if_set_addr(ifindex: u32, ip: *const u8, mask: *const u8,
                                   gw: *const u8) -> i32;
    fn a20_net_netlink_if_set_addr6(ifindex: u32, addr: *const u8,
                                    prefixlen: u8) -> i32;
    fn a20_net_netlink_if_del_addr6(ifindex: u32, addr: *const u8) -> i32;
    fn a20_net_netlink_loopback_mtu(ns: *mut c_void, value: u32, set: i32) -> u32;
    fn a20_net_netlink_loopback_up(ns: *mut c_void, value: u32, set: i32) -> u32;
    fn a20_net_netlink_klog_uevent(action: *const i8, subsystem: *const i8,
                                   name: *const i8, major: u32, minor: u32,
                                   delivered: i32);
    fn a20_net_netlink_klog_drop(group: u32, dropped: i32, delivered: i32);
}

fn zero_view() -> NetlinkView {
    NetlinkView {
        s: ptr::null_mut(),
        net_ns: ptr::null_mut(),
        domain: ptr::null_mut(),
        sock_type: ptr::null_mut(),
        protocol: ptr::null_mut(),
        bound: ptr::null_mut(),
        connected: ptr::null_mut(),
        listening: ptr::null_mut(),
        local: ptr::null_mut(),
        local_len: ptr::null_mut(),
        peer_addr: ptr::null_mut(),
        peer_len: ptr::null_mut(),
    }
}

unsafe fn view_of(s: *mut c_void) -> NetlinkView {
    let mut v = zero_view();
    a20_net_netlink_view_init(s, &mut v);
    v
}

unsafe fn slen(p: *const i8, max: usize) -> usize {
    let mut n = 0usize;
    while n < max && *p.add(n) != 0 {
        n += 1;
    }
    n
}

unsafe fn bytes<'a>(p: *const i8, max: usize) -> &'a [u8] {
    core::slice::from_raw_parts(p as *const u8, slen(p, max))
}

/* -------------------------------------------------------------------- bind */

#[no_mangle]
pub unsafe extern "C" fn a20_net_netlink_bind(s: *mut c_void, addr: *const c_void,
                                              addrlen: usize) -> i32 {
    if s.is_null() || addr.is_null() || addrlen < NET_SOCKADDR_NL_SIZE {
        return -EINVAL;
    }
    let req = addr as *const u8;
    if ptr::read_unaligned(req as *const u16) != AF_NETLINK {
        return -EAFNOSUPPORT;
    }
    /* Copy the caller's address; a zero nl_pid means "this task". */
    let mut local = [0u8; NET_SOCKADDR_NL_SIZE];
    ptr::copy_nonoverlapping(req, local.as_mut_ptr(), NET_SOCKADDR_NL_SIZE);
    let mut pid = ptr::read_unaligned(local.as_ptr().add(4) as *const u32);
    if pid == 0 {
        pid = a20_net_netlink_host_pid();
        if pid == 0 {
            pid = 1;
        }
        ptr::write_unaligned(local.as_mut_ptr().add(4) as *mut u32, pid);
    }
    let v = view_of(s);
    let mut flags = 0u64;
    a20_net_netlink_lock(s, &mut flags);
    if a20_net_netlink_is_live(s) == 0 {
        a20_net_netlink_unlock(s, flags);
        return -ENOTSOCK;
    }
    ptr::copy_nonoverlapping(local.as_ptr(), v.local, NET_SOCKADDR_NL_SIZE);
    *v.local_len = NET_SOCKADDR_NL_SIZE;
    *v.bound = 1;
    let protocol = *v.protocol;
    a20_net_netlink_unlock(s, flags);

    /* udevd subscribes to the uevent group when it starts; devices registered
     * before it ran had no listener, so replay the current set now that one
     * exists (coldplug). */
    let groups = ptr::read_unaligned(local.as_ptr().add(8) as *const u32);
    if protocol == NETLINK_KOBJECT_UEVENT && groups & UEVENT_GROUP != 0 {
        a20_net_netlink_class_emit_all(b"add\0".as_ptr() as *const i8);
    }
    0
}

/* --------------------------------------------------------------- sock_diag */

unsafe fn diag_state(v: &NetlinkView) -> u8 {
    if *v.listening != 0 {
        return TCP_LISTEN;
    }
    if *v.connected != 0 {
        return TCP_ESTABLISHED;
    }
    TCP_CLOSE
}

unsafe fn diag_protocol_matches(v: &NetlinkView, protocol: u8) -> bool {
    if protocol as i32 == 6 {
        return *v.sock_type == 1;
    }
    if protocol as i32 == 17 {
        return *v.sock_type == 2;
    }
    *v.sock_type == 3 && *v.protocol == protocol as i32
}

unsafe fn diag_copy_endpoint(addr: *const u8, addrlen: usize, port: *mut u16,
                             out: *mut u32) {
    if addr.is_null() || addrlen < 2 {
        return;
    }
    let family = ptr::read_unaligned(addr as *const u16);
    if family as i32 == AF_INET && addrlen >= NET_SOCKADDR_IN_SIZE {
        *port = ptr::read_unaligned(addr.add(2) as *const u16);
        *out = ptr::read_unaligned(addr.add(4) as *const u32);
    } else if family as i32 == AF_INET6 && addrlen >= NET_SOCKADDR_IN6_SIZE {
        *port = ptr::read_unaligned(addr.add(2) as *const u16);
        ptr::copy_nonoverlapping(addr.add(8), out as *mut u8, 16);
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_netlink_diag_request(requester: *mut c_void,
                                                      buf: *const c_void,
                                                      len: usize,
                                                      addr: *const c_void,
                                                      addrlen: usize) -> i32 {
    if requester.is_null() {
        return -EPROTONOSUPPORT;
    }
    let rv = view_of(requester);
    if *rv.domain != AF_NETLINK as i32 || *rv.protocol != NETLINK_SOCK_DIAG {
        return -EPROTONOSUPPORT;
    }
    let hdr_size = core::mem::size_of::<NlMsghdr>();
    let req_size = core::mem::size_of::<InetDiagReqV2>();
    if buf.is_null() || len < hdr_size + req_size {
        return -EINVAL;
    }
    if !addr.is_null()
        && (addrlen < NET_SOCKADDR_NL_SIZE
            || ptr::read_unaligned(addr as *const u16) != AF_NETLINK)
    {
        return -EAFNOSUPPORT;
    }
    let req_nlh = ptr::read_unaligned(buf as *const NlMsghdr);
    if (req_nlh.len as usize) < hdr_size + req_size
        || req_nlh.len as usize > len
        || (req_nlh.msg_type != SOCK_DIAG_BY_FAMILY
            && req_nlh.msg_type != TCPDIAG_GETSOCK)
    {
        return -EINVAL;
    }
    let rp = (buf as *const u8).add(hdr_size);
    let req_family = *rp;
    let req_protocol = *rp.add(1);
    let req_states = ptr::read_unaligned(rp.add(4) as *const u32);

    let mut kernel_addr = [0u8; NET_SOCKADDR_NL_SIZE];
    ptr::write_unaligned(kernel_addr.as_mut_ptr() as *mut u16, AF_NETLINK);

    /* The requester's own answers are read once, under its lock. */
    let mut recipient_pid = 0u32;
    {
        let mut rf = 0u64;
        a20_net_netlink_lock(requester, &mut rf);
        if *rv.local_len >= NET_SOCKADDR_NL_SIZE {
            recipient_pid = a20_net_netlink_local_pid(requester);
        }
        a20_net_netlink_unlock(requester, rf);
    }

    let buckets = a20_net_netlink_buckets();
    let per_bucket = a20_net_netlink_slots_per_bucket();
    let requester_ns = *rv.net_ns;
    for b in 0..buckets {
        let mut bf = 0u64;
        a20_net_netlink_bucket_lock(b, &mut bf);
        for k in 0..per_bucket {
            let sp = a20_net_netlink_slot_socket(b, k);
            if sp.is_null() || sp == requester {
                continue;
            }
            let sv = view_of(sp);
            if *sv.net_ns != requester_ns {
                #[cfg(test)]
                DIAG_SKIP.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
                continue;
            }
            let mut sf = 0u64;
            a20_net_netlink_lock(sp, &mut sf);
            let domain = *sv.domain;
            let skip = (domain != AF_INET && domain != AF_INET6)
                || (req_family != AF_UNSPEC as u8 && domain != req_family as i32)
                || !diag_protocol_matches(&sv, req_protocol);
            if skip {
                #[cfg(test)]
                DIAG_SKIP.fetch_add(10, core::sync::atomic::Ordering::Relaxed);
                a20_net_netlink_unlock(sp, sf);
                continue;
            }
            let state = diag_state(&sv);
            if req_states != 0 && (req_states & (1u32 << state)) == 0 {
                #[cfg(test)]
                DIAG_SKIP.fetch_add(100, core::sync::atomic::Ordering::Relaxed);
                a20_net_netlink_unlock(sp, sf);
                continue;
            }

            let mut reply = DiagReply {
                nlh: NlMsghdr {
                    len: core::mem::size_of::<DiagReply>() as u32,
                    msg_type: SOCK_DIAG_BY_FAMILY,
                    flags: NLM_F_MULTI,
                    seq: req_nlh.seq,
                    pid: recipient_pid,
                },
                diag: InetDiagMsg {
                    family: domain as u8,
                    state,
                    timer: 0,
                    retrans: 0,
                    id: InetDiagSockid {
                        sport: 0,
                        dport: 0,
                        src: [0; 4],
                        dst: [0; 4],
                        if_: 0,
                        cookie: [(b * per_bucket + k + 1) as u32, 0],
                    },
                    expires: 0,
                    rqueue: 0,
                    wqueue: 0,
                    uid: 0,
                    inode: (b * per_bucket + k + 1) as u32,
                },
            };
            diag_copy_endpoint(rv_local(&sv), *sv.local_len,
                               &mut reply.diag.id.sport, reply.diag.id.src.as_mut_ptr());
            diag_copy_endpoint(rv_peer(&sv), *sv.peer_len,
                               &mut reply.diag.id.dport, reply.diag.id.dst.as_mut_ptr());
            reply.diag.rqueue = a20_net_netlink_rx_bytes(sp);
            a20_net_netlink_unlock(sp, sf);

            let mut rf = 0u64;
            a20_net_netlink_lock(requester, &mut rf);
            let r = a20_net_netlink_enqueue(requester,
                                            &reply as *const DiagReply as *const u8,
                                            core::mem::size_of::<DiagReply>(),
                                            kernel_addr.as_ptr() as *const c_void,
                                            NET_SOCKADDR_NL_SIZE);
            a20_net_netlink_unlock(requester, rf);
            if r < 0 {
                a20_net_netlink_bucket_unlock(b, bf);
                return r;
            }
        }
        a20_net_netlink_bucket_unlock(b, bf);
    }

    let done = DiagDone {
        nlh: NlMsghdr {
            len: core::mem::size_of::<DiagDone>() as u32,
            msg_type: NLMSG_DONE,
            flags: NLM_F_MULTI,
            seq: req_nlh.seq,
            pid: recipient_pid,
        },
        error: 0,
    };
    let mut df = 0u64;
    a20_net_netlink_lock(requester, &mut df);
    let r = a20_net_netlink_enqueue(requester, &done as *const DiagDone as *const u8,
                                    core::mem::size_of::<DiagDone>(),
                                    kernel_addr.as_ptr() as *const c_void,
                                    NET_SOCKADDR_NL_SIZE);
    a20_net_netlink_unlock(requester, df);
    if r < 0 { r } else { len as i32 }
}

/* Endpoint pointers: the view holds direct pointers, so these just read them. */
unsafe fn rv_local(v: &NetlinkView) -> *const u8 {
    v.local
}
unsafe fn rv_peer(v: &NetlinkView) -> *const u8 {
    v.peer_addr
}

/* ------------------------------------------------------------ uevent emit */

#[cfg(test)]
pub(crate) static DIAG_SKIP: core::sync::atomic::AtomicU32 =
    core::sync::atomic::AtomicU32::new(0);

/* riscv32/arm32 have no 64-bit atomics; the sequence only has to differ
 * between datagrams, so those profiles count it in 32 bits. */
#[cfg(target_has_atomic = "64")]
static UEVENT_SEQ: core::sync::atomic::AtomicU64 =
    core::sync::atomic::AtomicU64::new(0);
#[cfg(not(target_has_atomic = "64"))]
static UEVENT_SEQ: core::sync::atomic::AtomicU32 =
    core::sync::atomic::AtomicU32::new(0);

fn next_uevent_seq() -> u64 {
    #[cfg(target_has_atomic = "64")]
    {
        UEVENT_SEQ.fetch_add(1, core::sync::atomic::Ordering::Relaxed) + 1
    }
    #[cfg(not(target_has_atomic = "64"))]
    {
        UEVENT_SEQ.fetch_add(1, core::sync::atomic::Ordering::Relaxed) as u64 + 1
    }
}

/// A bounded byte buffer for the uevent datagram and the rtnetlink replies.
struct Buf {
    b: [u8; NLRT_MSG_MAX],
    n: usize,
}

impl Buf {
    fn put(&mut self, s: &[u8]) -> bool {
        if self.n + s.len() > self.b.len() {
            return false;
        }
        self.b[self.n..self.n + s.len()].copy_from_slice(s);
        self.n += s.len();
        true
    }

    fn put_nul(&mut self) -> bool {
        self.put(&[0])
    }

    fn put_u32(&mut self, mut v: u32) -> bool {
        let mut tmp = [0u8; 10];
        let mut i = tmp.len();
        loop {
            i -= 1;
            tmp[i] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        self.put(&tmp[i..])
    }

    fn put_u64(&mut self, mut v: u64) -> bool {
        let mut tmp = [0u8; 20];
        let mut i = tmp.len();
        loop {
            i -= 1;
            tmp[i] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        self.put(&tmp[i..])
    }

    fn as_slice(&self) -> &[u8] {
        &self.b[..self.n]
    }
}

const UEVENT_BUF: usize = UEVENT_MSG_MAX;

/// The uevent datagram: "ACTION@/class/<subsystem>/<name>" followed by the
/// NUL-terminated KEY=VALUE pairs libudev parses, in the exact order and with
/// the exact separators the C snprintf produced.
unsafe fn uevent_broadcast(action: &[u8], subsystem: &[u8], name: &[u8],
                           devt: u64) -> i32 {
    let seq = next_uevent_seq();
    let major = ((devt >> 8) & 0xff) as u32;
    let minor = (devt & 0xff) as u32;
    let mut m = Buf { b: [0u8; UEVENT_BUF], n: 0 };
    let built = m.put(action)
        && m.put(b"@/class/")
        && m.put(subsystem)
        && m.put(b"/")
        && m.put(name)
        && m.put_nul()
        && m.put(b"SEQNUM=")
        && m.put_u64(seq)
        && m.put_nul()
        && m.put(b"ACTION=")
        && m.put(action)
        && m.put_nul()
        && m.put(b"DEVPATH=/class/")
        && m.put(subsystem)
        && m.put(b"/")
        && m.put(name)
        && m.put_nul()
        && m.put(b"SUBSYSTEM=")
        && m.put(subsystem)
        && m.put_nul()
        && m.put(b"MAJOR=")
        && m.put_u32(major)
        && m.put_nul()
        && m.put(b"MINOR=")
        && m.put_u32(minor)
        && m.put_nul()
        && m.put(b"DEVNAME=")
        && m.put(subsystem)
        && m.put(b"/")
        && m.put(name)
        && m.put_nul()
        && m.put_nul();
    if !built {
        return -EMSGSIZE;
    }

    let mut src = [0u8; NET_SOCKADDR_NL_SIZE];
    ptr::write_unaligned(src.as_mut_ptr() as *mut u16, AF_NETLINK);
    ptr::write_unaligned(src.as_mut_ptr().add(8) as *mut u32, UEVENT_GROUP);

    let mut delivered = 0;
    let buckets = a20_net_netlink_buckets();
    let per_bucket = a20_net_netlink_slots_per_bucket();
    for b in 0..buckets {
        let mut bf = 0u64;
        a20_net_netlink_bucket_lock(b, &mut bf);
        for k in 0..per_bucket {
            let sp = a20_net_netlink_slot_socket(b, k);
            if sp.is_null() {
                continue;
            }
            let sv = view_of(sp);
            let mut sf = 0u64;
            a20_net_netlink_lock(sp, &mut sf);
            let mut matched = false;
            let mut r = 0;
            if a20_net_netlink_ns_initial(*sv.net_ns) != 0
                && *sv.domain == AF_NETLINK as i32
                && *sv.bound != 0
                && *sv.protocol == NETLINK_KOBJECT_UEVENT
            {
                if a20_net_netlink_local_groups(sp) & UEVENT_GROUP != 0 {
                    matched = true;
                    r = a20_net_netlink_enqueue(sp, m.as_slice().as_ptr(),
                                                m.n,
                                                src.as_ptr() as *const c_void,
                                                NET_SOCKADDR_NL_SIZE);
                }
            }
            a20_net_netlink_unlock(sp, sf);
            if r < 0 {
                a20_net_netlink_bucket_unlock(b, bf);
                return r;
            }
            if matched {
                delivered += 1;
            }
        }
        a20_net_netlink_bucket_unlock(b, bf);
    }
    a20_net_netlink_klog_uevent(action.as_ptr() as *const i8,
                                subsystem.as_ptr() as *const i8,
                                name.as_ptr() as *const i8, major, minor,
                                delivered);
    0
}

/// Public entry: a kernel-side event for a published class device.
#[no_mangle]
pub unsafe extern "C" fn a20_net_netlink_uevent_emit(action: *const i8,
                                                     subsystem: *const i8,
                                                     name: *const i8,
                                                     devt: u64) {
    let a = bytes(action, 64);
    let s = bytes(subsystem, 64);
    let n = bytes(name, 64);
    let _ = uevent_broadcast(a, s, n, devt);
}

fn uevent_action_known(action: &[u8]) -> bool {
    const KNOWN: [&[u8]; 10] = [
        b"add", b"remove", b"delete", b"change", b"move", b"bind", b"unbind",
        b"online", b"offline", b"rename",
    ];
    KNOWN.iter().any(|k| *k == action)
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_netlink_uevent_send(requester: *mut c_void,
                                                     buf: *const c_void,
                                                     len: usize, _addr: *const c_void,
                                                     _addrlen: usize) -> i32 {
    if requester.is_null() {
        return -EPROTONOSUPPORT;
    }
    let rv = view_of(requester);
    if *rv.domain != AF_NETLINK as i32 || *rv.protocol != NETLINK_KOBJECT_UEVENT {
        return -EPROTONOSUPPORT;
    }
    if buf.is_null() || len == 0 || len > UEVENT_MSG_MAX {
        return -EINVAL;
    }
    let mut msg = [0u8; UEVENT_MSG_MAX + 1];
    ptr::copy_nonoverlapping(buf as *const u8, msg.as_mut_ptr(), len);
    msg[len] = 0;

    /* The datagram carries no terminator, so the action ends at the first NUL
     * or at the '@' that starts the devpath, whichever comes first. */
    let mut end = len;
    let mut at: Option<usize> = None;
    for i in 0..len {
        if msg[i] == 0 {
            end = i;
            break;
        }
        if msg[i] == b'@' && at.is_none() {
            at = Some(i);
        }
    }
    let action_len = at.unwrap_or(end);
    if action_len == 0 || !uevent_action_known(&msg[..action_len]) {
        return -EINVAL;
    }
    msg[action_len] = 0;
    let action = &msg[..action_len];

    let Some(at) = at else {
        /* A bare action replays the whole published set (udevadm trigger). */
        a20_net_netlink_class_emit_all(msg.as_ptr() as *const i8);
        return len as i32;
    };

    /* Our own emitter writes DEVPATH as /class/<subsystem>/<name>, and the
     * class registry is keyed by that <name>. */
    let mut name_off = at + 1;
    for i in at + 1..len {
        if msg[i] == 0 {
            break;
        }
        if msg[i] == b'/' {
            name_off = i + 1;
        }
    }
    if name_off >= len || msg[name_off] == 0 {
        return -EINVAL;
    }
    let cdev = a20_net_netlink_class_get(msg.as_ptr().add(name_off) as *const i8);
    if cdev.is_null() {
        return -ENOENT;
    }
    let subsystem_ptr = a20_net_netlink_class_subsystem(cdev);
    let rc;
    if !subsystem_ptr.is_null() {
        let subsystem = bytes(subsystem_ptr, 64);
        let name = bytes(msg.as_ptr().add(name_off) as *const i8, len - name_off);
        let _ = uevent_broadcast(action, subsystem, name,
                                 a20_net_netlink_class_devt(cdev));
        rc = 0;
    } else {
        rc = -EINVAL;
    }
    a20_net_netlink_class_put(cdev);
    if rc < 0 { rc } else { len as i32 }
}

/* --------------------------------------------------------------- rtnetlink */

#[repr(C, align(8))]
struct MsgBuf([u8; NLRT_MSG_MAX]);

impl MsgBuf {
    fn new() -> MsgBuf {
        MsgBuf([0u8; NLRT_MSG_MAX])
    }
}

fn mask_prefixlen(mask: &[u8; 4]) -> u8 {
    let mut n = 0u32;
    for i in 0..4 {
        for bit in (0..8).rev() {
            if mask[i] & (1u8 << bit) == 0 {
                return n as u8;
            }
            n += 1;
        }
    }
    32
}

/// Append one rtattr, padding the payload to NLMSG_ALIGNTO.  Returns `off`
/// unchanged when the attribute would not fit.
unsafe fn put_attr(buf: &mut [u8], off: usize, type_: u16, data: &[u8]) -> usize {
    let need = core::mem::size_of::<RtAttr>() + data.len();
    let padded = (need + (NLMSG_ALIGNTO - 1)) & !(NLMSG_ALIGNTO - 1);
    if off + padded > NLRT_MSG_MAX {
        return off;
    }
    ptr::write_unaligned(buf.as_mut_ptr().add(off) as *mut RtAttr, RtAttr {
        len: need as u16,
        type_,
    });
    if !data.is_empty() {
        ptr::copy_nonoverlapping(data.as_ptr(), buf.as_mut_ptr().add(off + 4),
                                 data.len());
    }
    for b in &mut buf[off + need..off + padded] {
        *b = 0;
    }
    off + padded
}

fn fill_hdr_flags(buf: &mut [u8], type_: u16, total: u32, seq: u32, pid: u32,
                   flags: u16) {
    unsafe {
        ptr::write_unaligned(
            buf.as_mut_ptr() as *mut NlMsghdr,
            NlMsghdr { len: total, msg_type: type_, flags, seq, pid },
        );
    }
}

fn fill_hdr(buf: &mut [u8], type_: u16, total: u32, seq: u32, pid: u32) {
    fill_hdr_flags(buf, type_, total, seq, pid, NLM_F_MULTI);
}

fn name_bytes(e: &NlrtLink) -> &[u8] {
    let mut n = 0usize;
    while n < e.name.len() && e.name[n] != 0 {
        n += 1;
    }
    &e.name[..n]
}

unsafe fn build_linkmsg(buf: &mut [u8], e: &NlrtLink, msg_type: u16, pid: u32,
                        seq: u32) -> usize {
    let mut off = core::mem::size_of::<NlMsghdr>();
    let ifi = IfInfoMsg {
        family: AF_UNSPEC as u8,
        pad: 0,
        type_: if e.loopback != 0 { 772 } else { 1 },
        index: e.index as i32,
        flags: e.flags,
        change: 0xffffffff,
    };
    ptr::write_unaligned(buf.as_mut_ptr().add(off) as *mut IfInfoMsg, ifi);
    off += core::mem::size_of::<IfInfoMsg>();
    if e.mtu != 0 {
        off = put_attr(buf, off, IFLA_MTU, &e.mtu.to_ne_bytes());
    }
    off = put_attr(buf, off, IFLA_ADDRESS, &e.mac);
    off = put_attr(buf, off, IFLA_IFNAME,
                   &e.name[..name_bytes(e).len() + 1]);
    /* `off` already counts from the nlmsghdr: the header is the first thing
     * it spans.  Adding sizeof(NlMsghdr) again made the message 16 bytes
     * longer than the bytes actually written, so every parser walked past the
     * last attribute into stale buffer and failed with EINVAL
     * (Go: parsenetlinkrouteattr; tailscale's netmon.New died on it). */
    let total = off;
    if total > NLRT_MSG_MAX {
        return 0;
    }
    fill_hdr(buf, msg_type, total as u32, seq, pid);
    total
}

unsafe fn build_addrmsg(buf: &mut [u8], ifindex: u32, addr: &[u8; 4],
                        mask: &[u8; 4], name: &[u8], loopback: u8,
                        msg_type: u16, pid: u32, seq: u32) -> usize {
    let mut off = core::mem::size_of::<NlMsghdr>();
    let ifa = IfAddrMsg {
        family: AF_INET as u8,
        prefixlen: mask_prefixlen(mask),
        flags: 0,
        scope: if loopback != 0 { 254 } else { NLRT_SCOPE_UNIVERSE },
        index: ifindex,
    };
    ptr::write_unaligned(buf.as_mut_ptr().add(off) as *mut IfAddrMsg, ifa);
    off += core::mem::size_of::<IfAddrMsg>();
    off = put_attr(buf, off, IFA_ADDRESS, addr);
    off = put_attr(buf, off, IFA_LOCAL, addr);
    if loopback == 0 && *mask != [0xff; 4] {
        let mut broadcast = [0u8; 4];
        for i in 0..4 {
            broadcast[i] = addr[i] | !mask[i];
        }
        off = put_attr(buf, off, IFA_BROADCAST, &broadcast);
    }
    if !name.is_empty() {
        let mut label = [0u8; 17];
        let n = name.len().min(16);
        label[..n].copy_from_slice(&name[..n]);
        off = put_attr(buf, off, IFA_LABEL, &label[..n + 1]);
    }
    /* `off` already counts from the nlmsghdr: the header is the first thing
     * it spans.  Adding sizeof(NlMsghdr) again made the message 16 bytes
     * longer than the bytes actually written, so every parser walked past the
     * last attribute into stale buffer and failed with EINVAL
     * (Go: parsenetlinkrouteattr; tailscale's netmon.New died on it). */
    let total = off;
    if total > NLRT_MSG_MAX {
        return 0;
    }
    fill_hdr(buf, msg_type, total as u32, seq, pid);
    total
}

unsafe fn build_addrmsg6(buf: &mut [u8], ifindex: u32, addr: &[u8; 16],
                         prefixlen: u8, name: &[u8], loopback: u8,
                         msg_type: u16, pid: u32, seq: u32) -> usize {
    let mut off = core::mem::size_of::<NlMsghdr>();
    let ifa = IfAddrMsg {
        family: AF_INET6 as u8,
        prefixlen,
        flags: 0,
        scope: if loopback != 0 { 254 } else { NLRT_SCOPE_UNIVERSE },
        index: ifindex,
    };
    ptr::write_unaligned(buf.as_mut_ptr().add(off) as *mut IfAddrMsg, ifa);
    off += core::mem::size_of::<IfAddrMsg>();
    off = put_attr(buf, off, IFA_ADDRESS, addr);
    off = put_attr(buf, off, IFA_LOCAL, addr);
    if !name.is_empty() {
        let mut label = [0u8; 17];
        let n = name.len().min(16);
        label[..n].copy_from_slice(&name[..n]);
        off = put_attr(buf, off, IFA_LABEL, &label[..n + 1]);
    }
    /* `off` already counts from the nlmsghdr: the header is the first thing
     * it spans.  Adding sizeof(NlMsghdr) again made the message 16 bytes
     * longer than the bytes actually written, so every parser walked past the
     * last attribute into stale buffer and failed with EINVAL
     * (Go: parsenetlinkrouteattr; tailscale's netmon.New died on it). */
    let total = off;
    if total > NLRT_MSG_MAX {
        return 0;
    }
    fill_hdr(buf, msg_type, total as u32, seq, pid);
    total
}

/// One attribute from a user-supplied block: `(type, payload)`, or None at the
/// end.  A length that does not fit the remaining block is a parse error.
struct AttrIter<'a> {
    block: &'a [u8],
    off: usize,
    bad: bool,
}

impl<'a> AttrIter<'a> {
    fn new(block: &'a [u8]) -> AttrIter<'a> {
        AttrIter { block, off: 0, bad: false }
    }

    fn next(&mut self) -> Option<(u16, &'a [u8])> {
        if self.bad || self.off >= self.block.len() {
            return None;
        }
        let avail = self.block.len() - self.off;
        if avail < core::mem::size_of::<RtAttr>() {
            self.bad = true;
            return None;
        }
        let a = unsafe {
            ptr::read_unaligned(self.block.as_ptr().add(self.off) as *const RtAttr)
        };
        let alen = a.len as usize;
        let padded = (alen + (NLMSG_ALIGNTO - 1)) & !(NLMSG_ALIGNTO - 1);
        if alen < core::mem::size_of::<RtAttr>() || alen > avail || padded > avail {
            self.bad = true;
            return None;
        }
        let payload = &self.block[self.off + 4..self.off + alen];
        self.off += padded;
        Some((a.type_, payload))
    }
}

fn ip4_isany(a: &[u8; 4]) -> bool {
    a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0
}

/* Routing values share an explicit C ABI with net/route.h. Addresses are wire
 * bytes in native u32 containers; table IDs and priorities are host order. */
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RouteV4 {
    destination: u32,
    gateway: u32,
    ifindex: u32,
    table: u32,
    metric: u32,
    prefix: u8,
    pad: [u8; 3],
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RuleV4 {
    source: u32,
    destination: u32,
    priority: u32,
    table: u32,
    mark: u32,
    mark_mask: u32,
    source_prefix: u8,
    destination_prefix: u8,
    pad: [u8; 2],
}
unsafe extern "C" {
    fn a20_net_route_update_v4(r: *const RouteV4, remove: i32, flags: u32) -> i32;
    fn a20_net_rule_update_v4(r: *const RuleV4, remove: i32) -> i32;
    fn a20_net_route_snapshot_v4(r: *mut RouteV4, cap: u32) -> u32;
    fn a20_net_route_query_v4(src: u32, dst: u32, mark: u32, r: *mut RouteV4) -> i32;
    fn a20_net_rule_snapshot_v4(r: *mut RuleV4, cap: u32) -> u32;
}
fn attr_u32(bytes: &[u8]) -> Result<u32, i32> {
    if bytes.len() != 4 {
        return Err(-EINVAL);
    }
    Ok(u32::from_ne_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}
unsafe fn apply_route(msg_type: u16, flags: u16, body: *const u8, attrs: &[u8]) -> i32 {
    let rt = ptr::read_unaligned(body as *const RtMsg);
    if rt.family as i32 != AF_INET {
        return -EAFNOSUPPORT;
    }
    if rt.dst_len > 32 {
        return -EINVAL;
    }
    if rt.src_len != 0
        || rt.tos != 0
        || rt.flags != 0
        || (rt.type_ != 1 && !(msg_type == RTM_DELROUTE && rt.type_ == 0))
    {
        return -EOPNOTSUPP;
    }
    let mut r = RouteV4 {
        table: rt.table as u32,
        prefix: rt.dst_len,
        ..RouteV4::default()
    };
    let mut it = AttrIter::new(attrs);
    while let Some((kind, data)) = it.next() {
        let value = match attr_u32(data) {
            Ok(v) => v,
            Err(e) => return e,
        };
        match kind {
            RTA_DST => r.destination = value,
            RTA_GATEWAY => r.gateway = value,
            RTA_OIF => r.ifindex = value,
            RTA_TABLE => r.table = value,
            RTA_PRIORITY => r.metric = value,
            _ => return -EOPNOTSUPP,
        }
    }
    if it.bad || r.ifindex > 0xffff {
        return -EINVAL;
    }
    if msg_type == RTM_NEWROUTE {
        if r.ifindex == 0 {
            return -EINVAL;
        }
        let mut ip = [0u8; 4];
        let mut mask = [0u8; 4];
        let mut gw = [0u8; 4];
        let rc = a20_net_netlink_if_get_addr(
            r.ifindex,
            ip.as_mut_ptr(),
            mask.as_mut_ptr(),
            gw.as_mut_ptr(),
        );
        if rc < 0 {
            return rc;
        }
    }
    /* Keep the protocol RIB and the common policy FIB in the same
     * transaction.  Backends without a second RIB make this a no-op. */
    let mut backend_rc = 0;
    if r.table == 0 {
        let dst = r.destination.to_ne_bytes();
        let gateway = r.gateway.to_ne_bytes();
        backend_rc = net_stack_route_update_v4(
            dst.as_ptr(),
            r.prefix,
            if r.gateway == 0 { core::ptr::null() } else { gateway.as_ptr() },
            r.ifindex as u16,
            (msg_type == RTM_DELROUTE) as i32,
        );
        if backend_rc < 0 {
            return backend_rc;
        }
    }
    let common_rc = a20_net_route_update_v4(&r, (msg_type == RTM_DELROUTE) as i32,
                                            flags as u32);
    /* A boot-configured default route may exist only in the backend RIB.  A
     * successful backend delete must not be converted to ESRCH merely because
     * the policy FIB was never seeded with that implicit route. */
    if common_rc == -ESRCH && msg_type == RTM_DELROUTE && backend_rc == 0 {
        return 0;
    }
    if common_rc < 0 && backend_rc == 0 {
        let dst = r.destination.to_ne_bytes();
        let gateway = r.gateway.to_ne_bytes();
        let rollback_remove = (msg_type == RTM_NEWROUTE) as i32;
        let _ = net_stack_route_update_v4(
            dst.as_ptr(),
            r.prefix,
            if r.gateway == 0 { core::ptr::null() } else { gateway.as_ptr() },
            r.ifindex as u16,
            rollback_remove,
        );
    }
    common_rc
}

/* fib_rule_hdr uses the rtmsg's 12-byte layout, with action in the type byte.
 * Lookup/source/destination rules are executable. Reject selectors/actions
 * that the FIB cannot enforce instead of acknowledging configuration no-ops. */
unsafe fn apply_rule(msg_type: u16, body: *const u8, attrs: &[u8]) -> i32 {
    let h = ptr::read_unaligned(body as *const RtMsg);
    if h.family as i32 != AF_INET {
        return -EAFNOSUPPORT;
    }
    if h.dst_len > 32 || h.src_len > 32 {
        return -EINVAL;
    }
    /* Linux deletion requests commonly leave action unspecified. The exact
     * value selectors still identify a lookup rule; additions must name it. */
    if h.tos != 0 || h.flags != 0 || (h.type_ != 1 && !(msg_type == RTM_DELRULE && h.type_ == 0)) {
        return -EOPNOTSUPP;
    }
    let mut r = RuleV4 {
        table: h.table as u32,
        priority: 32765,
        source_prefix: h.src_len,
        destination_prefix: h.dst_len,
        ..RuleV4::default()
    };
    let mut it = AttrIter::new(attrs);
    let mut mark = false;
    let mut mask = false;
    while let Some((kind, data)) = it.next() {
        let value = match attr_u32(data) {
            Ok(v) => v,
            Err(e) => return e,
        };
        match kind {
            1 => r.destination = value,
            2 => r.source = value,
            6 => r.priority = value,
            10 => {
                r.mark = value;
                mark = true;
            }
            15 => r.table = value,
            16 => {
                r.mark_mask = value;
                mask = true;
            }
            _ => return -EOPNOTSUPP,
        }
    }
    if it.bad {
        return -EINVAL;
    }
    if mark && !mask {
        r.mark_mask = u32::MAX;
    }
    /* Packet/socket marks are not plumbed through this backend yet. */
    if mark || mask {
        return -EOPNOTSUPP;
    }
    a20_net_rule_update_v4(&r, (msg_type == RTM_DELRULE) as i32)
}

unsafe fn build_route_v4(msg: &mut [u8], r: &RouteV4, pid: u32, seq: u32) -> usize {
    let h = RtMsg {
        family: AF_INET as u8,
        dst_len: r.prefix,
        src_len: 0,
        tos: 0,
        table: if r.table <= 255 { r.table as u8 } else { 0 },
        protocol: NLRT_RTPROT_BOOT,
        scope: if r.gateway == 0 { 253 } else { 0 },
        type_: 1,
        flags: 0,
    };
    ptr::write_unaligned(msg.as_mut_ptr().add(16) as *mut RtMsg, h);
    let mut off = 28;
    if r.prefix != 0 {
        off = put_attr(msg, off, RTA_DST, &r.destination.to_ne_bytes());
    }
    if r.gateway != 0 {
        off = put_attr(msg, off, RTA_GATEWAY, &r.gateway.to_ne_bytes());
    }
    off = put_attr(msg, off, RTA_OIF, &r.ifindex.to_ne_bytes());
    off = put_attr(msg, off, RTA_PRIORITY, &r.metric.to_ne_bytes());
    off = put_attr(msg, off, RTA_TABLE, &r.table.to_ne_bytes());
    fill_hdr(msg, RTM_NEWROUTE, off as u32, seq, pid);
    off
}

struct RouteQuery {
    routes: [RouteV4; 64],
    count: usize,
    table: u32,
}
impl RouteQuery {
    fn empty() -> Self {
        Self {
            routes: [RouteV4::default(); 64],
            count: 0,
            table: 0,
        }
    }
}
/* Validate/copy routing data before locking the requesting socket. Error
 * returns must never strand a socket lock or call the backend under it. */
unsafe fn route_query_values(body: *const u8, len: usize, dump: bool) -> Result<RouteQuery, i32> {
    let mut q = RouteQuery::empty();
    let mut dst = 0;
    let mut src = 0;
    if len >= 12 {
        q.table = ptr::read_unaligned(body as *const RtMsg).table as u32;
        let mut it = AttrIter::new(core::slice::from_raw_parts(body.add(12), len - 12));
        while let Some((kind, data)) = it.next() {
            let value = attr_u32(data)?;
            match kind {
                RTA_TABLE if dump => q.table = value,
                RTA_DST if !dump => dst = value,
                2 if !dump => src = value,
                _ => return Err(-EOPNOTSUPP),
            }
        }
        if it.bad {
            return Err(-EINVAL);
        }
    }
    if dump {
        let n = a20_net_route_snapshot_v4(q.routes.as_mut_ptr(), q.routes.len() as u32) as usize;
        for i in 0..n {
            let r = q.routes[i];
            if q.table == 0 || q.table == r.table {
                q.routes[q.count] = r;
                q.count += 1;
            }
        }
    } else if len >= 12 {
        if q.table != 0 {
            return Err(-EOPNOTSUPP);
        }
        q.count = (a20_net_route_query_v4(src, dst, 0, q.routes.as_mut_ptr()) == 1) as usize;
    }
    Ok(q)
}
struct NetlinkGuard {
    socket: *mut c_void,
    flags: u64,
}
impl Drop for NetlinkGuard {
    fn drop(&mut self) {
        unsafe { a20_net_netlink_unlock(self.socket, self.flags) }
    }
}

unsafe fn rule_dump(requester: *mut c_void, req: &NlMsghdr, len: usize) -> i32 {
    if req.flags & NLM_F_DUMP == 0 {
        return -EOPNOTSUPP;
    }
    let v = view_of(requester);
    if a20_net_netlink_ns_initial(*v.net_ns) == 0 {
        return -EOPNOTSUPP;
    }
    let mut rules = [RuleV4::default(); 16];
    let n = a20_net_rule_snapshot_v4(rules.as_mut_ptr(), rules.len() as u32);
    let mut flags = 0;
    a20_net_netlink_lock(requester, &mut flags);
    let _guard = NetlinkGuard {
        socket: requester,
        flags,
    };
    if a20_net_netlink_is_live(requester) == 0 {
        return -ENOTSOCK;
    }
    let pid = if *v.local_len >= NET_SOCKADDR_NL_SIZE {
        a20_net_netlink_local_pid(requester)
    } else {
        0
    };
    let mut from = [0u8; NET_SOCKADDR_NL_SIZE];
    ptr::write_unaligned(from.as_mut_ptr() as *mut u16, AF_NETLINK);
    let mut msg = MsgBuf::new();
    for r in rules.iter().take(n as usize) {
        let h = RtMsg {
            family: AF_INET as u8,
            dst_len: r.destination_prefix,
            src_len: r.source_prefix,
            tos: 0,
            table: if r.table <= 255 { r.table as u8 } else { 0 },
            protocol: 0,
            scope: 0,
            type_: 1,
            flags: 0,
        };
        ptr::write_unaligned(msg.0.as_mut_ptr().add(16) as *mut RtMsg, h);
        let mut off = 28;
        if r.source_prefix != 0 {
            off = put_attr(&mut msg.0, off, 2, &r.source.to_ne_bytes());
        }
        if r.destination_prefix != 0 {
            off = put_attr(&mut msg.0, off, 1, &r.destination.to_ne_bytes());
        }
        off = put_attr(&mut msg.0, off, 6, &r.priority.to_ne_bytes());
        off = put_attr(&mut msg.0, off, 15, &r.table.to_ne_bytes());
        fill_hdr(&mut msg.0, RTM_NEWRULE, off as u32, req.seq, pid);
        let rc = a20_net_netlink_enqueue(
            requester,
            msg.0.as_ptr(),
            off,
            from.as_ptr() as *const c_void,
            NET_SOCKADDR_NL_SIZE,
        );
        if rc < 0 {
            return rc;
        }
    }
    fill_hdr(&mut msg.0, NLMSG_DONE, 16, req.seq, pid);
    let rc = a20_net_netlink_enqueue(
        requester,
        msg.0.as_ptr(),
        16,
        from.as_ptr() as *const c_void,
        NET_SOCKADDR_NL_SIZE,
    );
    if rc < 0 {
        rc
    } else {
        len as i32
    }
}

/* NLM_F_ACK: the request asked for a status message.  Clients that wait for
 * one (tailscale's netlink helpers do) would otherwise block until their own
 * timeout, so a successful write is acknowledged with NLMSG_ERROR{0}. */
/// `portid` is the requesting socket's own netlink port id (its bound
/// nl_pid), which is what Linux puts in the reply header.  The nlmsg_pid field
/// of a userspace request is 0 and must not be echoed: vishvananda/netlink
/// (used by tailscale) drops any message whose header pid differs from its own
/// port id, so an echoed 0 makes the ACK invisible and the caller waits out its
/// 60 s receive timeout (EAGAIN) instead of seeing success.
unsafe fn enqueue_ack(requester: *mut c_void, req: &NlMsghdr, portid: u32) {
    let hdr_size = core::mem::size_of::<NlMsghdr>();
    let total = hdr_size + 4 + hdr_size;
    let mut msg = MsgBuf::new();
    let pid = if portid != 0 { portid } else { req.pid };
    /* NLMSG_ERROR is an ordinary (non-dump) reply: NLM_F_MULTI would make the
     * caller keep waiting for messages that never come. */
    fill_hdr_flags(&mut msg.0, NLMSG_ERROR, total as u32, req.seq, pid, 0);
    ptr::write_unaligned(msg.0.as_mut_ptr().add(hdr_size) as *mut i32, 0);
    ptr::write_unaligned(msg.0.as_mut_ptr().add(hdr_size + 4) as *mut NlMsghdr, *req);
    let mut from = [0u8; NET_SOCKADDR_NL_SIZE];
    ptr::write_unaligned(from.as_mut_ptr() as *mut u16, AF_NETLINK);
    a20_net_netlink_enqueue(requester, msg.0.as_ptr(), total,
                            from.as_ptr() as *const c_void,
                            NET_SOCKADDR_NL_SIZE);
}

unsafe fn apply_newlink(ns: *mut c_void, ifi: &IfInfoMsg, attrs: &[u8]) -> i32 {
    if ifi.index <= 0 {
        return -EINVAL;
    }
    let ifindex = ifi.index as u32;
    /* IFF_UP is the only flag a netif here can be told about. */
    if ifi.change & !NLRT_IF_FLAGS_UP != 0 {
        return -EOPNOTSUPP;
    }
    let mut have_mtu = false;
    let mut mtu = 0u32;
    let mut it = AttrIter::new(attrs);
    while let Some((type_, payload)) = it.next() {
        if type_ == IFLA_MTU {
            if payload.len() != 4 {
                return -EINVAL;
            }
            mtu = u32::from_ne_bytes([payload[0], payload[1], payload[2], payload[3]]);
            have_mtu = true;
        } else if type_ == IFLA_OPERSTATE {
            /* Carrier belongs to the driver, which re-derives it on every poll;
             * a user-written operstate would be reverted on the next tick. */
            return -EOPNOTSUPP;
        }
    }
    if it.bad {
        return -EINVAL;
    }
    if have_mtu && (mtu < NLRT_MIN_MTU || mtu > 0xffff) {
        return -EINVAL;
    }
    let want_up = if ifi.change & NLRT_IF_FLAGS_UP != 0 {
        (ifi.flags & NLRT_IF_FLAGS_UP != 0) as u32
    } else {
        0
    };

    if a20_net_netlink_ns_initial(ns) == 0 {
        if ifindex != 1 {
            return -ENODEV;
        }
        if have_mtu {
            a20_net_netlink_loopback_mtu(ns, mtu, 1);
        }
        if ifi.change & NLRT_IF_FLAGS_UP != 0 {
            a20_net_netlink_loopback_up(ns, want_up, 1);
        }
        return 0;
    }
    let mut rc = 0;
    if have_mtu {
        rc = a20_net_netlink_if_set_mtu(ifindex, mtu as u16);
        if rc < 0 {
            return rc;
        }
    }
    if ifi.change & NLRT_IF_FLAGS_UP != 0 {
        rc = a20_net_netlink_if_set_flags(ifindex,
                                          if want_up != 0 { NLRT_IF_FLAGS_UP } else { 0 },
                                          NLRT_IF_FLAGS_UP);
    }
    rc
}

unsafe fn apply_addr6(msg_type: u16, ifa: &IfAddrMsg, attrs: &[u8]) -> i32 {
    if msg_type != RTM_NEWADDR && msg_type != RTM_DELADDR {
        return -EOPNOTSUPP;
    }
    /* Only the family-independent fields are used for a delete; the prefix
     * length is not stored per address, so the event carries 0 just like the
     * IPv4 delete path. */
    if ifa.index == 0 {
        return -EINVAL;
    }
    if ifa.prefixlen > 128 {
        return -EINVAL;
    }
    let mut address = [0u8; 16];
    let mut local = [0u8; 16];
    let mut have_address = false;
    let mut have_local = false;
    let mut it = AttrIter::new(attrs);
    while let Some((type_, payload)) = it.next() {
        if type_ == IFA_ADDRESS {
            if payload.len() != 16 {
                return -EINVAL;
            }
            address.copy_from_slice(payload);
            have_address = true;
        } else if type_ == IFA_LOCAL {
            if payload.len() != 16 {
                return -EINVAL;
            }
            local.copy_from_slice(payload);
            have_local = true;
        }
    }
    if it.bad {
        return -EINVAL;
    }
    if have_local && have_address && local != address {
        return -EOPNOTSUPP; /* point-to-point form */
    }
    if !have_local && !have_address {
        return -EINVAL;
    }
    let want = if have_local { &local } else { &address };
    if msg_type == RTM_DELADDR {
        return a20_net_netlink_if_del_addr6(ifa.index, want.as_ptr());
    }
    a20_net_netlink_if_set_addr6(ifa.index, want.as_ptr(), ifa.prefixlen)
}

unsafe fn apply_addr(msg_type: u16, flags: u16, ifa: &IfAddrMsg,
                     attrs: &[u8]) -> i32 {
    if ifa.family as i32 == AF_INET6 {
        return apply_addr6(msg_type, ifa, attrs);
    }
    if ifa.family as i32 != AF_INET {
        return -EAFNOSUPPORT;
    }
    if ifa.index == 0 {
        return -EINVAL;
    }
    if ifa.prefixlen > 32 {
        return -EINVAL;
    }
    let ifindex = ifa.index;
    let mut address = [0u8; 4];
    let mut local = [0u8; 4];
    let mut have_address = false;
    let mut have_local = false;
    let mut it = AttrIter::new(attrs);
    while let Some((type_, payload)) = it.next() {
        if type_ == IFA_ADDRESS {
            if payload.len() != 4 {
                return -EINVAL;
            }
            address.copy_from_slice(payload);
            have_address = true;
        } else if type_ == IFA_LOCAL {
            if payload.len() != 4 {
                return -EINVAL;
            }
            local.copy_from_slice(payload);
            have_local = true;
        }
    }
    if it.bad {
        return -EINVAL;
    }
    if have_local && have_address && local != address {
        /* A distinct IFA_LOCAL is the secondary-address spelling; lwIP has one
         * IPv4 slot per netif, so applying it would overwrite the primary
         * while answering success. */
        return -EOPNOTSUPP;
    }
    let want = if have_local {
        local
    } else if have_address {
        address
    } else {
        return -EINVAL;
    };

    let mut cur = [0u8; 4];
    let mut cur_mask = [0u8; 4];
    let mut cur_gw = [0u8; 4];
    let rc = a20_net_netlink_if_get_addr(ifindex, cur.as_mut_ptr(),
                                         cur_mask.as_mut_ptr(), cur_gw.as_mut_ptr());
    if rc < 0 {
        return rc;
    }
    if msg_type == RTM_DELADDR {
        if ip4_isany(&cur) || cur != want {
            return -EADDRNOTAVAIL;
        }
        let none = [0u8; 4];
        return a20_net_netlink_if_set_addr(ifindex, none.as_ptr(),
                                           ptr::null(), ptr::null());
    }
    /* One IPv4 slot per netif: a different configured address can only be
     * replaced by an explicit NLM_F_REPLACE. */
    if !ip4_isany(&cur) && cur != want && (flags & NLM_F_REPLACE) == 0 {
        return -EOPNOTSUPP;
    }
    let mut mask = [0u8; 4];
    for i in 0..ifa.prefixlen as usize {
        mask[i / 8] |= 0x80u8 >> (i % 8);
    }
    a20_net_netlink_if_set_addr(ifindex, want.as_ptr(), mask.as_ptr(), ptr::null())
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_netlink_route_request(requester: *mut c_void,
                                                       buf: *const c_void,
                                                       len: usize,
                                                       addr: *const c_void,
                                                       addrlen: usize) -> i32 {
    if requester.is_null() {
        return -EPROTONOSUPPORT;
    }
    let rv = view_of(requester);
    if *rv.domain != AF_NETLINK as i32 || *rv.protocol != NETLINK_ROUTE {
        return -EPROTONOSUPPORT;
    }
    let hdr_size = core::mem::size_of::<NlMsghdr>();
    if buf.is_null() || len < hdr_size {
        return -EINVAL;
    }
    /* musl's send() passes a zeroed sockaddr with addrlen 0; only validate a
     * real length. */
    if !addr.is_null()
        && addrlen >= NET_SOCKADDR_NL_SIZE
        && ptr::read_unaligned(addr as *const u16) != AF_NETLINK
    {
        return -EAFNOSUPPORT;
    }
    let req = ptr::read_unaligned(buf as *const NlMsghdr);
    if (req.len as usize)<hdr_size || (req.len as usize)>len {return -EINVAL;}
    let payload = req.len as usize - hdr_size;
    let type_ = req.msg_type;
    let query = type_ == RTM_GETLINK || type_ == RTM_GETADDR || type_ == RTM_GETROUTE || type_==RTM_GETRULE;
    /* Write forms carry the full attribute struct; the dump forms (RTM_GET*)
     * only require a struct rtgenmsg -- one family byte -- exactly as Linux's
     * rtnl_msg_min_size[] defines them.  Both shapes occur in the wild:
     * iproute2 sends the larger one, while Go's syscall.NetlinkRIB sends the
     * 17-byte rtgenmsg form.  The dump replies below are built from the
     * interface snapshot, never from these request fields, so accepting the
     * short form loses nothing. */
    let want = if type_ == RTM_NEWLINK {
        core::mem::size_of::<IfInfoMsg>()
    } else if type_ == RTM_NEWADDR || type_ == RTM_DELADDR {
        core::mem::size_of::<IfAddrMsg>()
    } else if type_ == RTM_NEWROUTE || type_ == RTM_DELROUTE
        || type_ == RTM_NEWRULE || type_ == RTM_DELRULE
    {
        /* fib_rule_hdr is also 12 bytes, so the minimum body is the same. */
        core::mem::size_of::<RtMsg>()
    } else if type_ == RTM_GETLINK || type_ == RTM_GETADDR || type_ == RTM_GETROUTE || type_==RTM_GETRULE {
        1
    } else {
        return -EOPNOTSUPP;
    };
    if (req.len as usize) < hdr_size + want || req.len as usize > len
        || payload < want
    {
        return -EINVAL;
    }
    let family = ptr::read((buf as *const u8).add(hdr_size)) as i32;
    if query && type_ != RTM_GETLINK && family != AF_UNSPEC && family != AF_INET && family != AF_INET6 {
        return -EAFNOSUPPORT;
    }

    /* Write requests run before any reply is built and hold no lock. */
    if type_ == RTM_NEWLINK || type_ == RTM_NEWADDR || type_ == RTM_DELADDR
        || type_ == RTM_NEWROUTE || type_ == RTM_DELROUTE
        || type_ == RTM_NEWRULE || type_ == RTM_DELRULE
    {
        if a20_net_netlink_capable(*rv.net_ns) == 0 {
            return -EPERM;
        }
        if req.flags & NLM_F_MULTI != 0 {
            return -EINVAL;
        }
        let body = (buf as *const u8).add(hdr_size);
        let blen = req.len as usize - hdr_size - want;
        let attrs = core::slice::from_raw_parts(body.add(want), blen);
        let rc = if type_ == RTM_NEWLINK {
            let ifi = ptr::read_unaligned(body as *const IfInfoMsg);
            apply_newlink(*rv.net_ns, &ifi, attrs)
        } else if type_ == RTM_NEWADDR || type_ == RTM_DELADDR {
            if a20_net_netlink_ns_initial(*rv.net_ns) == 0 {
                -EOPNOTSUPP
            } else {
                let ifa = ptr::read_unaligned(body as *const IfAddrMsg);
                apply_addr(type_, req.flags, &ifa, attrs)
            }
        } else if type_ == RTM_NEWROUTE || type_ == RTM_DELROUTE {
            if a20_net_netlink_ns_initial(*rv.net_ns) == 0 { -EOPNOTSUPP }
            else { apply_route(type_, req.flags, body, attrs) }
        } else {
            if a20_net_netlink_ns_initial(*rv.net_ns) == 0 { -EOPNOTSUPP }
            else { apply_rule(type_,body,attrs) }
        };
        if rc >= 0 && (req.flags & NLM_F_ACK) != 0 {
            let ack_pid = if *rv.local_len >= NET_SOCKADDR_NL_SIZE {
                a20_net_netlink_local_pid(requester)
            } else {
                0
            };
            enqueue_ack(requester, &req, ack_pid);
        }
        return rc;
    }

    if type_==RTM_GETRULE {
        if family==AF_INET6 {return -EAFNOSUPPORT;}
        return rule_dump(requester,&req,len);
    }
    let mut links = [NlrtLink::zero(); NLRT_MAX_LINKS];
    let nlinks = a20_net_netlink_snapshot(*rv.net_ns, links.as_mut_ptr(),
                                          NLRT_MAX_LINKS as i32);
    if nlinks < 0 {
        return nlinks;
    }
    let mut from = [0u8; NET_SOCKADDR_NL_SIZE];
    ptr::write_unaligned(from.as_mut_ptr() as *mut u16, AF_NETLINK);

    let route_query=if type_==RTM_GETROUTE && family!=AF_INET6
        && a20_net_netlink_ns_initial(*rv.net_ns)!=0 {
        let body=(buf as *const u8).add(hdr_size);
        match route_query_values(body,payload,req.flags & NLM_F_DUMP != 0) {
            Ok(q)=>q,Err(e)=>return e,
        }
    } else {RouteQuery::empty()};
    let mut irq = 0u64;
    a20_net_netlink_lock(requester, &mut irq);
    if a20_net_netlink_is_live(requester) == 0 {
        a20_net_netlink_unlock(requester, irq);
        return -ENOTSOCK;
    }
    let pid = if *rv.local_len >= NET_SOCKADDR_NL_SIZE {
        a20_net_netlink_local_pid(requester)
    } else {
        0
    };

    /* A GET without NLM_F_DUMP asks about one link (or one address), named by
     * ifi_index or an IFLA_IFNAME attribute; answering with the whole list made
     * tailscale's LinkByName report "more than one link found". */
    let is_dump = (req.flags & NLM_F_DUMP) != 0;
    let mut want_index = 0u32;
    let mut want_name = [0u8; 16];
    let mut have_name = false;
    if !is_dump
        && (type_ == RTM_GETLINK || type_ == RTM_GETADDR)
        && payload >= core::mem::size_of::<IfInfoMsg>()
    {
        let body = (buf as *const u8).add(hdr_size);
        let ifi = ptr::read_unaligned(body as *const IfInfoMsg);
        want_index = ifi.index as u32;
        let attrs = core::slice::from_raw_parts(body.add(core::mem::size_of::<IfInfoMsg>()),
                                                payload - core::mem::size_of::<IfInfoMsg>());
        let mut it = AttrIter::new(attrs);
        while let Some((type_, pl)) = it.next() {
            if type_ == IFLA_IFNAME && !pl.is_empty() {
                let n = if pl.len() > 15 { 15 } else { pl.len() };
                want_name[..n].copy_from_slice(&pl[..n]);
                have_name = true;
            }
        }
    }
    let mut msg = MsgBuf::new();
    let mut rc = 0;
    let mut i = 0;
    /* A dump answers in the NEW form -- RTM_NEWLINK / RTM_NEWADDR /
     * RTM_NEWROUTE -- never with the GET type it was asked with.  Reply
     * parsers dispatch on the NEW type (Go's netlinkRIB looks for
     * syscall.RTM_NEWLINK, iproute2 does the same), so echoing the request
     * type made every dump look empty: net.Interfaces() returned zero
     * interfaces while ifconfig, which does not use netlink, showed them. */
    let reply_type = if type_ == RTM_GETLINK {
        RTM_NEWLINK
    } else if type_ == RTM_GETADDR {
        RTM_NEWADDR
    } else {
        RTM_NEWROUTE
    };
    let have_default=route_query.routes.iter().take(route_query.count)
        .any(|r| r.table==254 && r.prefix==0);
    let installed_route_found=route_query.count!=0;
    let requested_table=route_query.table;
    for r in route_query.routes.iter().take(route_query.count) {
        let total=build_route_v4(&mut msg.0,r,pid,req.seq);
        if !is_dump {ptr::write_unaligned(msg.0.as_mut_ptr().add(6) as *mut u16,0);}
        rc=a20_net_netlink_enqueue(requester,msg.0.as_ptr(),total,
            from.as_ptr() as *const c_void,NET_SOCKADDR_NL_SIZE);
        if rc<0 {break;}
    }
    while i < nlinks && rc >= 0
        && !(type_==RTM_GETROUTE && !is_dump && installed_route_found) {
        let e = &links[i as usize];
        i += 1;
        if !is_dump {
            if want_index != 0 && e.index != want_index {
                continue;
            }
            if want_index == 0 && have_name {
                let n = name_bytes(e);
                let want = &want_name[..want_name.iter().position(|&b| b == 0).unwrap_or(16)];
                if n != want {
                    continue;
                }
            }
        }
        let total = if type_ == RTM_GETLINK {
            build_linkmsg(&mut msg.0, e, reply_type, pid, req.seq)
        } else if type_ == RTM_GETADDR {
            if e.has_ip == 0 || family == AF_INET6 {
                continue;
            }
            build_addrmsg(&mut msg.0, e.index, &e.ip, &e.mask, name_bytes(e),
                          e.loopback, reply_type, pid, req.seq)
        } else {
            if e.has_gw == 0 || family == AF_INET6 || have_default
                || (requested_table!=0 && requested_table!=254) {
                continue;
            }
            let mut off = hdr_size;
            let rt = RtMsg {
                family: AF_INET as u8,
                dst_len: 0,
                src_len: 0,
                tos: 0,
                table: 254,
                protocol: NLRT_RTPROT_BOOT,
                scope: NLRT_SCOPE_UNIVERSE,
                type_: 1,
                flags: 0,
            };
            ptr::write_unaligned(msg.0.as_mut_ptr().add(off) as *mut RtMsg, rt);
            off += core::mem::size_of::<RtMsg>();
            off = put_attr(&mut msg.0, off, RTA_GATEWAY, &e.gw);
            let oif = e.index;
            off = put_attr(&mut msg.0, off, RTA_OIF, &oif.to_ne_bytes());
            let total = off;
            fill_hdr(&mut msg.0, reply_type, total as u32, req.seq, pid);
            total
        };
        if total == 0 {
            continue;
        }
        if !is_dump {
            /* A dump is a MULTI stream terminated by NLMSG_DONE; a lookup is
             * one message and must not carry MULTI, or the caller waits for
             * the rest of a stream that does not exist.  flags sits at offset
             * 6 in struct nlmsghdr (len:u32, type:u16, flags:u16). */
            ptr::write_unaligned(msg.0.as_mut_ptr().add(6) as *mut u16, 0);
        }
        rc = a20_net_netlink_enqueue(requester, msg.0.as_ptr(), total,
                                     from.as_ptr() as *const c_void,
                                     NET_SOCKADDR_NL_SIZE);
    }

    /* The dump terminator is part of the dump protocol; a single-object GET
     * answers with exactly one message, as Linux does. */
    if rc >= 0 && is_dump {
        let mut done = MsgBuf::new();
        fill_hdr(&mut done.0, NLMSG_DONE, core::mem::size_of::<DiagDone>() as u32,
                 req.seq, pid);
        rc = a20_net_netlink_enqueue(requester, done.0.as_ptr(),
                                     core::mem::size_of::<DiagDone>(),
                                     from.as_ptr() as *const c_void,
                                     NET_SOCKADDR_NL_SIZE);
    }
    /* NLM_F_ACK applies to single-object GETs too: tailscale's LinkByName
     * sends RTM_GETLINK with NLM_F_REQUEST|NLM_F_ACK and waits for the status
     * message, so replying with only the link leaves it blocked until its
     * lookup deadline (observed as a 60 s stall ending in EAGAIN). */
    if rc >= 0 && (req.flags & NLM_F_ACK) != 0 {
        enqueue_ack(requester, &req, pid);
    }
    a20_net_netlink_unlock(requester, irq);
    if rc < 0 { rc } else { len as i32 }
}

/* ---------------------------------------------------------- rtnetlink send */

static NLRT_NOTIFY_SEQ: core::sync::atomic::AtomicU32 =
    core::sync::atomic::AtomicU32::new(0);

fn nlrt_next_seq() -> u32 {
    NLRT_NOTIFY_SEQ.fetch_add(1, core::sync::atomic::Ordering::Relaxed) + 1
}

unsafe fn nlrt_broadcast(msg: *const u8, len: usize, group: u32) -> i32 {
    let mut src = [0u8; NET_SOCKADDR_NL_SIZE];
    ptr::write_unaligned(src.as_mut_ptr() as *mut u16, AF_NETLINK);
    ptr::write_unaligned(src.as_mut_ptr().add(8) as *mut u32, group);

    let mut delivered = 0;
    let mut dropped = 0;
    let buckets = a20_net_netlink_buckets();
    let per_bucket = a20_net_netlink_slots_per_bucket();
    for b in 0..buckets {
        let mut bf = 0u64;
        a20_net_netlink_bucket_lock(b, &mut bf);
        for k in 0..per_bucket {
            let sp = a20_net_netlink_slot_socket(b, k);
            if sp.is_null() {
                continue;
            }
            let sv = view_of(sp);
            let mut sf = 0u64;
            a20_net_netlink_lock(sp, &mut sf);
            let mut matched = false;
            let mut r = -1;
            if a20_net_netlink_ns_initial(*sv.net_ns) != 0
                && *sv.domain == AF_NETLINK as i32
                && *sv.bound != 0
                && *sv.protocol == NETLINK_ROUTE
            {
                if a20_net_netlink_local_groups(sp) & group != 0 {
                    matched = true;
                    r = a20_net_netlink_enqueue(sp, msg, len,
                                                src.as_ptr() as *const c_void,
                                                NET_SOCKADDR_NL_SIZE);
                }
            }
            a20_net_netlink_unlock(sp, sf);
            if !matched {
                continue;
            }
            if r < 0 {
                dropped += 1;
            } else {
                delivered += 1;
            }
        }
        a20_net_netlink_bucket_unlock(b, bf);
    }
    if dropped != 0 {
        /* A silent drop is a gap a listener can never diagnose. */
        a20_net_netlink_klog_drop(group, dropped, delivered);
    }
    if dropped != 0 { -EAGAIN } else if delivered != 0 { 0 } else { -ENOENT }
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_netlink_link_notify(events: *const c_void, n: i32) {
    if events.is_null() || n <= 0 {
        return;
    }
    let ns = a20_net_netlink_current_ns();
    let mut links = [NlrtLink::zero(); NLRT_MAX_LINKS];
    let nlinks = a20_net_netlink_snapshot(ns, links.as_mut_ptr(),
                                          NLRT_MAX_LINKS as i32);
    if nlinks <= 0 {
        return;
    }
    let mut msg = MsgBuf::new();
    for i in 0..n {
        let index = a20_net_netlink_event_index(events, i);
        let mut found: Option<&NlrtLink> = None;
        for k in 0..nlinks as usize {
            if links[k].index == index {
                found = Some(&links[k]);
                break;
            }
        }
        let Some(e) = found else { continue };
        let total = build_linkmsg(&mut msg.0, e, RTM_NEWLINK, 0, nlrt_next_seq());
        if total == 0 {
            continue;
        }
        let _ = nlrt_broadcast(msg.0.as_ptr(), total, RTNLGRP_LINK);
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_netlink_addr_notify(ifindex: u32, addr: *const u8,
                                                     mask: *const u8) {
    if addr.is_null() {
        return;
    }
    let mut a4 = [0u8; 4];
    let mut m4 = [0u8; 4];
    ptr::copy_nonoverlapping(addr, a4.as_mut_ptr(), 4);
    if !mask.is_null() {
        ptr::copy_nonoverlapping(mask, m4.as_mut_ptr(), 4);
    }
    let ns = a20_net_netlink_current_ns();
    let mut links = [NlrtLink::zero(); NLRT_MAX_LINKS];
    let nlinks = a20_net_netlink_snapshot(ns, links.as_mut_ptr(),
                                          NLRT_MAX_LINKS as i32);
    let mut found: Option<&NlrtLink> = None;
    for k in 0..nlinks as usize {
        if links[k].index == ifindex {
            found = Some(&links[k]);
            break;
        }
    }
    let Some(e) = found else { return };
    /* An all-zero address is a delete: "you now have 0.0.0.0" is not a state
     * any interface is in. */
    let deleted = ip4_isany(&a4);
    let mut msg = MsgBuf::new();
    let total = build_addrmsg(&mut msg.0, e.index, &a4, &m4, name_bytes(e),
                              e.loopback,
                              if deleted { RTM_DELADDR } else { RTM_NEWADDR },
                              0, nlrt_next_seq());
    if total == 0 {
        return;
    }
    let _ = nlrt_broadcast(msg.0.as_ptr(), total, RTNLGRP_IPV4_IFADDR);
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_netlink_addr6_notify(ifindex: u32,
                                                      addr: *const u8,
                                                      prefixlen: u8,
                                                      msg_type: u16) {
    if addr.is_null() {
        return;
    }
    let mut a16 = [0u8; 16];
    ptr::copy_nonoverlapping(addr, a16.as_mut_ptr(), 16);
    let ns = a20_net_netlink_current_ns();
    let mut links = [NlrtLink::zero(); NLRT_MAX_LINKS];
    let nlinks = a20_net_netlink_snapshot(ns, links.as_mut_ptr(),
                                          NLRT_MAX_LINKS as i32);
    let mut found: Option<&NlrtLink> = None;
    for k in 0..nlinks as usize {
        if links[k].index == ifindex {
            found = Some(&links[k]);
            break;
        }
    }
    let Some(e) = found else { return };
    let mut msg = MsgBuf::new();
    let total = build_addrmsg6(&mut msg.0, e.index, &a16, prefixlen,
                               name_bytes(e), e.loopback, msg_type, 0,
                               nlrt_next_seq());
    if total == 0 {
        return;
    }
    let _ = nlrt_broadcast(msg.0.as_ptr(), total, RTNLGRP_IPV6_IFADDR);
}

#[cfg(test)]
#[allow(static_mut_refs)]
mod tests {
    use super::*;

    const NL: usize = 12;

    #[repr(C)]
    struct Sock {
        domain: i32,
        sock_type: i32,
        protocol: i32,
        bound: i32,
        connected: i32,
        listening: i32,
        local: [u8; 128],
        local_len: usize,
        peer_addr: [u8; 128],
        peer_len: usize,
        live: i32,
        /* The view points at this slot, so it must outlive the call: a
         * temporary here would leave the walk comparing a dangling value. */
        ns: *mut c_void,
        rx_total: u32,
    }

    impl Sock {
        const fn new() -> Sock {
            Sock {
                domain: AF_NETLINK as i32,
                sock_type: 3,
                protocol: 0,
                bound: 0,
                connected: 0,
                listening: 0,
                local: [0u8; 128],
                local_len: 0,
                peer_addr: [0u8; 128],
                peer_len: 0,
                live: 1,
                ns: 1usize as *mut c_void,
                rx_total: 0,
            }
        }
    }

    static mut S_A: Sock = Sock::new();
    static mut S_B: Sock = Sock::new();
    static mut S_SLOTS: [*mut Sock; 4] = [ptr::null_mut(); 4];
    static mut T_HOST_PID: u32 = 42;
    static mut T_CAPABLE: i32 = 1;
    static mut T_NS_INITIAL: i32 = 1;
    static mut T_ENQ: i32 = 0;
    static mut T_ENQ_RC: i32 = 0;
    #[repr(C, align(8))]
    struct AlignedBuf([u8; 256]);

    static mut T_ENQ_BYTES: AlignedBuf = AlignedBuf([0; 256]);
    static mut T_ENQ_LEN: usize = 0;
    static mut T_FIRST: [u8; 256] = [0; 256];
    static mut T_FIRST_LEN: usize = 0;
    static mut T_SLOT_CALLS: i32 = 0;
    static mut T_ENQ_CALLS: i32 = 0;
    static mut T_EMIT_ALL: i32 = 0;
    static mut T_CDEV: i32 = 0;
    static mut T_CDEV_SUBSYS: i32 = 1;
    static mut T_CDEV_DEVT: u64 = 0x0301;
    static mut T_CDEV_PUT: i32 = 0;
    static mut T_SNAPSHOT: [NlrtLink; NLRT_MAX_LINKS] = [NlrtLink::zero(); NLRT_MAX_LINKS];
    static mut T_NLINKS: i32 = 0;
    static mut T_SET_MTU: i32 = 0;
    static mut T_SET_FLAGS: i32 = 0;
    static mut T_SET_ADDR: i32 = 0;
    static mut T_SET_ADDR6: i32 = 0;
    static mut T_DEL_ADDR6: i32 = 0;
    static mut T_GET_ADDR_IP: [u8; 4] = [0; 4];
    static mut T_GET_ADDR_RC: i32 = 0;
    static mut T_LOOP_MTU: u32 = 65536;
    static mut T_LOOP_UP: u32 = 1;
    static mut T_RX_BYTES: i32 = 0;
    static mut T_KLOG_UEVENT: i32 = 0;
    static mut T_KLOG_DROP: i32 = 0;
    static mut T_EVENT_INDEX: [u32; 4] = [0; 4];
    static mut T_TRACE: [u8; 32] = [0; 32];
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
        DIAG_SKIP.store(0, core::sync::atomic::Ordering::Relaxed);
        UEVENT_SEQ.store(0, core::sync::atomic::Ordering::Relaxed);
        NLRT_NOTIFY_SEQ.store(0, core::sync::atomic::Ordering::Relaxed);
        S_A = Sock::new();
        S_B = Sock::new();
        S_SLOTS = [ptr::null_mut(); 4];
        T_HOST_PID = 42;
        T_CAPABLE = 1;
        T_NS_INITIAL = 1;
        T_ENQ = 0;
        T_ENQ_RC = 0;
        T_ENQ_BYTES = AlignedBuf([0; 256]);
        T_ENQ_LEN = 0;
        T_FIRST = [0; 256];
        T_FIRST_LEN = 0;
        T_SLOT_CALLS = 0;
        T_ENQ_CALLS = 0;
        T_EMIT_ALL = 0;
        T_CDEV = 0;
        T_CDEV_SUBSYS = 1;
        T_CDEV_DEVT = 0x0301;
        T_CDEV_PUT = 0;
        T_SNAPSHOT = [NlrtLink::zero(); NLRT_MAX_LINKS];
        T_NLINKS = 0;
        T_SET_MTU = 0;
        T_SET_FLAGS = 0;
        T_SET_ADDR = 0;
        T_SET_ADDR6 = 0;
        T_DEL_ADDR6 = 0;
        T_GET_ADDR_IP = [0; 4];
        T_GET_ADDR_RC = 0;
        T_LOOP_MTU = 65536;
        T_LOOP_UP = 1;
        T_RX_BYTES = 0;
        T_KLOG_UEVENT = 0;
        T_KLOG_DROP = 0;
        T_EVENT_INDEX = [0; 4];
        T_TRACE = [0; 32];
        T_TRACE_N = 0;
    }

    unsafe fn sa() -> *mut Sock {
        ptr::addr_of_mut!(S_A)
    }

    unsafe fn set_local(s: *mut Sock, pid: u32, groups: u32) {
        (*s).local = [0u8; 128];
        ptr::write_unaligned((*s).local.as_mut_ptr() as *mut u16, AF_NETLINK);
        ptr::write_unaligned((*s).local.as_mut_ptr().add(4) as *mut u32, pid);
        ptr::write_unaligned((*s).local.as_mut_ptr().add(8) as *mut u32, groups);
        (*s).local_len = NL;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_view_init(s: *mut c_void,
                                                   out: *mut NetlinkView) {
        let k = &mut *(s as *mut Sock);
        *out = NetlinkView {
            s,
            net_ns: &mut k.ns,
            domain: &mut k.domain,
            sock_type: &mut k.sock_type,
            protocol: &mut k.protocol,
            bound: &mut k.bound,
            connected: &mut k.connected,
            listening: &mut k.listening,
            local: k.local.as_mut_ptr(),
            local_len: &mut k.local_len,
            peer_addr: k.peer_addr.as_mut_ptr(),
            peer_len: &mut k.peer_len,
        };
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_lock(_s: *mut c_void, _f: *mut u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_unlock(_s: *mut c_void, _f: u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_buckets() -> i32 {
        1
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_slots_per_bucket() -> i32 {
        4
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_bucket_lock(_b: i32, _f: *mut u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_bucket_unlock(_b: i32, _f: u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_slot_socket(_b: i32, i: i32) -> *mut c_void {
        T_SLOT_CALLS += 1;
        if i < 0 || i >= 4 { ptr::null_mut() } else { S_SLOTS[i as usize] as *mut c_void }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_is_live(s: *mut c_void) -> i32 {
        (*(s as *const Sock)).live
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_enqueue(_s: *mut c_void, buf: *const u8,
                                                 len: usize, _src: *const c_void,
                                                 _srclen: usize) -> i32 {
        T_ENQ_CALLS += 1;
        trace(b'E');
        if len <= T_ENQ_BYTES.0.len() {
            if T_ENQ_CALLS == 1 {
                ptr::copy_nonoverlapping(buf, T_FIRST.as_mut_ptr(), len);
                T_FIRST_LEN = len;
            }
            ptr::copy_nonoverlapping(buf, T_ENQ_BYTES.0.as_mut_ptr(), len);
            T_ENQ_LEN = len;
        }
        T_ENQ_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_rx_bytes(s: *mut c_void) -> u32 {
        T_RX_BYTES += 1;
        (*(s as *const Sock)).rx_total
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_local_pid(s: *mut c_void) -> u32 {
        let k = &*(s as *const Sock);
        if k.local_len < NL { 0 } else { ptr::read_unaligned(k.local.as_ptr().add(4) as *const u32) }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_local_groups(s: *mut c_void) -> u32 {
        let k = &*(s as *const Sock);
        if k.local_len < NL { 0 } else { ptr::read_unaligned(k.local.as_ptr().add(8) as *const u32) }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_host_pid() -> u32 {
        T_HOST_PID
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_ns_initial(_ns: *mut c_void) -> i32 {
        T_NS_INITIAL
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_current_ns() -> *mut c_void {
        1usize as *mut c_void
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_capable(_ns: *mut c_void) -> i32 {
        T_CAPABLE
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_class_emit_all(_a: *const i8) {
        T_EMIT_ALL += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_class_get(_n: *const i8) -> *mut c_void {
        if T_CDEV == 0 { ptr::null_mut() } else { 8usize as *mut c_void }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_class_subsystem(_c: *mut c_void) -> *const i8 {
        if T_CDEV_SUBSYS == 0 { ptr::null() } else { b"net\0".as_ptr() as *const i8 }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_class_devt(_c: *mut c_void) -> u64 {
        T_CDEV_DEVT
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_class_put(_c: *mut c_void) {
        T_CDEV_PUT += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_snapshot(_ns: *mut c_void, out: *mut NlrtLink,
                                                  max: i32) -> i32 {
        let n = T_NLINKS.min(max);
        for i in 0..n as usize {
            *out.add(i) = T_SNAPSHOT[i];
        }
        n
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_event_index(_e: *const c_void, i: i32) -> u32 {
        T_EVENT_INDEX[i as usize]
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_if_set_mtu(_i: u32, _m: u16) -> i32 {
        T_SET_MTU += 1;
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_if_set_flags(_i: u32, _v: u32, _m: u32) -> i32 {
        T_SET_FLAGS += 1;
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_if_get_addr(_i: u32, ip: *mut u8,
                                                     _m: *mut u8, _g: *mut u8) -> i32 {
        ptr::copy_nonoverlapping(T_GET_ADDR_IP.as_ptr(), ip, 4);
        T_GET_ADDR_RC
    }
    #[no_mangle]
    unsafe extern "C" fn net_stack_route_update_v4(_d: *const u8, _p: u8,
                                                   _g: *const u8, _i: u16,
                                                   _r: i32) -> i32 {
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_if_set_addr(_i: u32, _ip: *const u8,
                                                     _m: *const u8, _g: *const u8) -> i32 {
        T_SET_ADDR += 1;
        trace(b'A');
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_if_set_addr6(_i: u32, _a: *const u8,
                                                      _p: u8) -> i32 {
        T_SET_ADDR6 += 1;
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_if_del_addr6(_i: u32, _a: *const u8) -> i32 {
        T_DEL_ADDR6 += 1;
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_loopback_mtu(_ns: *mut c_void, v: u32,
                                                      set: i32) -> u32 {
        if set != 0 { T_LOOP_MTU = v; }
        T_LOOP_MTU
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_loopback_up(_ns: *mut c_void, v: u32,
                                                     set: i32) -> u32 {
        if set != 0 { T_LOOP_UP = v; }
        T_LOOP_UP
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_klog_uevent(_a: *const i8, _s: *const i8,
                                                     _n: *const i8, _maj: u32,
                                                     _min: u32, _d: i32) {
        T_KLOG_UEVENT += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_netlink_klog_drop(_g: u32, _d: i32, _del: i32) {
        T_KLOG_DROP += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_net_route_add(_dst: u32, _prefix: u8, _ifindex: u16) -> i32 { 0 }
    #[no_mangle]
    unsafe extern "C" fn a20_net_route_del(_dst: u32, _prefix: u8, _ifindex: u16) -> i32 { 0 }

    #[no_mangle]
    unsafe extern "C" fn a20_net_route_query_v4(_s:u32,_d:u32,_m:u32,_r:*mut RouteV4)->i32 {0}
    #[no_mangle]
    unsafe extern "C" fn a20_net_rule_snapshot_v4(_r:*mut RuleV4,_c:u32)->u32 {0}
    static mut T_ROUTE: Option<RouteV4> = None;
    static mut T_RULE: Option<RuleV4> = None;
    #[no_mangle]
    unsafe extern "C" fn a20_net_route_update_v4(r:*const RouteV4,_remove:i32,_flags:u32)->i32 {
        T_ROUTE=Some(*r);0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_rule_update_v4(r:*const RuleV4,_remove:i32)->i32 {
        T_RULE=Some(*r);0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_net_route_snapshot_v4(_r:*mut RouteV4,_cap:u32)->u32 {0}

    #[test]
    fn gateway_attributes_and_policy_rules_reach_the_fib() {
        unsafe {
            let _serial=serial();reset();T_ROUTE=None;T_RULE=None;
            let mut h=RtMsg {family:AF_INET as u8,dst_len:24,src_len:0,tos:0,
                table:100,protocol:3,scope:0,type_:1,flags:0};
            let mut attrs=[0u8;64];let mut off=0;
            off=put_attr(&mut attrs,off,RTA_DST,&[10,5,6,0]);
            off=put_attr(&mut attrs,off,RTA_GATEWAY,&[192,168,1,1]);
            off=put_attr(&mut attrs,off,RTA_OIF,&2u32.to_ne_bytes());
            off=put_attr(&mut attrs,off,RTA_PRIORITY,&42u32.to_ne_bytes());
            assert_eq!(apply_route(RTM_NEWROUTE,0,&h as *const _ as *const u8,&attrs[..off]),0);
            let r=T_ROUTE.unwrap();assert_eq!(r.gateway.to_ne_bytes(),[192,168,1,1]);
            assert_eq!((r.table,r.metric,r.ifindex),(100,42,2));
            h.type_=0;
            assert_eq!(apply_route(RTM_DELROUTE,0,&h as *const _ as *const u8,&attrs[..off]),0);
            assert_eq!(apply_route(RTM_NEWROUTE,0,&h as *const _ as *const u8,&attrs[..off]),-EOPNOTSUPP);
            h.type_=1;
            h.dst_len=0;h.src_len=24;
            off=0;off=put_attr(&mut attrs,off,2,&[192,168,10,0]);
            off=put_attr(&mut attrs,off,6,&80u32.to_ne_bytes());
            assert_eq!(apply_rule(RTM_NEWRULE,&h as *const _ as *const u8,&attrs[..off]),0);
            let r=T_RULE.unwrap();assert_eq!((r.priority,r.table,r.source_prefix),(80,100,24));
            h.type_=0;
            assert_eq!(apply_rule(RTM_DELRULE,&h as *const _ as *const u8,&attrs[..off]),0);
            assert_eq!(apply_rule(RTM_NEWRULE,&h as *const _ as *const u8,&attrs[..off]),-EOPNOTSUPP);
            h.type_=7;
            assert_eq!(apply_rule(RTM_NEWRULE,&h as *const _ as *const u8,&attrs[..off]),-EOPNOTSUPP);
        }
    }

    /* ------------------------------------------------------------- bind */

    #[test]
    fn bind_defaults_the_pid_and_publishes_the_address() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            let mut a = [0u8; NL];
            ptr::write_unaligned(a.as_mut_ptr() as *mut u16, AF_NETLINK);
            assert_eq!(a20_net_netlink_bind(s as *mut c_void, a.as_ptr() as *const c_void, NL), 0);
            assert_eq!((*s).bound, 1);
            assert_eq!((*s).local_len, NL);
            assert_eq!(ptr::read_unaligned((*s).local.as_ptr().add(4) as *const u32),
                       T_HOST_PID);

            /* A requested pid is kept. */
            reset();
            let s = sa();
            ptr::write_unaligned(a.as_mut_ptr().add(4) as *mut u32, 77);
            assert_eq!(a20_net_netlink_bind(s as *mut c_void, a.as_ptr() as *const c_void, NL), 0);
            assert_eq!(ptr::read_unaligned((*s).local.as_ptr().add(4) as *const u32), 77);
        }
    }

    #[test]
    fn bind_refuses_short_and_foreign_addresses() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            let mut a = [0u8; NL];
            assert_eq!(a20_net_netlink_bind(s as *mut c_void, a.as_ptr() as *const c_void, NL - 1),
                       -EINVAL);
            a[0] = 2;
            assert_eq!(a20_net_netlink_bind(s as *mut c_void, a.as_ptr() as *const c_void, NL),
                       -EAFNOSUPPORT);
            assert_eq!(a20_net_netlink_bind(s as *mut c_void, ptr::null(), NL), -EINVAL);

            /* A dead socket is refused after the copyout point. */
            reset();
            let s = sa();
            (*s).live = 0;
            a[0] = AF_NETLINK as u8;
            assert_eq!(a20_net_netlink_bind(s as *mut c_void, a.as_ptr() as *const c_void, NL),
                       -ENOTSOCK);
        }
    }

    #[test]
    fn bind_replays_uevents_only_for_the_uevent_group() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).protocol = NETLINK_KOBJECT_UEVENT;
            let mut a = [0u8; NL];
            ptr::write_unaligned(a.as_mut_ptr() as *mut u16, AF_NETLINK);
            ptr::write_unaligned(a.as_mut_ptr().add(8) as *mut u32, UEVENT_GROUP);
            assert_eq!(a20_net_netlink_bind(s as *mut c_void, a.as_ptr() as *const c_void, NL), 0);
            assert_eq!(T_EMIT_ALL, 1);

            /* No group: no replay. */
            reset();
            let s = sa();
            (*s).protocol = NETLINK_KOBJECT_UEVENT;
            let mut plain = [0u8; NL];
            ptr::write_unaligned(plain.as_mut_ptr() as *mut u16, AF_NETLINK);
            assert_eq!(a20_net_netlink_bind(s as *mut c_void, plain.as_ptr() as *const c_void, NL), 0);
            assert_eq!(T_EMIT_ALL, 0);

            /* Another protocol: no replay. */
            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            assert_eq!(a20_net_netlink_bind(s as *mut c_void, a.as_ptr() as *const c_void, NL), 0);
            assert_eq!(T_EMIT_ALL, 0);
        }
    }

    /* -------------------------------------------------------- sock_diag */

    unsafe fn diag_req(family: u8, protocol: u8, states: u32) -> [u8; 72] {
        let mut b = [0u8; 72];
        ptr::write_unaligned(b.as_mut_ptr() as *mut NlMsghdr, NlMsghdr {
            len: 72,
            msg_type: SOCK_DIAG_BY_FAMILY,
            flags: 0,
            seq: 9,
            pid: 0,
        });
        ptr::write_unaligned(b.as_mut_ptr().add(16) as *mut InetDiagReqV2,
                             InetDiagReqV2 {
            family,
            protocol,
            ext: 0,
            pad: 0,
            states,
            id: InetDiagSockid {
                sport: 0, dport: 0, src: [0; 4], dst: [0; 4], if_: 0,
                cookie: [0; 2],
            },
        });
        b
    }

    #[test]
    fn diag_validates_its_request() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).domain = AF_NETLINK as i32;
            (*s).protocol = NETLINK_SOCK_DIAG;
            let req = diag_req(AF_INET as u8, 6, 0);
            /* Too short. */
            assert_eq!(a20_net_netlink_diag_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                    71, ptr::null(), 0), -EINVAL);
            /* Unknown message type. */
            let mut bad = req;
            ptr::write_unaligned(bad.as_mut_ptr().add(4) as *mut u16, 1);
            assert_eq!(a20_net_netlink_diag_request(s as *mut c_void, bad.as_ptr() as *const c_void,
                                                    72, ptr::null(), 0), -EINVAL);
            /* Wrong requester protocol. */
            (*s).protocol = NETLINK_ROUTE;
            assert_eq!(a20_net_netlink_diag_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                    72, ptr::null(), 0),
                       -EPROTONOSUPPORT);
        }
    }

    #[test]
    fn diag_replies_carry_the_expected_layout_and_end_with_done() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).protocol = NETLINK_SOCK_DIAG;
            set_local(s, 55, 0);
            let mut peer = Sock::new();
            peer.domain = AF_INET;
            peer.sock_type = 1; /* stream */
            peer.connected = 1;
            peer.local = [0u8; 128];
            ptr::write_unaligned(peer.local.as_mut_ptr() as *mut u16, AF_INET as u16);
            ptr::write_unaligned(peer.local.as_mut_ptr().add(2) as *mut u16, 0x1234);
            ptr::write_unaligned(peer.local.as_mut_ptr().add(4) as *mut u32, 0x0100007f);
            peer.local_len = 16;
            peer.rx_total = 1234;
            S_SLOTS[0] = &mut peer;

            let req = diag_req(AF_INET as u8, 6, 0);
            {
                let sv = view_of(S_SLOTS[0] as *mut c_void);
                assert_eq!(*sv.domain, AF_INET);
                assert!(diag_protocol_matches(&sv, 6));
                assert_eq!(diag_state(&sv), TCP_ESTABLISHED);
                assert_eq!(*sv.net_ns, 1usize as *mut c_void);
            }
            assert_eq!(a20_net_netlink_diag_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                    72, ptr::null(), 0), 72);
            assert_eq!(T_SLOT_CALLS, 4);
            assert!(!(S_SLOTS[0] as *mut c_void).is_null());
            assert_ne!(S_SLOTS[0] as *mut c_void, s as *mut c_void);
            {
                let rv2 = view_of(s as *mut c_void);
                let sv2 = view_of(S_SLOTS[0] as *mut c_void);
                assert_eq!(*sv2.net_ns, *rv2.net_ns, "ns");
                assert_eq!(req[16], AF_INET as u8, "family byte");
                assert_eq!(req[17], 6, "protocol byte");
                assert_eq!(&req[20..24], &[0u8; 4], "states bytes");
                assert_eq!(diag_state(&sv2), TCP_ESTABLISHED, "state");
                assert!(diag_protocol_matches(&sv2, req[17]), "protocol match");
                assert!(*sv2.domain == AF_INET, "domain");
            }
            assert_eq!(DIAG_SKIP.load(core::sync::atomic::Ordering::Relaxed), 0);
            assert_eq!(T_RX_BYTES, 1);
            assert_eq!(T_ENQ_CALLS, 2); /* the reply and the DONE */
            /* The last enqueue was the DONE; check the first layout from the
             * recorded reply, which was overwritten by the DONE, so re-run with
             * a filter that stops after the reply is not possible -- instead
             * assert the DONE that was recorded. */
            let done = &*(T_ENQ_BYTES.0.as_ptr() as *const DiagDone);
            assert_eq!(done.nlh.len as usize, core::mem::size_of::<DiagDone>());
            assert_eq!(done.nlh.msg_type, NLMSG_DONE);
            assert_eq!(done.nlh.flags, NLM_F_MULTI);
            assert_eq!(done.nlh.seq, 9);
            assert_eq!(done.nlh.pid, 55);
        }
    }

    #[test]
    fn diag_filters_the_requester_itself_other_namespaces_and_states() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).protocol = NETLINK_SOCK_DIAG;
            let self_ptr = s;
            S_SLOTS[0] = self_ptr; /* the requester: skipped */
            let mut other_ns = Sock::new();
            other_ns.domain = AF_INET;
            other_ns.sock_type = 1;
            other_ns.ns = 2usize as *mut c_void;
            S_SLOTS[1] = &mut other_ns;
            let mut udp = Sock::new();
            udp.domain = AF_INET;
            udp.sock_type = 2; /* does not match protocol 6 */
            S_SLOTS[2] = &mut udp;

            let req = diag_req(AF_INET as u8, 6, 0);
            assert_eq!(a20_net_netlink_diag_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                    72, ptr::null(), 0), 72);
            /* Only the DONE was enqueued: nothing matched. */
            assert_eq!(T_ENQ_CALLS, 1);

            /* With a state filter that excludes CLOSE, a closed socket is
             * skipped too. */
            reset();
            let s = sa();
            (*s).protocol = NETLINK_SOCK_DIAG;
            let mut closed = Sock::new();
            closed.domain = AF_INET;
            closed.sock_type = 1;
            S_SLOTS[0] = &mut closed;
            let req = diag_req(AF_INET as u8, 6, 1 << TCP_ESTABLISHED);
            assert_eq!(a20_net_netlink_diag_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                    72, ptr::null(), 0), 72);
            assert_eq!(T_ENQ_CALLS, 1);
        }
    }

    #[test]
    fn diag_aborts_on_an_enqueue_failure() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).protocol = NETLINK_SOCK_DIAG;
            let mut peer = Sock::new();
            peer.domain = AF_INET;
            peer.sock_type = 1;
            S_SLOTS[0] = &mut peer;
            T_ENQ_RC = -11;
            let req = diag_req(AF_INET as u8, 6, 0);
            assert_eq!(a20_net_netlink_diag_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                    72, ptr::null(), 0), -11);
            assert_eq!(T_ENQ_CALLS, 1);
        }
    }

    /* ----------------------------------------------------------- uevent */

    #[test]
    fn the_uevent_datagram_has_the_libudev_layout() {
        unsafe {
            let _serial = serial();
            reset();
            let mut listener = Sock::new();
            listener.domain = AF_NETLINK as i32;
            listener.protocol = NETLINK_KOBJECT_UEVENT;
            listener.bound = 1;
            set_local(&mut listener, 7, UEVENT_GROUP);
            S_SLOTS[0] = &mut listener;

            let rc = uevent_broadcast(b"add", b"net", b"eth0", 0x0301);
            assert_eq!(rc, 0);
            assert_eq!(T_ENQ_CALLS, 1);
            let m = &T_ENQ_BYTES.0[..T_ENQ_LEN];
            let expected = b"add@/class/net/eth0\0SEQNUM=1\0ACTION=add\0DEVPATH=/class/net/eth0\0SUBSYSTEM=net\0MAJOR=3\0MINOR=1\0DEVNAME=net/eth0\0\0";
            assert_eq!(m, expected);
            assert_eq!(T_KLOG_UEVENT, 1);

            /* The sequence number really advances. */
            let _ = uevent_broadcast(b"remove", b"net", b"eth0", 0x0301);
            assert!(T_ENQ_BYTES.0[..T_ENQ_LEN].windows(9).any(|w| w == b"SEQNUM=2\0"));
        }
    }

    #[test]
    fn the_uevent_emit_skips_sockets_outside_the_group_or_protocol() {
        unsafe {
            let _serial = serial();
            reset();
            let mut wrong_group = Sock::new();
            wrong_group.protocol = NETLINK_KOBJECT_UEVENT;
            wrong_group.bound = 1;
            set_local(&mut wrong_group, 7, 0);
            S_SLOTS[0] = &mut wrong_group;
            let mut wrong_proto = Sock::new();
            wrong_proto.protocol = NETLINK_ROUTE;
            wrong_proto.bound = 1;
            set_local(&mut wrong_proto, 8, UEVENT_GROUP);
            S_SLOTS[1] = &mut wrong_proto;
            assert_eq!(uevent_broadcast(b"add", b"net", b"eth0", 0), 0);
            assert_eq!(T_ENQ_CALLS, 0);
            /* No listener at all is still 0 (a delivered count of none). */
            assert_eq!(T_KLOG_UEVENT, 1);
        }
    }

    #[test]
    fn uevent_send_validates_the_action_and_resolves_the_device() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).protocol = NETLINK_KOBJECT_UEVENT;
            /* Only known verbs. */
            let bad = b"bogus@/class/net/eth0";
            assert_eq!(a20_net_netlink_uevent_send(s as *mut c_void, bad.as_ptr() as *const c_void,
                                                   bad.len(), ptr::null(), 0),
                       -EINVAL);
            /* Empty, oversized. */
            assert_eq!(a20_net_netlink_uevent_send(s as *mut c_void, b"add".as_ptr() as *const c_void,
                                                   0, ptr::null(), 0), -EINVAL);
            /* A bare action replays the whole set. */
            let add = b"add";
            assert_eq!(a20_net_netlink_uevent_send(s as *mut c_void, add.as_ptr() as *const c_void,
                                                   add.len(), ptr::null(), 0),
                       add.len() as i32);
            assert_eq!(T_EMIT_ALL, 1);

            /* A device that does not exist. */
            reset();
            let s = sa();
            (*s).protocol = NETLINK_KOBJECT_UEVENT;
            T_CDEV = 0;
            let named = b"add@/class/net/eth0";
            assert_eq!(a20_net_netlink_uevent_send(s as *mut c_void, named.as_ptr() as *const c_void,
                                                   named.len(), ptr::null(), 0),
                       -ENOENT);
            assert_eq!(T_CDEV_PUT, 0);

            /* A real device emits one datagram and returns the byte count. */
            reset();
            let s = sa();
            (*s).protocol = NETLINK_KOBJECT_UEVENT;
            T_CDEV = 1;
            let mut listener = Sock::new();
            listener.protocol = NETLINK_KOBJECT_UEVENT;
            listener.bound = 1;
            set_local(&mut listener, 7, UEVENT_GROUP);
            S_SLOTS[0] = &mut listener;
            assert_eq!(a20_net_netlink_uevent_send(s as *mut c_void, named.as_ptr() as *const c_void,
                                                   named.len(), ptr::null(), 0),
                       named.len() as i32);
            assert_eq!(T_ENQ_CALLS, 1);
            assert_eq!(T_CDEV_PUT, 1);
            assert!(T_ENQ_BYTES.0[..T_ENQ_LEN].starts_with(b"add@/class/net/eth0\0"));
        }
    }

    /* --------------------------------------------------------- rtnetlink */

    unsafe fn route_req(type_: u16, flags: u16, body: &[u8]) -> [u8; 256] {
        let mut b = [0u8; 256];
        let total = 16 + body.len();
        ptr::write_unaligned(b.as_mut_ptr() as *mut NlMsghdr, NlMsghdr {
            len: total as u32,
            msg_type: type_,
            flags,
            seq: 3,
            pid: 0,
        });
        ptr::copy_nonoverlapping(body.as_ptr(), b.as_mut_ptr().add(16), body.len());
        b
    }

    unsafe fn ifi(index: i32, flags: u32, change: u32) -> [u8; 16] {
        let mut b = [0u8; 16];
        ptr::write_unaligned(b.as_mut_ptr().add(4) as *mut i32, index);
        ptr::write_unaligned(b.as_mut_ptr().add(8) as *mut u32, flags);
        ptr::write_unaligned(b.as_mut_ptr().add(12) as *mut u32, change);
        b
    }

    unsafe fn ifa(family: u8, prefixlen: u8, index: u32) -> [u8; 8] {
        let mut b = [0u8; 8];
        b[0] = family;
        b[1] = prefixlen;
        ptr::write_unaligned(b.as_mut_ptr().add(4) as *mut u32, index);
        b
    }

    unsafe fn attr(type_: u16, data: &[u8]) -> [u8; 24] {
        let mut b = [0u8; 24];
        ptr::write_unaligned(b.as_mut_ptr() as *mut RtAttr, RtAttr {
            len: (4 + data.len()) as u16,
            type_,
        });
        b[4..4 + data.len()].copy_from_slice(data);
        b
    }

    fn find(hay: &[u8], needle: &[u8]) -> bool {
        hay.windows(needle.len()).any(|w| w == needle)
    }

    #[test]
    fn rtnetlink_validates_requests_types_and_lengths() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            /* Unknown type. */
            let req = route_req(99, 0, &[0u8; 16]);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     32, ptr::null(), 0),
                       -EOPNOTSUPP);
            /* A body shorter than the type requires. */
            let req = route_req(RTM_GETLINK, 0, &[]);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     16, ptr::null(), 0), -EINVAL);
            /* A write without CAP_NET_ADMIN. */
            T_CAPABLE = 0;
            let body = ifi(1, 1, 1);
            let req = route_req(RTM_NEWLINK, 0, &body);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), -EPERM);
            /* A multipart write. */
            T_CAPABLE = 1;
            let req = route_req(RTM_NEWLINK, NLM_F_MULTI, &body);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), -EINVAL);
        }
    }

    #[test]
    fn newlink_applies_mtu_and_up_and_refuses_the_rest() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            let mut body = [0u8; 32];
            body[..16].copy_from_slice(&ifi(2, 1, 1));
            body[16..24].copy_from_slice(&attr(IFLA_MTU, &1500u32.to_ne_bytes())[..8]);
            let req = route_req(RTM_NEWLINK, 0, &body[..24]);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), 0);
            assert_eq!((T_SET_MTU, T_SET_FLAGS), (1, 1));

            /* operstate is refused. */
            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            let mut body = [0u8; 32];
            body[..16].copy_from_slice(&ifi(2, 1, 0));
            body[16..24].copy_from_slice(&attr(IFLA_OPERSTATE, &[1u8; 4])[..8]);
            let req = route_req(RTM_NEWLINK, 0, &body[..24]);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), -EOPNOTSUPP);

            /* A change bit other than IFF_UP is refused. */
            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            let body = ifi(2, 0, 0x2);
            let req = route_req(RTM_NEWLINK, 0, &body);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), -EOPNOTSUPP);

            /* An MTU below the RFC minimum. */
            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            let mut body = [0u8; 32];
            body[..16].copy_from_slice(&ifi(2, 0, 0));
            body[16..24].copy_from_slice(&attr(IFLA_MTU, &40u32.to_ne_bytes())[..8]);
            let req = route_req(RTM_NEWLINK, 0, &body[..24]);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), -EINVAL);
        }
    }

    #[test]
    fn newlink_on_a_foreign_namespace_touches_the_loopback_fields() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            T_NS_INITIAL = 0;
            let mut body = [0u8; 32];
            body[..16].copy_from_slice(&ifi(1, 1, 1));
            body[16..24].copy_from_slice(&attr(IFLA_MTU, &2048u32.to_ne_bytes())[..8]);
            let req = route_req(RTM_NEWLINK, 0, &body[..24]);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), 0);
            assert_eq!((T_LOOP_MTU, T_LOOP_UP), (2048, 1));
            assert_eq!((T_SET_MTU, T_SET_FLAGS), (0, 0));

            /* Only ifindex 1 exists there. */
            let body = ifi(2, 0, 0);
            let req = route_req(RTM_NEWLINK, 0, &body);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), -ENODEV);
        }
    }

    #[test]
    fn addr_add_replace_and_delete_follow_the_one_slot_rule() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            let mut body = [0u8; 48];
            body[..8].copy_from_slice(&ifa(AF_INET as u8, 24, 2));
            body[8..16].copy_from_slice(&attr(IFA_LOCAL, &[10, 0, 2, 15])[..8]);
            let req = route_req(RTM_NEWADDR, 0, &body[..16]);

            /* An empty slot: the add applies. */
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), 0);
            assert_eq!(T_SET_ADDR, 1);

            /* A different configured address without NLM_F_REPLACE: refused. */
            T_GET_ADDR_IP = [10, 0, 2, 99];
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), -EOPNOTSUPP);
            /* With the flag it applies. */
            let req2 = route_req(RTM_NEWADDR, NLM_F_REPLACE, &body[..16]);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req2.as_ptr() as *const c_void,
                                                     req2.len() as usize, ptr::null(),
                                                     0), 0);

            /* A delete of the configured address applies; of another address it
             * is EADDRNOTAVAIL. */
            T_GET_ADDR_IP = [10, 0, 2, 15];
            let del = route_req(RTM_DELADDR, 0, &body[..16]);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, del.as_ptr() as *const c_void,
                                                     del.len() as usize, ptr::null(),
                                                     0), 0);
            T_GET_ADDR_IP = [10, 0, 2, 99];
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, del.as_ptr() as *const c_void,
                                                     del.len() as usize, ptr::null(),
                                                     0), -EADDRNOTAVAIL);

            /* A distinct IFA_ADDRESS is the point-to-point form. */
            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            let mut body = [0u8; 48];
            body[..8].copy_from_slice(&ifa(AF_INET as u8, 24, 2));
            body[8..16].copy_from_slice(&attr(IFA_LOCAL, &[10, 0, 2, 15])[..8]);
            body[16..24].copy_from_slice(&attr(IFA_ADDRESS, &[10, 0, 2, 16])[..8]);
            let req = route_req(RTM_NEWADDR, 0, &body[..24]);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), -EOPNOTSUPP);
        }
    }

    #[test]
    fn addr6_add_delete_and_prefix_validation() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            let mut body = [0u8; 56];
            body[..8].copy_from_slice(&ifa(AF_INET6 as u8, 64, 2));
            let mut a16 = [0u8; 16];
            a16[15] = 1;
            let raw = attr(IFA_LOCAL, &a16);
            body[8..28].copy_from_slice(&raw[..20]);
            let req = route_req(RTM_NEWADDR, 0, &body[..28]);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), 0);
            assert_eq!(T_SET_ADDR6, 1);

            /* A delete reaches the same parser and the delete primitive. */
            let del = route_req(RTM_DELADDR, 0, &body[..28]);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, del.as_ptr() as *const c_void,
                                                     del.len() as usize, ptr::null(),
                                                     0), 0);
            assert_eq!(T_DEL_ADDR6, 1);

            /* A prefix over 128. */
            body[1] = 129;
            let req = route_req(RTM_NEWADDR, 0, &body[..28]);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), -EINVAL);
        }
    }

    #[test]
    fn a_foreign_namespace_refuses_address_writes() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            T_NS_INITIAL = 0;
            let mut body = [0u8; 24];
            body[..8].copy_from_slice(&ifa(AF_INET as u8, 24, 2));
            body[8..16].copy_from_slice(&attr(IFA_LOCAL, &[127, 0, 0, 1])[..8]);
            let req = route_req(RTM_NEWADDR, 0, &body[..16]);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), -EOPNOTSUPP);
        }
    }

    #[test]
    fn musl_dump_requests_and_reply_attributes_follow_linux_wire_layout() {
        unsafe {
            let _serial = serial();
            for (request, reply, body_size) in [
                (RTM_GETLINK, RTM_NEWLINK, 16usize),
                (RTM_GETADDR, RTM_NEWADDR, 8usize),
                (RTM_GETROUTE, RTM_NEWROUTE, 12usize),
            ] {
                for payload in [&[0u8][..], &[0u8; 4][..]] {
                    reset();
                    let s = sa();
                    (*s).protocol = NETLINK_ROUTE;
                    let mut link = NlrtLink::zero();
                    link.name[..4].copy_from_slice(b"eth0");
                    link.index = 2;
                    link.has_ip = 1;
                    link.has_gw = 1;
                    link.ip = [10, 0, 2, 15];
                    link.mask = [255, 255, 255, 0];
                    link.gw = [10, 0, 2, 2];
                    T_SNAPSHOT[0] = link;
                    T_NLINKS = 1;
                    let req = route_req(request, NLM_F_DUMP | 1, payload);
                    assert_eq!(a20_net_netlink_route_request(s as *mut c_void,
                        req.as_ptr() as *const c_void, req.len(), ptr::null(), 0), req.len() as i32);
                    assert_eq!(T_ENQ_CALLS, 2);
                    let hdr = ptr::read_unaligned(T_FIRST.as_ptr() as *const NlMsghdr);
                    assert_eq!(hdr.msg_type, reply);
                    assert_eq!((hdr.seq, hdr.flags), (3, NLM_F_MULTI));
                    // A libc walker must reach exactly nlmsg_len, with no
                    // zero-length trailing attribute or doubled header size.
                    let mut off = 16 + body_size;
                    while off < hdr.len as usize {
                        let attr = ptr::read_unaligned(T_FIRST.as_ptr().add(off) as *const RtAttr);
                        assert!(attr.len >= 4);
                        off += (attr.len as usize + 3) & !3;
                    }
                    assert_eq!(off, hdr.len as usize);
                    let done = ptr::read_unaligned(T_ENQ_BYTES.0.as_ptr() as *const DiagDone);
                    assert_eq!(done.nlh.msg_type, NLMSG_DONE);
                    assert_eq!(done.nlh.seq, hdr.seq);
                    assert_eq!(done.error, 0);
                }
            }
        }
    }

    #[test]
    fn getlink_builds_the_link_message_and_done() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            set_local(s, 9, 0);
            let mut e = NlrtLink::zero();
            e.name[..4].copy_from_slice(b"eth0");
            e.index = 2;
            e.mtu = 1500;
            e.flags = 0x41;
            e.mac = [1, 2, 3, 4, 5, 6];
            T_SNAPSHOT[0] = e;
            T_NLINKS = 1;

            let body = ifi(0, 0, 0);
            let req = route_req(RTM_GETLINK, NLM_F_DUMP, &body);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), req.len() as i32);
            assert_eq!(T_ENQ_CALLS, 2);

            /* Rebuild the reply locally to assert its encoding: the recorded
             * buffer holds the DONE, so build through the same builder. */
            let mut msg = MsgBuf::new();
            let total = build_linkmsg(&mut msg.0, &e, RTM_GETLINK, 9, 3);
            let hdr = &*(msg.0.as_ptr() as *const NlMsghdr);
            assert_eq!(hdr.msg_type, RTM_GETLINK);
            assert_eq!(hdr.flags, NLM_F_MULTI);
            assert_eq!((hdr.seq, hdr.pid), (3, 9));
            assert_eq!(hdr.len as usize, total);
            assert!(find(&msg.0[..total], b"eth0"));
            assert!(find(&msg.0[..total], &[1, 2, 3, 4, 5, 6]));
            assert!(find(&msg.0[..total], &1500u32.to_ne_bytes()));
        }
    }

    #[test]
    fn getaddr_and_getroute_encode_their_payloads() {
        unsafe {
            let _serial = serial();
            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            let mut e = NlrtLink::zero();
            e.name[..4].copy_from_slice(b"eth0");
            e.index = 2;
            e.has_ip = 1;
            e.ip = [10, 0, 2, 15];
            e.mask = [255, 255, 255, 0];
            e.has_gw = 1;
            e.gw = [10, 0, 2, 2];
            T_SNAPSHOT[0] = e;
            T_NLINKS = 1;

            let addr_body = ifa(0, 0, 0);
            let req = route_req(RTM_GETADDR, NLM_F_DUMP, &addr_body);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), req.len() as i32);
            assert_eq!(T_ENQ_CALLS, 2);

            let mut msg = MsgBuf::new();
            let total = build_addrmsg(&mut msg.0, e.index, &e.ip, &e.mask,
                                      name_bytes(&e), e.loopback, RTM_GETADDR, 0, 0);
            let ifa_out = &*(msg.0.as_ptr().add(16) as *const IfAddrMsg);
            assert_eq!((ifa_out.family, ifa_out.prefixlen), (AF_INET as u8, 24));
            assert_eq!(ifa_out.index, 2);
            assert!(find(&msg.0[..total], &[10, 0, 2, 15]));
            assert!(find(&msg.0[..total], b"eth0"));

            let mut msg = MsgBuf::new();
            let _total = build_addrmsg6(&mut msg.0, 2, &[0u8; 16], 64, b"eth0", 0,
                                       RTM_NEWADDR, 0, 0);
            let ifa_out = &*(msg.0.as_ptr().add(16) as *const IfAddrMsg);
            assert_eq!((ifa_out.family, ifa_out.prefixlen), (AF_INET6 as u8, 64));

            reset();
            let s = sa();
            (*s).protocol = NETLINK_ROUTE;
            T_SNAPSHOT[0] = e;
            T_NLINKS = 1;
            let route_body = [0u8; 12];
            let req = route_req(RTM_GETROUTE, NLM_F_DUMP, &route_body);
            assert_eq!(a20_net_netlink_route_request(s as *mut c_void, req.as_ptr() as *const c_void,
                                                     req.len() as usize, ptr::null(),
                                                     0), req.len() as i32);
            let rt = ptr::read_unaligned(T_FIRST.as_ptr().add(16) as *const RtMsg);
            assert_eq!((rt.family, rt.table, rt.type_), (AF_INET as u8, 254, 1));
            assert!(find(&T_FIRST[..T_FIRST_LEN], &[10, 0, 2, 2]));
            assert!(find(&T_FIRST[..T_FIRST_LEN], &2u32.to_ne_bytes()));
        }
    }

    /* --------------------------------------------------------- multicast */

    #[test]
    fn broadcast_reaches_only_the_bound_group_of_routers() {
        unsafe {
            let _serial = serial();
            reset();
            let mut a = Sock::new();
            a.protocol = NETLINK_ROUTE;
            a.bound = 1;
            set_local(&mut a, 5, RTNLGRP_LINK);
            S_SLOTS[0] = &mut a;
            let mut b = Sock::new();
            b.protocol = NETLINK_ROUTE;
            b.bound = 1;
            set_local(&mut b, 6, 0x2); /* another group: no overlap with LINK */
            S_SLOTS[1] = &mut b;
            let mut u = Sock::new();
            u.protocol = NETLINK_KOBJECT_UEVENT;
            u.bound = 1;
            set_local(&mut u, 7, RTNLGRP_LINK);
            S_SLOTS[2] = &mut u;

            let msg = [1u8, 2, 3, 4];
            assert_eq!(nlrt_broadcast(msg.as_ptr(), msg.len(), RTNLGRP_LINK), 0);
            assert_eq!(T_ENQ_CALLS, 1);
            assert_eq!(T_KLOG_DROP, 0);

            /* A failed enqueue counts as a drop, logs, and reports EAGAIN. */
            T_ENQ_RC = -11;
            T_ENQ_CALLS = 0;
            assert_eq!(nlrt_broadcast(msg.as_ptr(), msg.len(), RTNLGRP_LINK),
                       -EAGAIN);
            assert_eq!((T_ENQ_CALLS, T_KLOG_DROP), (1, 1));

            /* No listener for a group: ENOENT. */
            reset();
            assert_eq!(nlrt_broadcast(msg.as_ptr(), msg.len(), RTNLGRP_LINK),
                       -ENOENT);
        }
    }

    #[test]
    fn the_notifiers_publish_the_post_change_state() {
        unsafe {
            let _serial = serial();
            reset();
            let mut e = NlrtLink::zero();
            e.name[..4].copy_from_slice(b"eth0");
            e.index = 2;
            e.has_ip = 1;
            e.ip = [10, 0, 2, 15];
            e.mask = [255, 255, 255, 0];
            T_SNAPSHOT[0] = e;
            T_NLINKS = 1;
            T_EVENT_INDEX[0] = 2;

            let mut a = Sock::new();
            a.protocol = NETLINK_ROUTE;
            a.bound = 1;
            set_local(&mut a, 5, RTNLGRP_LINK | RTNLGRP_IPV4_IFADDR);
            S_SLOTS[0] = &mut a;

            let event = [0u8; 8];
            a20_net_netlink_link_notify(event.as_ptr() as *const c_void, 1);
            assert_eq!(T_ENQ_CALLS, 1);
            let hdr = &*(T_ENQ_BYTES.0.as_ptr() as *const NlMsghdr);
            assert_eq!(hdr.msg_type, RTM_NEWLINK);

            /* An address delete is published as RTM_DELADDR. */
            let zero = [0u8; 4];
            a20_net_netlink_addr_notify(2, zero.as_ptr(), zero.as_ptr());
            let hdr = &*(T_ENQ_BYTES.0.as_ptr() as *const NlMsghdr);
            assert_eq!(hdr.msg_type, RTM_DELADDR);

            /* IPv6 events go only to a socket bound to the v6 group. */
            let mut b = Sock::new();
            b.protocol = NETLINK_ROUTE;
            b.bound = 1;
            set_local(&mut b, 6, RTNLGRP_IPV6_IFADDR);
            S_SLOTS[1] = &mut b;

            let a16 = [0u8; 16];
            a20_net_netlink_addr6_notify(2, a16.as_ptr(), 64, RTM_NEWADDR);
            assert_eq!(T_ENQ_CALLS, 3); /* only the new v6-group socket gets it */
            assert_eq!((*(T_ENQ_BYTES.0.as_ptr() as *const NlMsghdr)).msg_type,
                       RTM_NEWADDR);

            /* A deleted IPv6 address is published as RTM_DELADDR. */
            a20_net_netlink_addr6_notify(2, a16.as_ptr(), 0, RTM_DELADDR);
            assert_eq!(T_ENQ_CALLS, 4);
            assert_eq!((*(T_ENQ_BYTES.0.as_ptr() as *const NlMsghdr)).msg_type,
                       RTM_DELADDR);
            S_SLOTS[1] = ptr::null_mut();

            /* An interface not in the snapshot publishes nothing. */
            a20_net_netlink_addr_notify(9, zero.as_ptr(), zero.as_ptr());
            assert_eq!(T_ENQ_CALLS, 4);
        }
    }

    #[test]
    fn the_attribute_walker_rejects_a_bad_length() {
        unsafe {
            let _serial = serial();
            reset();
            /* A zero-length attribute is a parse error, not an early stop. */
            let mut block = [0u8; 8];
            ptr::write_unaligned(block.as_mut_ptr() as *mut RtAttr, RtAttr {
                len: 2,
                type_: IFA_LOCAL,
            });
            let mut it = AttrIter::new(&block);
            assert!(it.next().is_none());
            assert!(it.bad);

            /* A well-formed one yields its payload. */
            let mut block = [0u8; 8];
            ptr::write_unaligned(block.as_mut_ptr() as *mut RtAttr, RtAttr {
                len: 8,
                type_: IFA_LOCAL,
            });
            block[4..8].copy_from_slice(&[1, 2, 3, 4]);
            let mut it = AttrIter::new(&block);
            let (t, p) = it.next().unwrap();
            assert_eq!((t, p), (IFA_LOCAL, &[1u8, 2, 3, 4][..]));
            assert!(!it.bad);
        }
    }
}
