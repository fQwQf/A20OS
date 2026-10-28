#![no_std]

//! The netif-configuration and link-event state machine of lwip_stack.c: the
//! address/flag/MTU transactions the netlink layer drives, the carrier
//! transition detector, the deferred link-event flush, and the AF_PACKET
//! transmit entry.
//!
//! The C side keeps the primitives this module calls: the lwIP lock, the netif
//! lookup and its field accessors, the device ops, the event collection over the
//! per-device state array, and the netlink notify calls.
//!
//! Everything that is a *decision* is here: which validation refuses a write,
//! what an address notification carries (the mask captured before the write, and
//! the zeroed mask a delete carries), that an admin up/down is published
//! synchronously while a carrier flip is deferred and coalesced by device, that
//! an IPv6 address is promoted to VALID without duplicate-address detection, and
//! the status mapping of a device transmit.

use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::ptr;
use core::sync::atomic::{AtomicBool, Ordering};

const EIO: i32 = 5;
const ENODEV: i32 = 19;
const EINVAL: i32 = 22;
const ENOSPC: i32 = 28;
const EOPNOTSUPP: i32 = 95;
const EADDRNOTAVAIL: i32 = 99;

const A20_LWIP_IF_F_UP: u32 = 0x1;

/* rtnetlink message numbers, restated because this module does not include the
 * netlink parser.  They are part of the wire ABI and must not drift. */
const RTM_NEWADDR: u16 = 20;
const RTM_DELADDR: u16 = 21;
const MAX_LINKS: usize = 32;

/// `nlrt_link_event_t`, filled by the collect primitive.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LinkEvent {
    pub index: u32,
    pub want_up: u8,
}

impl LinkEvent {
    const fn zero() -> LinkEvent {
        LinkEvent { index: 0, want_up: 0 }
    }
}

unsafe extern "C" {
    fn a20_lwip_stack_lock(flags: *mut u64);
    fn a20_lwip_stack_unlock(flags: u64);
    fn a20_lwip_stack_netif_by_index(ifindex: u32) -> *mut c_void;
    fn a20_lwip_stack_netif_has_state(n: *mut c_void) -> i32;
    fn a20_lwip_stack_netif_hwaddr(n: *mut c_void, out: *mut u8);
    fn a20_lwip_stack_netif_is_up(n: *mut c_void) -> i32;
    fn a20_lwip_stack_netif_is_link_up(n: *mut c_void) -> i32;
    fn a20_lwip_stack_netif_index(n: *mut c_void) -> u32;
    fn a20_lwip_stack_default_index() -> i32;
    fn a20_lwip_stack_set_ip(n: *mut c_void, addr: *const u8);
    fn a20_lwip_stack_set_mask(n: *mut c_void, mask: *const u8);
    fn a20_lwip_stack_set_gw(n: *mut c_void, gw: *const u8);
    fn a20_lwip_stack_get_ip(n: *mut c_void, out: *mut u8);
    fn a20_lwip_stack_get_mask(n: *mut c_void, out: *mut u8);
    fn a20_lwip_stack_get_gw(n: *mut c_void, out: *mut u8);
    fn a20_lwip_stack_add_ip6(n: *mut c_void, addr: *const u8) -> i32;
    fn a20_lwip_stack_del_ip6(n: *mut c_void, addr: *const u8) -> i32;
    fn a20_lwip_stack_set_mtu(n: *mut c_void, mtu: u16);
    fn a20_lwip_stack_set_up(n: *mut c_void, up: i32);
    fn a20_lwip_stack_frame_size() -> i32;
    fn a20_lwip_stack_max_devs() -> i32;
    fn a20_lwip_stack_device_link_up(n: *mut c_void) -> i32;
    fn a20_lwip_stack_set_link(n: *mut c_void, up: i32);
    fn a20_lwip_stack_link_pending_mark(n: *mut c_void);
    fn a20_lwip_stack_link_pending_any() -> i32;
    fn a20_lwip_stack_collect_link_events(out: *mut c_void, max: i32) -> i32;
    fn a20_lwip_stack_notify_link_events(events: *const c_void, n: i32);
    fn a20_lwip_stack_notify_addr(ifindex: u32, addr: *const u8, mask: *const u8);
    fn a20_lwip_stack_notify_addr6(ifindex: u32, addr: *const u8, prefixlen: u8,
                                   msg_type: u16);
    fn a20_lwip_stack_netif_sendable(n: *mut c_void) -> i32;
    fn a20_lwip_stack_device_send(n: *mut c_void, frame: *const u8, len: usize) -> i32;
}

fn ip4_isany(addr: &[u8; 4]) -> bool {
    addr[0] == 0 && addr[1] == 0 && addr[2] == 0 && addr[3] == 0
}

/* ------------------------------------------------------- carrier detector */

/// A driver carrier flip, recorded as a pending link event.
///
/// Only a transition, not every poll: setting the link up on every pass would
/// flood RTNLGRP_LINK with a message per poll per device.  The event is queued
/// here and published by the flush, which is reachable from the same poll path.
#[no_mangle]
pub unsafe extern "C" fn a20_lwip_netif_sync_link_state(n: *mut c_void) {
    if n.is_null() || a20_lwip_stack_netif_has_state(n) == 0 {
        return;
    }
    let link_up = a20_lwip_stack_device_link_up(n) != 0;
    let is_up = a20_lwip_stack_netif_is_link_up(n) != 0;
    if link_up && !is_up {
        a20_lwip_stack_set_link(n, 1);
    } else if !link_up && is_up {
        a20_lwip_stack_set_link(n, 0);
    } else {
        return;
    }
    a20_lwip_stack_link_pending_mark(n);
}

/// Publish the link-state changes collected under the lwIP lock.
///
/// Runs with the lwIP lock **not** held: the notify walks the socket table under
/// net locks, and the lock contract forbids holding the two together.  Events
/// are coalesced by device, so two flips between two flushes publish only the
/// settled state.
#[no_mangle]
pub unsafe extern "C" fn a20_lwip_netlink_flush_events() {
    if a20_lwip_stack_link_pending_any() == 0 {
        return;
    }
    let max = a20_lwip_stack_max_devs();
    let cap = if max <= 0 {
        0
    } else if (max as usize) < MAX_LINKS {
        max as usize
    } else {
        MAX_LINKS
    };
    let mut events = [LinkEvent::zero(); MAX_LINKS];
    let n = a20_lwip_stack_collect_link_events(events.as_mut_ptr() as *mut c_void,
                                               cap as i32);
    if n > 0 {
        a20_lwip_stack_notify_link_events(events.as_ptr() as *const c_void, n);
    }
}

/* --------------------------------------------------------- netif queries */

#[no_mangle]
pub unsafe extern "C" fn a20_lwip_if_hwaddr(ifindex: u32, out: *mut u8) -> i32 {
    if out.is_null() {
        return -EINVAL;
    }
    let mut flags = 0u64;
    a20_lwip_stack_lock(&mut flags);
    let n = a20_lwip_stack_netif_by_index(ifindex);
    if n.is_null() {
        a20_lwip_stack_unlock(flags);
        return -ENODEV;
    }
    a20_lwip_stack_netif_hwaddr(n, out);
    a20_lwip_stack_unlock(flags);
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_lwip_if_up(ifindex: u32) -> i32 {
    let mut flags = 0u64;
    a20_lwip_stack_lock(&mut flags);
    let n = a20_lwip_stack_netif_by_index(ifindex);
    let up = if n.is_null() {
        -ENODEV
    } else if a20_lwip_stack_netif_is_up(n) != 0
        && a20_lwip_stack_netif_is_link_up(n) != 0
    {
        1
    } else {
        0
    };
    a20_lwip_stack_unlock(flags);
    up
}

#[no_mangle]
pub unsafe extern "C" fn a20_lwip_if_default_index() -> i32 {
    let mut flags = 0u64;
    a20_lwip_stack_lock(&mut flags);
    let idx = a20_lwip_stack_default_index();
    a20_lwip_stack_unlock(flags);
    idx
}

/* ------------------------------------------------------- address changes */

/// The single IPv4 slot lwIP keeps per netif.  An all-zero address clears the
/// slot and takes the netmask and gateway with it: lwIP derives subnet
/// membership from the (addr, netmask) pair, so keeping the netmask would
/// advertise a prefix for an interface that has no address.
#[no_mangle]
pub unsafe extern "C" fn a20_lwip_if_set_addr(ifindex: u32, addr: *const u8,
                                              mask: *const u8,
                                              gw: *const u8) -> i32 {
    if addr.is_null() {
        return -EINVAL;
    }
    let mut want = [0u8; 4];
    ptr::copy_nonoverlapping(addr, want.as_mut_ptr(), 4);
    /* What the event will carry: the address that is being applied, and the
     * mask captured before the write.  A delete's mask is zeroed below. */
    let eff_addr = want;
    let mut eff_mask = [0u8; 4];

    let mut flags = 0u64;
    a20_lwip_stack_lock(&mut flags);
    let n = a20_lwip_stack_netif_by_index(ifindex);
    if n.is_null() {
        a20_lwip_stack_unlock(flags);
        return -ENODEV;
    }
    a20_lwip_stack_get_mask(n, eff_mask.as_mut_ptr());
    a20_lwip_stack_set_ip(n, want.as_ptr());
    if ip4_isany(&want) {
        let zero = [0u8; 4];
        a20_lwip_stack_set_mask(n, zero.as_ptr());
        a20_lwip_stack_set_gw(n, zero.as_ptr());
        /* The prefix went with the address: RTM_DELADDR names the address that
         * went away, and a stale /24 would tell a listener to remove the wrong
         * prefix. */
        eff_mask = [0u8; 4];
    } else if !mask.is_null() {
        let mut m = [0u8; 4];
        ptr::copy_nonoverlapping(mask, m.as_mut_ptr(), 4);
        a20_lwip_stack_set_mask(n, m.as_ptr());
        eff_mask = m;
    } else if !gw.is_null() {
        let mut g = [0u8; 4];
        ptr::copy_nonoverlapping(gw, g.as_mut_ptr(), 4);
        a20_lwip_stack_set_gw(n, g.as_ptr());
    }
    a20_lwip_stack_unlock(flags);
    /* Outside the lwIP lock: the notify path takes net locks. */
    a20_lwip_stack_notify_addr(ifindex, eff_addr.as_ptr(), eff_mask.as_ptr());
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_lwip_if_get_addr(ifindex: u32, addr: *mut u8,
                                             mask: *mut u8, gw: *mut u8) -> i32 {
    if addr.is_null() || mask.is_null() || gw.is_null() {
        return -EINVAL;
    }
    let mut flags = 0u64;
    a20_lwip_stack_lock(&mut flags);
    let n = a20_lwip_stack_netif_by_index(ifindex);
    if n.is_null() {
        a20_lwip_stack_unlock(flags);
        return -ENODEV;
    }
    a20_lwip_stack_get_ip(n, addr);
    a20_lwip_stack_get_mask(n, mask);
    a20_lwip_stack_get_gw(n, gw);
    a20_lwip_stack_unlock(flags);
    0
}

/// Add one IPv6 address, promoted straight to VALID: the address came from an
/// explicit administrative request, not from a router advertisement, so no DAD
/// runs.  This is the DAD contract for the write path; see
/// docs/net/network-config-design.md.
#[no_mangle]
pub unsafe extern "C" fn a20_lwip_if_set_addr6(ifindex: u32, addr: *const u8,
                                              prefixlen: u8) -> i32 {
    if addr.is_null() {
        return -EINVAL;
    }
    if prefixlen > 128 {
        return -EINVAL;
    }
    let mut want = [0u8; 16];
    ptr::copy_nonoverlapping(addr, want.as_mut_ptr(), 16);
    /* An all-zero address is not a state an interface can be put into, and
     * lwIP would store it and never use it. */
    if want.iter().all(|b| *b == 0) {
        return -EINVAL;
    }
    let mut flags = 0u64;
    a20_lwip_stack_lock(&mut flags);
    let n = a20_lwip_stack_netif_by_index(ifindex);
    if n.is_null() {
        a20_lwip_stack_unlock(flags);
        return -ENODEV;
    }
    let e = a20_lwip_stack_add_ip6(n, want.as_ptr());
    if e < 0 {
        a20_lwip_stack_unlock(flags);
        return -ENOSPC;
    }
    a20_lwip_stack_unlock(flags);
    a20_lwip_stack_notify_addr6(ifindex, want.as_ptr(), prefixlen, RTM_NEWADDR);
    0
}

/// Delete one administratively added IPv6 address.  The event is a DELADDR and
/// carries no prefix length, matching the IPv4 delete convention: this tree
/// does not store a per-address prefix, and the socket cannot discover one.
#[no_mangle]
pub unsafe extern "C" fn a20_lwip_if_del_addr6(ifindex: u32,
                                               addr: *const u8) -> i32 {
    if addr.is_null() {
        return -EINVAL;
    }
    let mut want = [0u8; 16];
    ptr::copy_nonoverlapping(addr, want.as_mut_ptr(), 16);
    if want.iter().all(|b| *b == 0) {
        return -EINVAL;
    }
    let mut flags = 0u64;
    a20_lwip_stack_lock(&mut flags);
    let n = a20_lwip_stack_netif_by_index(ifindex);
    if n.is_null() {
        a20_lwip_stack_unlock(flags);
        return -ENODEV;
    }
    let e = a20_lwip_stack_del_ip6(n, want.as_ptr());
    a20_lwip_stack_unlock(flags);
    if e < 0 {
        return -EADDRNOTAVAIL;
    }
    a20_lwip_stack_notify_addr6(ifindex, want.as_ptr(), 0, RTM_DELADDR);
    0
}

/* ------------------------------------------------------------ MTU, flags */

#[no_mangle]
pub unsafe extern "C" fn a20_lwip_if_set_mtu(ifindex: u32, mtu: u16) -> i32 {
    if mtu < 68 {
        /* RFC 791 minimum link MTU. */
        return -EINVAL;
    }
    /* An MTU larger than the profile's frame buffer is a broken one: the
     * receive path would truncate every full-size frame and the transmit path
     * would refuse it outright. */
    if mtu as i32 + 14 > a20_lwip_stack_frame_size() {
        return -EINVAL;
    }
    let mut flags = 0u64;
    a20_lwip_stack_lock(&mut flags);
    let n = a20_lwip_stack_netif_by_index(ifindex);
    if n.is_null() {
        a20_lwip_stack_unlock(flags);
        return -ENODEV;
    }
    a20_lwip_stack_set_mtu(n, mtu);
    a20_lwip_stack_unlock(flags);
    0
}

/// IFF_UP, the single netif flag user space may drive; carrier is re-derived
/// from the driver on every poll.  An admin up/down is published synchronously
/// because the caller (RTM_NEWLINK) is waiting for a result: deferring it would
/// let the caller observe the new state from a dump before the event arrives.
#[no_mangle]
pub unsafe extern "C" fn a20_lwip_if_set_flags(ifindex: u32, flags: u32,
                                               mask: u32) -> i32 {
    if mask == 0 {
        return -EINVAL;
    }
    if mask & !A20_LWIP_IF_F_UP != 0 {
        return -EOPNOTSUPP;
    }
    let mut lf = 0u64;
    a20_lwip_stack_lock(&mut lf);
    let n = a20_lwip_stack_netif_by_index(ifindex);
    if n.is_null() {
        a20_lwip_stack_unlock(lf);
        return -ENODEV;
    }
    a20_lwip_stack_set_up(n, if flags & A20_LWIP_IF_F_UP != 0 { 1 } else { 0 });
    let index = a20_lwip_stack_netif_index(n);
    let up = if a20_lwip_stack_netif_is_link_up(n) != 0 { 1u8 } else { 0 };
    a20_lwip_stack_unlock(lf);
    let ev = LinkEvent { index, want_up: up };
    a20_lwip_stack_notify_link_events(&ev as *const LinkEvent as *const c_void, 1);
    0
}

/* ------------------------------------------------------------ packet TX */

#[no_mangle]
pub unsafe extern "C" fn a20_lwip_packet_tx(ifindex: u32, frame: *const u8,
                                           len: usize) -> i32 {
    if frame.is_null() || len == 0 || len > 0xffff {
        return -EINVAL;
    }
    let mut flags = 0u64;
    a20_lwip_stack_lock(&mut flags);
    let n = a20_lwip_stack_netif_by_index(ifindex);
    if n.is_null() || a20_lwip_stack_netif_sendable(n) == 0 {
        a20_lwip_stack_unlock(flags);
        return -ENODEV;
    }
    let r = a20_lwip_stack_device_send(n, frame, len);
    a20_lwip_stack_unlock(flags);
    if r == len as i32 { len as i32 } else { -EIO }
}


/* ------------------------------------------------------------- route table
 *
 * Prefix routes behind the Linux RTM_NEWROUTE ABI.  lwIP has no route table:
 * ip4_route selects by netmask or the default netif, which cannot express
 * "100.64.0.0/10 dev tun0" (tailscale) or an exit-node 0.0.0.0/1 +
 * 128.0.0.0/1 pair.  LWIP_HOOK_IP4_ROUTE_SRC (net/lwip_port/lwipopts.h) asks
 * this table first and returns NULL when nothing matches, so lwIP's own
 * selection stays the fallback.
 *
 * Ownership: this table is the only mutable shared state in the module, and it
 * is deliberately a single lock protecting one fixed array.  Callers never see
 * a pointer into it -- every operation is a value transaction (prefix in,
 * ifindex out), so a concurrent delete cannot invalidate a caller's view.
 */
const ROUTE_MAX: usize = 16;
const ROUTE_SPINS: u32 = 1 << 22;

#[derive(Clone, Copy)]
struct RouteEntry {
    used: bool,
    dst: u32, /* network byte order, as a sockaddr_in carries it */
    prefix: u8,
    ifindex: u16,
}

struct RouteTable {
    locked: AtomicBool,
    entries: UnsafeCell<[RouteEntry; ROUTE_MAX]>,
}

/* SAFETY: every access to `entries` is made while `locked` is held. */
unsafe impl Sync for RouteTable {}

static ROUTES: RouteTable = RouteTable {
    locked: AtomicBool::new(false),
    entries: UnsafeCell::new([RouteEntry {
        used: false,
        dst: 0,
        prefix: 0,
        ifindex: 0,
    }; ROUTE_MAX]),
};

fn route_lock() {
    for _ in 0..ROUTE_SPINS {
        if ROUTES
            .locked
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
        {
            return;
        }
        core::hint::spin_loop();
    }
    /* The critical sections are a few field writes; a table that never yields
     * is a deadlock in a caller, not contention, so observe the lock rather
     * than corrupt the table. */
}

fn route_unlock() {
    ROUTES.locked.store(false, Ordering::Release);
}

fn route_mask(prefix: u8) -> u32 {
    if prefix == 0 {
        0
    } else {
        u32::MAX << (32 - prefix as u32)
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_route_add(dst: u32, prefix: u8, ifindex: u16) -> i32 {
    if prefix > 32 || ifindex == 0 {
        return -EINVAL;
    }
    let want = dst & route_mask(prefix);
    route_lock();
    let t = &mut *ROUTES.entries.get();
    let mut slot = ROUTE_MAX;
    for i in 0..ROUTE_MAX {
        if t[i].used && t[i].prefix == prefix && t[i].dst == want && t[i].ifindex == ifindex {
            slot = i;
            break;
        }
    }
    if slot == ROUTE_MAX {
        for i in 0..ROUTE_MAX {
            if !t[i].used {
                slot = i;
                break;
            }
        }
    }
    let rc = if slot == ROUTE_MAX {
        -ENOSPC
    } else {
        t[slot] = RouteEntry {
            used: true,
            dst: want,
            prefix,
            ifindex,
        };
        0
    };
    route_unlock();
    rc
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_route_del(dst: u32, prefix: u8, ifindex: u16) -> i32 {
    if prefix > 32 {
        return -EINVAL;
    }
    let want = dst & route_mask(prefix);
    route_lock();
    let t = &mut *ROUTES.entries.get();
    let mut rc = -ESRCH_RUST;
    for i in 0..ROUTE_MAX {
        if t[i].used
            && t[i].prefix == prefix
            && t[i].dst == want
            && (ifindex == 0 || t[i].ifindex == ifindex)
        {
            t[i].used = false;
            rc = 0;
            break;
        }
    }
    route_unlock();
    rc
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_route_flush_if(ifindex: u16) {
    if ifindex == 0 {
        return;
    }
    route_lock();
    let t = &mut *ROUTES.entries.get();
    for i in 0..ROUTE_MAX {
        if t[i].used && t[i].ifindex == ifindex {
            t[i].used = false;
        }
    }
    route_unlock();
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_route_lookup(dst: u32) -> u32 {
    let mut best_prefix = 0u8;
    let mut ifindex = 0u32;
    let mut have = false;
    route_lock();
    let t = &*ROUTES.entries.get();
    for i in 0..ROUTE_MAX {
        if !t[i].used {
            continue;
        }
        let m = route_mask(t[i].prefix);
        if (dst & m) != (t[i].dst & m) {
            continue;
        }
        if !have || t[i].prefix > best_prefix {
            best_prefix = t[i].prefix;
            ifindex = t[i].ifindex as u32;
            have = true;
        }
    }
    route_unlock();
    if have {
        ifindex
    } else {
        0
    }
}

const ESRCH_RUST: i32 = 3;

#[cfg(test)]
#[allow(static_mut_refs)]
mod tests {
    use super::*;

    #[repr(C)]
    struct Netif {
        index: u32,
        up: i32,
        link_up: i32,
        has_state: i32,
        sendable: i32,
        hwaddr: [u8; 8],
        ip: [u8; 4],
        mask: [u8; 4],
        gw: [u8; 4],
        mtu: u16,
        send_rc: i32,
    }

    impl Netif {
        const fn new() -> Netif {
            Netif {
                index: 2,
                up: 0,
                link_up: 0,
                has_state: 1,
                sendable: 1,
                hwaddr: [1, 2, 3, 4, 5, 6, 0, 0],
                ip: [10, 0, 2, 15],
                mask: [255, 255, 255, 0],
                gw: [10, 0, 2, 2],
                mtu: 1500,
                send_rc: 0,
            }
        }
    }

    static mut N_A: Netif = Netif::new();
    static mut T_LOCK: i32 = 0;
    static mut T_DEFAULT_INDEX: i32 = 2;
    static mut T_FRAME_SIZE: i32 = 1536;
    static mut T_MAX_DEVS: i32 = 8;
    static mut T_DEV_LINK_UP: i32 = 1;
    static mut T_LINK_SET: i32 = 0;
    static mut T_LINK_SET_UP: i32 = 0;
    static mut T_PENDING_MARK: i32 = 0;
    static mut T_PENDING_ANY: i32 = 0;
    static mut T_COLLECT: i32 = 0;
    static mut T_COLLECT_MAX: i32 = 0;
    static mut T_NOTIFY_EVENTS: i32 = 0;
    static mut T_NOTIFY_LAST_UP: i32 = -1;
    static mut T_NOTIFY_ADDR: i32 = 0;
    static mut T_NOTIFY_ADDR_BYTES: [u8; 4] = [0; 4];
    static mut T_NOTIFY_MASK_BYTES: [u8; 4] = [0; 4];
    static mut T_NOTIFY_ADDR6: i32 = 0;
    static mut T_NOTIFY_ADDR6_PLEN: u8 = 0;
    static mut T_NOTIFY_ADDR6_TYPE: i32 = 0;
    static mut T_DEL6: i32 = 0;
    static mut T_DEL6_RC: i32 = 0;
    static mut T_ADD6: i32 = 0;
    static mut T_ADD6_RC: i32 = 0;
    static mut T_SET_IP: i32 = 0;
    static mut T_SET_MASK: i32 = 0;
    static mut T_SET_GW: i32 = 0;
    static mut T_SET_MTU: i32 = 0;
    static mut T_SET_UP: i32 = 0;
    static mut T_SEND: i32 = 0;
    static mut T_SEND_LEN: usize = 0;
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
        N_A = Netif::new();
        T_LOCK = 0;
        T_DEFAULT_INDEX = 2;
        T_FRAME_SIZE = 1536;
        T_MAX_DEVS = 8;
        T_DEV_LINK_UP = 1;
        T_LINK_SET = 0;
        T_LINK_SET_UP = 0;
        T_PENDING_MARK = 0;
        T_PENDING_ANY = 0;
        T_COLLECT = 0;
        T_COLLECT_MAX = 0;
        T_NOTIFY_EVENTS = 0;
        T_NOTIFY_LAST_UP = -1;
        T_NOTIFY_ADDR = 0;
        T_NOTIFY_ADDR_BYTES = [0; 4];
        T_NOTIFY_MASK_BYTES = [0; 4];
        T_NOTIFY_ADDR6 = 0;
        T_NOTIFY_ADDR6_PLEN = 0;
        T_NOTIFY_ADDR6_TYPE = 0;
        T_DEL6 = 0;
        T_DEL6_RC = 0;
        T_ADD6 = 0;
        T_ADD6_RC = 0;
        T_SET_IP = 0;
        T_SET_MASK = 0;
        T_SET_GW = 0;
        T_SET_MTU = 0;
        T_SET_UP = 0;
        T_SEND = 0;
        T_SEND_LEN = 0;
        T_TRACE = [0; 32];
        T_TRACE_N = 0;
    }

    unsafe fn n() -> *mut c_void {
        ptr::addr_of_mut!(N_A) as *mut c_void
    }

    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_lock(_f: *mut u64) {
        T_LOCK += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_unlock(_f: u64) {}
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_netif_by_index(ifindex: u32) -> *mut c_void {
        if ifindex == (*ptr::addr_of!(N_A)).index { n() } else { ptr::null_mut() }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_netif_has_state(np: *mut c_void) -> i32 {
        (*(np as *const Netif)).has_state
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_netif_hwaddr(np: *mut c_void, out: *mut u8) {
        ptr::copy_nonoverlapping((*(np as *const Netif)).hwaddr.as_ptr(), out, 8);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_netif_is_up(np: *mut c_void) -> i32 {
        (*(np as *const Netif)).up
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_netif_is_link_up(np: *mut c_void) -> i32 {
        (*(np as *const Netif)).link_up
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_netif_index(np: *mut c_void) -> u32 {
        (*(np as *const Netif)).index
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_default_index() -> i32 {
        T_DEFAULT_INDEX
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_set_ip(np: *mut c_void, a: *const u8) {
        T_SET_IP += 1;
        trace(b'I');
        ptr::copy_nonoverlapping(a, (*(np as *mut Netif)).ip.as_mut_ptr(), 4);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_set_mask(np: *mut c_void, a: *const u8) {
        T_SET_MASK += 1;
        trace(b'M');
        ptr::copy_nonoverlapping(a, (*(np as *mut Netif)).mask.as_mut_ptr(), 4);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_set_gw(np: *mut c_void, a: *const u8) {
        T_SET_GW += 1;
        trace(b'G');
        ptr::copy_nonoverlapping(a, (*(np as *mut Netif)).gw.as_mut_ptr(), 4);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_get_ip(np: *mut c_void, out: *mut u8) {
        ptr::copy_nonoverlapping((*(np as *const Netif)).ip.as_ptr(), out, 4);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_get_mask(np: *mut c_void, out: *mut u8) {
        ptr::copy_nonoverlapping((*(np as *const Netif)).mask.as_ptr(), out, 4);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_get_gw(np: *mut c_void, out: *mut u8) {
        ptr::copy_nonoverlapping((*(np as *const Netif)).gw.as_ptr(), out, 4);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_add_ip6(_np: *mut c_void, _a: *const u8) -> i32 {
        T_ADD6 += 1;
        T_ADD6_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_del_ip6(_np: *mut c_void, _a: *const u8) -> i32 {
        T_DEL6 += 1;
        T_DEL6_RC
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_set_mtu(np: *mut c_void, mtu: u16) {
        T_SET_MTU += 1;
        (*(np as *mut Netif)).mtu = mtu;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_set_up(np: *mut c_void, up: i32) {
        T_SET_UP += 1;
        let k = &mut *(np as *mut Netif);
        k.up = up;
        /* The driver's carrier state follows the admin state in this stub. */
        k.link_up = up;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_frame_size() -> i32 {
        T_FRAME_SIZE
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_max_devs() -> i32 {
        T_MAX_DEVS
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_device_link_up(_np: *mut c_void) -> i32 {
        T_DEV_LINK_UP
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_set_link(np: *mut c_void, up: i32) {
        T_LINK_SET += 1;
        T_LINK_SET_UP = up;
        (*(np as *mut Netif)).link_up = up;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_link_pending_mark(_np: *mut c_void) {
        T_PENDING_MARK += 1;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_link_pending_any() -> i32 {
        T_PENDING_ANY
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_collect_link_events(out: *mut c_void,
                                                            max: i32) -> i32 {
        T_COLLECT += 1;
        T_COLLECT_MAX = max;
        let ev = LinkEvent { index: 2, want_up: 1 };
        ptr::write_unaligned(out as *mut LinkEvent, ev);
        1
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_notify_link_events(events: *const c_void,
                                                           count: i32) {
        T_NOTIFY_EVENTS += count;
        if count > 0 {
            let ev = ptr::read_unaligned(events as *const LinkEvent);
            T_NOTIFY_LAST_UP = ev.want_up as i32;
        }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_notify_addr(_i: u32, a: *const u8,
                                                    m: *const u8) {
        T_NOTIFY_ADDR += 1;
        ptr::copy_nonoverlapping(a, T_NOTIFY_ADDR_BYTES.as_mut_ptr(), 4);
        ptr::copy_nonoverlapping(m, T_NOTIFY_MASK_BYTES.as_mut_ptr(), 4);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_notify_addr6(_i: u32, _a: *const u8,
                                                     plen: u8, msg_type: u16) {
        T_NOTIFY_ADDR6 += 1;
        T_NOTIFY_ADDR6_PLEN = plen;
        T_NOTIFY_ADDR6_TYPE = msg_type as i32;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_netif_sendable(np: *mut c_void) -> i32 {
        (*(np as *const Netif)).sendable
    }
    #[no_mangle]
    unsafe extern "C" fn a20_lwip_stack_device_send(np: *mut c_void,
                                                    _f: *const u8,
                                                    len: usize) -> i32 {
        T_SEND += 1;
        T_SEND_LEN = len;
        let rc = (*(np as *const Netif)).send_rc;
        if rc != 0 { rc } else { len as i32 }
    }

    /* ------------------------------------------------ carrier transitions */

    #[test]
    fn sync_link_state_publishes_only_transitions() {
        unsafe {
            let _serial = serial();
            reset();
            /* Up: a transition, marked pending. */
            a20_lwip_netif_sync_link_state(n());
            assert_eq!((T_LINK_SET, T_LINK_SET_UP, T_PENDING_MARK), (1, 1, 1));
            assert_eq!((*n().cast::<Netif>()).link_up, 1);

            /* Already up: no transition, no event. */
            T_DEV_LINK_UP = 1;
            a20_lwip_netif_sync_link_state(n());
            assert_eq!((T_LINK_SET, T_PENDING_MARK), (1, 1));

            /* Down: a transition the other way. */
            T_DEV_LINK_UP = 0;
            a20_lwip_netif_sync_link_state(n());
            assert_eq!((T_LINK_SET, T_LINK_SET_UP, T_PENDING_MARK), (2, 0, 2));

            /* A netif with no state is ignored. */
            reset();
            (*n().cast::<Netif>()).has_state = 0;
            a20_lwip_netif_sync_link_state(n());
            assert_eq!(T_LINK_SET, 0);
        }
    }

    #[test]
    fn the_flush_is_gated_on_pending_and_calls_the_notify_outside_the_lock() {
        unsafe {
            let _serial = serial();
            reset();
            /* Nothing pending: no collect, no notify, and no lock taken. */
            a20_lwip_netlink_flush_events();
            assert_eq!((T_COLLECT, T_NOTIFY_EVENTS, T_LOCK), (0, 0, 0));

            T_PENDING_ANY = 1;
            a20_lwip_netlink_flush_events();
            assert_eq!((T_COLLECT, T_NOTIFY_EVENTS), (1, 1));
            assert_eq!(T_COLLECT_MAX, 8); /* bounded by the device count */
            assert_eq!(T_NOTIFY_LAST_UP, 1);

            /* A cap larger than the Rust buffer is clamped. */
            reset();
            T_PENDING_ANY = 1;
            T_MAX_DEVS = 1000;
            a20_lwip_netlink_flush_events();
            assert_eq!(T_COLLECT_MAX, MAX_LINKS as i32);
        }
    }

    /* ------------------------------------------------------------ queries */

    #[test]
    fn the_queries_report_the_netif_state() {
        unsafe {
            let _serial = serial();
            reset();
            let mut hw = [0u8; 8];
            assert_eq!(a20_lwip_if_hwaddr(2, hw.as_mut_ptr()), 0);
            assert_eq!(&hw[..6], &[1, 2, 3, 4, 5, 6]);
            assert_eq!(a20_lwip_if_hwaddr(9, hw.as_mut_ptr()), -ENODEV);
            assert_eq!(a20_lwip_if_hwaddr(2, ptr::null_mut()), -EINVAL);

            /* Up requires both the admin and the carrier state. */
            (*n().cast::<Netif>()).up = 1;
            (*n().cast::<Netif>()).link_up = 1;
            assert_eq!(a20_lwip_if_up(2), 1);
            (*n().cast::<Netif>()).link_up = 0;
            assert_eq!(a20_lwip_if_up(2), 0);
            assert_eq!(a20_lwip_if_up(9), -ENODEV);

            assert_eq!(a20_lwip_if_default_index(), 2);
            T_DEFAULT_INDEX = -ENODEV;
            assert_eq!(a20_lwip_if_default_index(), -ENODEV);
        }
    }

    /* ---------------------------------------------------------- addresses */

    #[test]
    fn set_addr_applies_the_mask_and_reports_the_effective_pair() {
        unsafe {
            let _serial = serial();
            reset();
            let a = [10u8, 0, 2, 20];
            let m = [255u8, 255, 255, 0];
            assert_eq!(a20_lwip_if_set_addr(2, a.as_ptr(), m.as_ptr(), ptr::null()), 0);
            assert_eq!((T_SET_IP, T_SET_MASK, T_SET_GW), (1, 1, 0));
            assert_eq!(T_NOTIFY_ADDR_BYTES, a);
            assert_eq!(T_NOTIFY_MASK_BYTES, m);
            /* The notification comes after the write. */
            assert_eq!(&T_TRACE[..2], &[b'I', b'M']);

            /* A gateway without a mask only touches the gateway. */
            reset();
            let g = [10u8, 0, 2, 254];
            assert_eq!(a20_lwip_if_set_addr(2, a.as_ptr(), ptr::null(), g.as_ptr()), 0);
            assert_eq!((T_SET_MASK, T_SET_GW), (0, 1));
            /* The event carries the mask that was in force. */
            assert_eq!(T_NOTIFY_MASK_BYTES, [255, 255, 255, 0]);

            /* Unknown interface: refused before any write or notify. */
            reset();
            assert_eq!(a20_lwip_if_set_addr(9, a.as_ptr(), m.as_ptr(), ptr::null()),
                       -ENODEV);
            assert_eq!((T_SET_IP, T_NOTIFY_ADDR), (0, 0));

            /* Null address. */
            assert_eq!(a20_lwip_if_set_addr(2, ptr::null(), m.as_ptr(), ptr::null()),
                       -EINVAL);
        }
    }

    #[test]
    fn a_delete_clears_the_mask_and_the_event_carries_no_prefix() {
        unsafe {
            let _serial = serial();
            reset();
            let zero = [0u8; 4];
            assert_eq!(a20_lwip_if_set_addr(2, zero.as_ptr(), ptr::null(), ptr::null()), 0);
            /* The slot, the netmask and the gateway are all cleared, and the
             * notification's mask is zeroed even though the slot had a /24. */
            assert_eq!((T_SET_IP, T_SET_MASK, T_SET_GW), (1, 1, 1));
            assert_eq!(T_NOTIFY_MASK_BYTES, [0, 0, 0, 0]);
            assert_eq!(T_NOTIFY_ADDR_BYTES, [0, 0, 0, 0]);
        }
    }

    #[test]
    fn get_addr_reads_the_three_slots() {
        unsafe {
            let _serial = serial();
            reset();
            let mut a = [0u8; 4];
            let mut m = [0u8; 4];
            let mut g = [0u8; 4];
            assert_eq!(a20_lwip_if_get_addr(2, a.as_mut_ptr(), m.as_mut_ptr(),
                                            g.as_mut_ptr()), 0);
            assert_eq!(a, [10, 0, 2, 15]);
            assert_eq!(m, [255, 255, 255, 0]);
            assert_eq!(g, [10, 0, 2, 2]);
            assert_eq!(a20_lwip_if_get_addr(9, a.as_mut_ptr(), m.as_mut_ptr(),
                                            g.as_mut_ptr()), -ENODEV);
            assert_eq!(a20_lwip_if_get_addr(2, ptr::null_mut(), m.as_mut_ptr(),
                                            g.as_mut_ptr()), -EINVAL);
        }
    }

    #[test]
    fn addr6_is_validated_and_promoted_without_dad() {
        unsafe {
            let _serial = serial();
            reset();
            let mut a16 = [0u8; 16];
            a16[15] = 1;
            assert_eq!(a20_lwip_if_set_addr6(2, a16.as_ptr(), 64), 0);
            assert_eq!(T_ADD6, 1);
            assert_eq!((T_NOTIFY_ADDR6, T_NOTIFY_ADDR6_PLEN), (1, 64));
            assert_eq!(T_NOTIFY_ADDR6_TYPE, RTM_NEWADDR as i32);

            /* All-zero is refused. */
            let zero = [0u8; 16];
            assert_eq!(a20_lwip_if_set_addr6(2, zero.as_ptr(), 64), -EINVAL);
            /* A prefix over 128 is refused. */
            assert_eq!(a20_lwip_if_set_addr6(2, a16.as_ptr(), 129), -EINVAL);
            /* A full address table reports ENOSPC. */
            T_ADD6_RC = -1;
            assert_eq!(a20_lwip_if_set_addr6(2, a16.as_ptr(), 64), -ENOSPC);
            assert_eq!(T_NOTIFY_ADDR6, 1);
            /* Unknown interface. */
            assert_eq!(a20_lwip_if_set_addr6(9, a16.as_ptr(), 64), -ENODEV);

            /* Delete: present -> DELADDR event, absent -> EADDRNOTAVAIL. */
            reset();
            assert_eq!(a20_lwip_if_del_addr6(2, a16.as_ptr()), 0);
            assert_eq!(T_DEL6, 1);
            assert_eq!((T_NOTIFY_ADDR6, T_NOTIFY_ADDR6_TYPE), (1, RTM_DELADDR as i32));
            assert_eq!(T_NOTIFY_ADDR6_PLEN, 0);
            T_DEL6_RC = -1;
            assert_eq!(a20_lwip_if_del_addr6(2, a16.as_ptr()), -EADDRNOTAVAIL);
            assert_eq!(a20_lwip_if_del_addr6(9, a16.as_ptr()), -ENODEV);
            assert_eq!(a20_lwip_if_del_addr6(2, ptr::null()), -EINVAL);
            let zero = [0u8; 16];
            assert_eq!(a20_lwip_if_del_addr6(2, zero.as_ptr()), -EINVAL);
        }
    }

    /* -------------------------------------------------------- MTU, flags */

    #[test]
    fn mtu_bounds_are_tied_to_the_rfc_minimum_and_the_frame_buffer() {
        unsafe {
            let _serial = serial();
            reset();
            assert_eq!(a20_lwip_if_set_mtu(2, 67), -EINVAL);
            assert_eq!(a20_lwip_if_set_mtu(2, 68), 0);
            assert_eq!(T_SET_MTU, 1);
            /* 1536-byte frame buffer: 1522 is the last MTU that fits. */
            assert_eq!(a20_lwip_if_set_mtu(2, 1522), 0);
            assert_eq!(a20_lwip_if_set_mtu(2, 1523), -EINVAL);
            assert_eq!(a20_lwip_if_set_mtu(9, 1500), -ENODEV);
            /* A small profile's buffer bounds it further. */
            T_FRAME_SIZE = 512;
            assert_eq!(a20_lwip_if_set_mtu(2, 500), -EINVAL);
        }
    }

    #[test]
    fn flags_drive_only_iff_up_and_publish_the_event_synchronously() {
        unsafe {
            let _serial = serial();
            reset();
            assert_eq!(a20_lwip_if_set_flags(2, 0, 0), -EINVAL);
            /* A mask wider than IFF_UP is refused. */
            assert_eq!(a20_lwip_if_set_flags(2, 0, 0x2), -EOPNOTSUPP);

            assert_eq!(a20_lwip_if_set_flags(2, A20_LWIP_IF_F_UP, A20_LWIP_IF_F_UP), 0);
            assert_eq!(T_SET_UP, 1);
            assert_eq!(T_NOTIFY_EVENTS, 1);
            assert_eq!(T_NOTIFY_LAST_UP, 1);

            /* Down publishes 0. */
            reset();
            (*n().cast::<Netif>()).link_up = 1;
            assert_eq!(a20_lwip_if_set_flags(2, 0, A20_LWIP_IF_F_UP), 0);
            assert_eq!(T_NOTIFY_LAST_UP, 0);

            assert_eq!(a20_lwip_if_set_flags(9, 0, A20_LWIP_IF_F_UP), -ENODEV);
        }
    }

    /* ------------------------------------------------------------ TX */

    #[test]
    fn packet_tx_maps_the_device_result() {
        unsafe {
            let _serial = serial();
            reset();
            let frame = [0u8; 60];
            assert_eq!(a20_lwip_packet_tx(2, frame.as_ptr(), 60), 60);
            assert_eq!((T_SEND, T_SEND_LEN), (1, 60));

            /* A partial transmit is an I/O error. */
            (*n().cast::<Netif>()).send_rc = -1;
            assert_eq!(a20_lwip_packet_tx(2, frame.as_ptr(), 60), -EIO);
            (*n().cast::<Netif>()).send_rc = 30;
            assert_eq!(a20_lwip_packet_tx(2, frame.as_ptr(), 60), -EIO);

            /* Validation and refusals. */
            assert_eq!(a20_lwip_packet_tx(2, ptr::null(), 60), -EINVAL);
            assert_eq!(a20_lwip_packet_tx(2, frame.as_ptr(), 0), -EINVAL);
            assert_eq!(a20_lwip_packet_tx(2, frame.as_ptr(), 0x10000), -EINVAL);
            (*n().cast::<Netif>()).sendable = 0;
            assert_eq!(a20_lwip_packet_tx(2, frame.as_ptr(), 60), -ENODEV);
            assert_eq!(a20_lwip_packet_tx(9, frame.as_ptr(), 60), -ENODEV);
        }
    }
}
