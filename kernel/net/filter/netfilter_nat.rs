#![no_std]

//! Conntrack table and NAT rule state machine for the IPv4 data plane.
//!
//! Rust owns transactions over C-provided bounded storage. CtGuard serializes
//! rule changes, tuple reservation, lookup, reindexing, expiry and snapshots
//! independently of the active TCP/IP backend. Shared entries are copied into
//! local values; no mutable references into the table escape or overlap.
//! New NAT mappings are committed before packet mutation and never evict live
//! mappings. Port allocation probes complete reply tuples, not ports alone.

use core::ptr;
#[cfg(target_has_atomic = "64")]
use core::sync::atomic::AtomicU64;
use core::sync::atomic::{AtomicU32, Ordering};

/* riscv32/arm32 have no 64-bit atomics.  These counters are only touched with
 * CtGuard held, so a plain read-modify-write keeps the same publication
 * discipline the C reference had. */
fn load_stat(p: *mut u64) -> u64 {
    #[cfg(target_has_atomic = "64")]
    {
        unsafe { AtomicU64::from_ptr(p).load(Ordering::Relaxed) }
    }
    #[cfg(not(target_has_atomic = "64"))]
    {
        unsafe { *p }
    }
}

fn add_stat(p: *mut u64, by: u64) {
    #[cfg(target_has_atomic = "64")]
    unsafe {
        AtomicU64::from_ptr(p).fetch_add(by, Ordering::Relaxed);
    }
    #[cfg(not(target_has_atomic = "64"))]
    unsafe {
        *p = (*p).wrapping_add(by);
    }
}

/* ------------------------------------------------------------- constants */
const NONE: u16 = 0xFFFF;
const NO_ADDR: u32 = 0xFFFF_FFFF;
const NO_PORT: u16 = 0xFFFF;

const PROTO_ANY: u8 = 0;
const PROTO_ICMP: u8 = 1;
const PROTO_TCP: u8 = 6;
const PROTO_UDP: u8 = 17;

const ICMP_ECHO_REQUEST: u16 = 8;

const DIR_IN: u8 = 0;
const DIR_OUT: u8 = 1;

const NAT_NONE: u8 = 0;
const NAT_SNAT: u8 = 1;
const NAT_MASQUERADE: u8 = 2;
const NAT_DNAT: u8 = 3;

const CT_NEW: u8 = 0;
const CT_ESTABLISHED: u8 = 1;

const EINVAL: i32 = 22;
const EOPNOTSUPP: i32 = 95;
const ENOSPC: i32 = 28;

const TO_INDEX_TCP_NEW: usize = 0;
const TO_INDEX_TCP_ESTABLISHED: usize = 1;
const TO_INDEX_UDP: usize = 2;

/* Defaults, mirrored from net/netfilter.h. */
const DEFAULT_TCP_NEW_MS: u32 = 30_000;
const DEFAULT_TCP_ESTABLISHED_MS: u32 = 120_000;
const DEFAULT_UDP_MS: u32 = 30_000;

/* -------------------------------------------------------------- mirrors */

/// Mirror of `net_conntrack_entry_t`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ConntrackEntry {
    pub src_addr: u32,
    pub dst_addr: u32,
    pub src_port: u16,
    pub dst_port: u16,
    pub proto: u8,
    pub state: u8,
    pub nat: u8,
    pub dir: u8,
    pub nat_src_addr: u32,
    pub nat_dst_addr: u32,
    pub nat_src_port: u16,
    pub nat_dst_port: u16,
    pub last_ms: u32,
    pub packets: u64,
    pub bytes: u64,
    pub hash_next: u16,
    pub rhash_next: u16,
}

/// Mirror of `net_conntrack_stats_t`.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ConntrackStats {
    pub tracked: u64,
    pub created: u64,
    pub evicted: u64,
    pub timeout: u64,
    pub packets: u64,
    pub bytes: u64,
}

/// Mirror of `net_nat_rule_t`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NatRule {
    pub dir: u8,
    pub proto: u8,
    pub nat: u8,
    pub src_port: u16,
    pub dst_port: u16,
    pub src_addr: u32,
    pub dst_addr: u32,
    pub nat_addr: u32,
    pub nat_port: u16,
    pub matched: u64,
    pub bytes: u64,
}

/// Mirror of `netfilter_frame_t`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Frame {
    pub src_addr: u32,
    pub dst_addr: u32,
    pub src_port: u16,
    pub dst_port: u16,
    pub proto: u8,
    pub has_ports: u8,
    pub ip_off: u16,
    pub l4_off: u16,
    pub tcp_flags: u16,
    pub icmp_off: u16,
    pub icmp_type: u8,
    pub icmp_id: u16,
}

/// Borrowed view of the C-side state this module transacts over.
#[repr(C)]
pub struct CtView {
    pub entries: *mut ConntrackEntry,
    pub max: u32,
    pub buckets: u32,
    pub head: *mut u16,
    pub rhead: *mut u16,
    pub used: *mut u32,
    pub sweep: *mut u32,
    pub enabled: *mut i32,
    pub lifetime: *mut ConntrackStats,
    pub dir_stats: [*mut ConntrackStats; 2],
    pub nat: *mut NatRule,
    pub nat_max: u32,
    pub nat_count: *mut u32,
    /// Three runtime overrides, 0 = use the compiled-in default.
    pub to_ms: *mut u32,
    pub sweeps: *mut u32,
}

unsafe extern "C" {
    fn a20_netfilter_now_ms() -> u32;
    fn a20_netfilter_rewrite_set_addr(
        frame: *mut u8,
        len: usize,
        pkt: *const Frame,
        new_src: u32,
        new_dst: u32,
    );
    fn a20_netfilter_rewrite_set_port(
        frame: *mut u8,
        len: usize,
        pkt: *const Frame,
        is_src: i32,
        new_port: u16,
    );
    fn a20_netfilter_masq_addr(net_idx: i32) -> u32;
    fn a20_netfilter_ipv4_from_str(s: *const u8, len: usize, out: *mut u32) -> i32;
}

/* ------------------------------------------------------------- view access */

/* The caller owns CtGuard. Storage is accessed as owned copies, never as
 * fabricated static mutable references that can alias during chain updates. */
struct View<'a>(&'a CtView);
impl View<'_> {
    unsafe fn entry(&self, i: usize) -> ConntrackEntry {
        debug_assert!(i < self.0.max as usize);
        ptr::read(self.0.entries.add(i))
    }
    unsafe fn put_entry(&self, i: usize, e: ConntrackEntry) {
        debug_assert!(i < self.0.max as usize);
        ptr::write(self.0.entries.add(i), e);
    }
    unsafe fn head(&self, b: usize, reply: bool) -> u16 {
        debug_assert!(b < self.0.buckets as usize);
        ptr::read(if reply { self.0.rhead } else { self.0.head }.add(b))
    }
    unsafe fn put_head(&self, b: usize, reply: bool, value: u16) {
        debug_assert!(b < self.0.buckets as usize);
        ptr::write(if reply { self.0.rhead } else { self.0.head }.add(b), value);
    }
    unsafe fn rule(&self, i: usize) -> NatRule {
        debug_assert!(i < self.0.nat_max as usize);
        ptr::read(self.0.nat.add(i))
    }
    unsafe fn put_rule(&self, i: usize, r: NatRule) {
        debug_assert!(i < self.0.nat_max as usize);
        ptr::write(self.0.nat.add(i), r);
    }
    unsafe fn clear_entries(&self) {
        for b in 0..self.0.buckets as usize {
            self.put_head(b, false, NONE);
            self.put_head(b, true, NONE);
        }
        for i in 0..self.0.max as usize {
            self.put_entry(i, zero_entry());
        }
    }
}
unsafe fn view<'a>(v: *const CtView) -> View<'a> {
    View(&*v)
}

#[cfg(not(test))]
unsafe extern "C" {
    fn a20_netfilter_ct_lock() -> u64;
    fn a20_netfilter_ct_unlock(flags: u64);
}
struct CtGuard(u64);
impl CtGuard {
    fn acquire() -> Self {
        #[cfg(not(test))]
        {
            Self(unsafe { a20_netfilter_ct_lock() })
        }
        #[cfg(test)]
        {
            Self(0)
        } // Unit tests use independent local views, never shared state.
    }
}
impl Drop for CtGuard {
    fn drop(&mut self) {
        let _ = self.0;
        #[cfg(not(test))]
        unsafe {
            a20_netfilter_ct_unlock(self.0)
        }
    }
}

unsafe fn used_slot(e: &ConntrackEntry) -> bool {
    e.proto != 0
}

/// FNV-1a over the tuple's wire bytes, folded to a bucket. The order is fixed
/// (src first) and deliberately not symmetric: a tuple and its reverse land in
/// different buckets, and the lookup compensates by probing both.
fn ct_hash(src: u32, dst: u32, sport: u16, dport: u16, proto: u8, buckets: u32) -> usize {
    let words: [u8; 13] = [
        (src >> 24) as u8,
        (src >> 16) as u8,
        (src >> 8) as u8,
        src as u8,
        (dst >> 24) as u8,
        (dst >> 16) as u8,
        (dst >> 8) as u8,
        dst as u8,
        (sport >> 8) as u8,
        sport as u8,
        (dport >> 8) as u8,
        dport as u8,
        proto,
    ];
    let mut h = 2_166_136_261u32;
    for w in words {
        h ^= w as u32;
        h = h.wrapping_mul(16_777_619);
    }
    (h % buckets) as usize
}

/// Does `e` hold this ICMP echo exchange? The identifier is the same in both
/// directions and the type is normalised to the request value, so the reply of
/// (A->B, id) is (B->A, id) with both 16-bit fields equal.
fn ct_icmp_is(e: &ConntrackEntry, src: u32, dst: u32, id: u16) -> Option<bool> {
    if e.dst_port != id || e.src_port != ICMP_ECHO_REQUEST {
        return None;
    }
    if e.src_addr == src && e.dst_addr == dst {
        return Some(false);
    }
    if e.src_addr == dst && e.dst_addr == src {
        return Some(true);
    }
    None
}

/// The tuple a reply to `e` arrives with; see netfilter_ct_reply_tuple().
fn ct_reply_tuple(e: &ConntrackEntry) -> (u32, u32, u16, u16) {
    let (mut rsrc, mut rdst, mut rsport, mut rdport) =
        (e.dst_addr, e.src_addr, e.dst_port, e.src_port);
    match e.nat {
        NAT_DNAT => {
            rsrc = e.nat_dst_addr;
            if e.nat_dst_port != 0 {
                rsport = e.nat_dst_port;
            }
        }
        NAT_SNAT | NAT_MASQUERADE => {
            rdst = e.nat_src_addr;
            if e.nat_src_port != 0 {
                rdport = e.nat_src_port;
            }
        }
        _ => {}
    }
    if e.proto == PROTO_ICMP {
        (rsrc, rdst, ICMP_ECHO_REQUEST, e.dst_port)
    } else {
        (rsrc, rdst, rsport, rdport)
    }
}

unsafe fn ct_find(
    v: &View,
    src: u32,
    dst: u32,
    sport: u16,
    dport: u16,
    proto: u8,
) -> Option<(usize, bool)> {
    ct_find_except(v, src, dst, sport, dport, proto, None)
}
unsafe fn ct_find_except(
    v: &View,
    src: u32,
    dst: u32,
    sport: u16,
    dport: u16,
    proto: u8,
    skip: Option<usize>,
) -> Option<(usize, bool)> {
    let buckets = v.0.buckets;
    let b0 = ct_hash(src, dst, sport, dport, proto, buckets);
    let b1 = ct_hash(dst, src, dport, sport, proto, buckets);

    for pass in 0..2 {
        let b = if pass == 0 { b0 } else { b1 };
        let mut idx = v.head(b, false);
        while idx != NONE {
            let e = unsafe { v.entry(idx as usize) };
            let next = e.hash_next;
            if Some(idx as usize) == skip {
                idx = next;
                continue;
            }
            if e.proto == proto {
                if proto == PROTO_ICMP {
                    if let Some(rev) = ct_icmp_is(&e, src, dst, dport) {
                        return Some((idx as usize, rev));
                    }
                } else if e.src_addr == src
                    && e.dst_addr == dst
                    && e.src_port == sport
                    && e.dst_port == dport
                {
                    return Some((idx as usize, false));
                } else if e.src_addr == dst
                    && e.dst_addr == src
                    && e.src_port == dport
                    && e.dst_port == sport
                {
                    return Some((idx as usize, true));
                }
            }
            idx = next;
        }
    }

    /* The reply-direction chain: the only place a translated reply can be
     * found. One probe, no swap -- the packet's tuple *is* the reply tuple, and
     * treating its swap as a match would translate in the wrong direction. */

    let mut idx = v.head(b0, true);
    while idx != NONE {
        let e = unsafe { v.entry(idx as usize) };
        let next = e.rhash_next;
        if Some(idx as usize) == skip {
            idx = next;
            continue;
        }
        if e.proto == proto {
            let (rsrc, rdst, rsport, rdport) = ct_reply_tuple(&e);
            if proto == PROTO_ICMP {
                if rsrc == src && rdst == dst && e.dst_port == dport {
                    return Some((idx as usize, true));
                }
            } else if rsrc == src && rdst == dst && rsport == sport && rdport == dport {
                return Some((idx as usize, true));
            }
        }
        idx = next;
    }
    None
}

unsafe fn ct_unlink_from(v: &View, b: usize, idx: usize, reply: bool) {
    let mut prev = NONE;
    let mut i = v.head(b, reply);
    while i != NONE {
        let mut e = v.entry(i as usize);
        let next = if reply { e.rhash_next } else { e.hash_next };
        if i as usize == idx {
            if prev == NONE {
                v.put_head(b, reply, next);
            } else {
                let mut p = v.entry(prev as usize);
                if reply {
                    p.rhash_next = next;
                } else {
                    p.hash_next = next;
                }
                v.put_entry(prev as usize, p);
            }
            if reply {
                e.rhash_next = NONE;
            } else {
                e.hash_next = NONE;
            }
            v.put_entry(i as usize, e);
            return;
        }
        prev = i;
        i = next;
    }
}
unsafe fn ct_link(v: &View, idx: usize, mut e: ConntrackEntry) {
    let b = ct_hash(
        e.src_addr,
        e.dst_addr,
        e.src_port,
        e.dst_port,
        e.proto,
        v.0.buckets,
    );
    let (src, dst, sport, dport) = ct_reply_tuple(&e);
    let rb = ct_hash(src, dst, sport, dport, e.proto, v.0.buckets);
    e.hash_next = v.head(b, false);
    e.rhash_next = v.head(rb, true);
    v.put_entry(idx, e);
    v.put_head(b, false, idx as u16);
    v.put_head(rb, true, idx as u16);
}
unsafe fn ct_unlink(v: &View, idx: usize) {
    let e = v.entry(idx);
    let b = ct_hash(
        e.src_addr,
        e.dst_addr,
        e.src_port,
        e.dst_port,
        e.proto,
        v.0.buckets,
    );
    ct_unlink_from(v, b, idx, false);
    let (src, dst, sport, dport) = ct_reply_tuple(&e);
    let rb = ct_hash(src, dst, sport, dport, e.proto, v.0.buckets);
    ct_unlink_from(v, rb, idx, true);
}

/// Least recently used live entry; ties broken by lower index.
unsafe fn ct_lru(v: &View) -> Option<usize> {
    let mut best: Option<usize> = None;
    let mut best_ms = 0u32;
    for i in 0..v.0.max as usize {
        if !unsafe { used_slot(&v.entry(i)) } {
            continue;
        }
        let ms = v.entry(i).last_ms;
        let better = match best {
            None => true,
            Some(b) => ms < best_ms || (ms == best_ms && i < b),
        };
        if better {
            best = Some(i);
            best_ms = ms;
        }
    }
    best
}

unsafe fn ct_now(_v: &View) -> u32 {
    a20_netfilter_now_ms()
}
unsafe fn ct_timeout_for(v: &View, e: &ConntrackEntry) -> u32 {
    let idx = if e.proto == PROTO_UDP || e.proto == PROTO_ICMP {
        TO_INDEX_UDP
    } else if e.state == CT_ESTABLISHED {
        TO_INDEX_TCP_ESTABLISHED
    } else {
        TO_INDEX_TCP_NEW
    };
    let value = *v.0.to_ms.add(idx);
    if value != 0 {
        value
    } else {
        match idx {
            TO_INDEX_TCP_NEW => DEFAULT_TCP_NEW_MS,
            TO_INDEX_TCP_ESTABLISHED => DEFAULT_TCP_ESTABLISHED_MS,
            _ => DEFAULT_UDP_MS,
        }
    }
}

unsafe fn ct_insert(v: &View, entry: &mut ConntrackEntry) -> i32 {
    /* Conntrack is state, not a cache. Evicting a live tuple permits a new NAT
     * mapping to steal replies from a connection that is still in use. The
     * periodic sweeper is the only automatic reclamation path. */
    if *v.0.used >= v.0.max || !binding_free(v, entry, None) {
        return -ENOSPC;
    }
    let Some(slot) = (0..v.0.max as usize).find(|&i| !used_slot(&v.entry(i))) else {
        return -ENOSPC;
    };
    entry.last_ms = ct_now(v);
    ct_link(v, slot, *entry);
    *v.0.used += 1;
    let lifetime = &mut *v.0.lifetime;
    lifetime.created = lifetime.created.wrapping_add(1);
    lifetime.tracked = *v.0.used as u64;
    0
}

/* ------------------------------------------------------------ entry points */

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_init(v: *const CtView) {
    let _guard = CtGuard::acquire();
    if v.is_null() {
        return;
    }
    let v = unsafe { view(v) };
    unsafe { v.clear_entries() };
    unsafe {
        *v.0.used = 0;
        *v.0.sweep = 0;
        *v.0.enabled = 1;
    }
}

fn zero_entry() -> ConntrackEntry {
    ConntrackEntry {
        src_addr: 0,
        dst_addr: 0,
        src_port: 0,
        dst_port: 0,
        proto: 0,
        state: 0,
        nat: 0,
        dir: 0,
        nat_src_addr: 0,
        nat_dst_addr: 0,
        nat_src_port: 0,
        nat_dst_port: 0,
        last_ms: 0,
        packets: 0,
        bytes: 0,
        hash_next: 0,
        rhash_next: 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_count(v: *const CtView) -> i32 {
    let _guard = CtGuard::acquire();
    if v.is_null() {
        return 0;
    }
    unsafe { *(*v).used as i32 }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_sweeps(v: *const CtView) -> u32 {
    let _guard = CtGuard::acquire();
    if v.is_null() {
        return 0;
    }
    // SAFETY: the view points at the live counter.
    unsafe { AtomicU32::from_ptr((*v).sweeps).load(Ordering::Relaxed) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_lru_victim(v: *const CtView) -> i32 {
    let _guard = CtGuard::acquire();
    if v.is_null() {
        return -1;
    }
    let v = unsafe { view(v) };
    match unsafe { ct_lru(&v) } {
        Some(slot) => unsafe { v.entry(slot).src_port as i32 },
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_inject(
    v: *const CtView,
    src: u32,
    dst: u32,
    sport: u16,
    dport: u16,
    proto: u8,
    state: u8,
) -> i32 {
    let _guard = CtGuard::acquire();
    if v.is_null() {
        return -EINVAL;
    }
    /* Only what the packet path would have created: an untracked flow is a UDP
     * or TCP tuple in one of the two real states. */
    if proto != PROTO_TCP && proto != PROTO_UDP {
        return -EINVAL;
    }
    if state != CT_NEW && state != CT_ESTABLISHED {
        return -EINVAL;
    }
    let v = unsafe { view(v) };
    let mut e = zero_entry();
    e.src_addr = src;
    e.dst_addr = dst;
    e.src_port = sport;
    e.dst_port = dport;
    e.proto = proto;
    e.state = state;
    e.dir = DIR_OUT;
    unsafe { ct_insert(&v, &mut e) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_snapshot(
    v: *const CtView,
    out: *mut ConntrackEntry,
    max: u32,
) -> i32 {
    let _guard = CtGuard::acquire();
    if v.is_null() || out.is_null() || max == 0 {
        return 0;
    }
    let v = unsafe { view(v) };

    let mut n = 0usize;
    for i in 0..v.0.max as usize {
        if n >= max as usize {
            break;
        }
        if unsafe { used_slot(&v.entry(i)) } {
            unsafe { *out.add(n) = v.entry(i) };
            n += 1;
        }
    }
    n as i32
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_flush(v: *const CtView) {
    let _guard = CtGuard::acquire();
    if v.is_null() {
        return;
    }
    let v = unsafe { view(v) };
    unsafe { v.clear_entries() };
    unsafe {
        *v.0.used = 0;
        *v.0.sweep = 0;
        /* `tracked` follows the table; the lifetime counters deliberately do
         * not -- flushing is an operator action and must not erase the
         * reclamation evidence a test came to read. */
        (*v.0.lifetime).tracked = 0;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_get_stats(v: *const CtView, out: *mut ConntrackStats) {
    let _guard = CtGuard::acquire();
    if v.is_null() || out.is_null() {
        return;
    }
    let v = unsafe { view(v) };
    let mut stats = ConntrackStats::default();
    for d in 0..2 {
        let s = v.0.dir_stats[d];
        if s.is_null() {
            continue;
        }
        stats.packets += load_stat(ptr::addr_of_mut!((*s).packets));
        stats.bytes += load_stat(ptr::addr_of_mut!((*s).bytes));
    }
    let lifetime = unsafe { &*v.0.lifetime };
    stats.tracked = unsafe { *v.0.used as u64 };
    stats.created = lifetime.created;
    stats.evicted = lifetime.evicted;
    stats.timeout = lifetime.timeout;
    unsafe { *out = stats };
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_set_enabled(v: *const CtView, on: i32) {
    let _guard = CtGuard::acquire();
    if v.is_null() {
        return;
    }
    /* Entries are kept when tracking is switched off: deleting them would
     * silently break every flow relying on a NAT binding. */
    unsafe { *(*v).enabled = if on != 0 { 1 } else { 0 } };
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_enabled(v: *const CtView) -> i32 {
    let _guard = CtGuard::acquire();
    if v.is_null() {
        return 0;
    }
    unsafe { *(*v).enabled }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_set_timeouts(v: *const CtView, a: u32, b: u32, c: u32) {
    let _guard = CtGuard::acquire();
    if v.is_null() {
        return;
    }
    let view = unsafe { &*v };
    unsafe {
        *view.to_ms.add(0) = a;
        *view.to_ms.add(1) = b;
        *view.to_ms.add(2) = c;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_expire(v: *const CtView, max_scan: u32) -> u32 {
    let _guard = CtGuard::acquire();
    if v.is_null() {
        return 0;
    }
    let v = unsafe { view(v) };

    let max = v.0.max as usize;
    let now = unsafe { ct_now(&v) };
    let mut max_scan = max_scan as usize;
    if max_scan == 0 {
        max_scan = max;
    }

    /* How many times the sweep ran at all: the difference between "nothing was
     * idle" and "nothing ever looked". */
    unsafe {
        AtomicU32::from_ptr(v.0.sweeps).fetch_add(1, Ordering::Relaxed);
    }

    let mut scanned = 0usize;
    let mut freed = 0u32;
    let sweep = unsafe { *v.0.sweep } as usize;
    let mut step = 0usize;
    while step < max && scanned < max_scan {
        let i = (sweep + step) % max;
        scanned += 1;
        if unsafe { used_slot(&v.entry(i)) } {
            let limit = unsafe { ct_timeout_for(&v, &v.entry(i)) };
            /* Unsigned subtract: correct across the tick counter's wrap. */
            if now.wrapping_sub(v.entry(i).last_ms) < limit {
                step += 1;
                continue;
            }
            unsafe { ct_unlink(&v, i) };
            v.put_entry(i, zero_entry());
            unsafe {
                if *v.0.used != 0 {
                    *v.0.used -= 1;
                }
            }
            freed += 1;
            let lifetime = unsafe { &mut *v.0.lifetime };
            lifetime.timeout = lifetime.timeout.wrapping_add(1);
        }
        step += 1;
    }
    unsafe {
        *v.0.sweep = ((sweep + scanned) % max) as u32;
        (*v.0.lifetime).tracked = *v.0.used as u64;
    }
    freed
}

/* --------------------------------------------------------------- the path */

/* Reserve a reply tuple before changing the packet. Checking the complete
 * tuple permits port reuse across different remote endpoints while ensuring
 * that every reply has exactly one owner. All probes hold CtGuard. */
unsafe fn binding_free(v: &View, e: &ConntrackEntry, skip: Option<usize>) -> bool {
    let (src, dst, sport, dport) = ct_reply_tuple(e);
    ct_find_except(v, src, dst, sport, dport, e.proto, skip).is_none()
}
unsafe fn select_nat(
    v: &View,
    e: &mut ConntrackEntry,
    pkt: &Frame,
    dir: u8,
    net_idx: i32,
    skip: Option<usize>,
) -> i32 {
    let Some(rule) = nat_pick(v, pkt, dir) else {
        return 0;
    };
    e.nat = rule.nat;
    if rule.nat == NAT_DNAT {
        e.nat_dst_addr = rule.nat_addr;
        e.nat_dst_port = rule.nat_port;
        return if binding_free(v, e, skip) { 0 } else { -ENOSPC };
    }
    e.nat_src_addr = if rule.nat == NAT_MASQUERADE {
        a20_netfilter_masq_addr(net_idx)
    } else {
        rule.nat_addr
    };
    if e.nat_src_addr == 0 {
        return -EINVAL;
    }
    // ICMP identifiers are not TCP/UDP ports; never feed them to a port writer.
    if e.proto == PROTO_ICMP {
        return if binding_free(v, e, skip) { 0 } else { -ENOSPC };
    }
    e.nat_src_port = if rule.nat_port != 0 {
        rule.nat_port
    } else {
        e.src_port
    };
    if binding_free(v, e, skip) {
        return 0;
    }
    if rule.nat_port != 0 {
        return -ENOSPC;
    } // Explicit fixed mapping is a contract.
    const FIRST: u32 = 1024;
    const PORTS: u32 = 65536 - FIRST;
    let start = ct_hash(
        e.src_addr, e.dst_addr, e.src_port, e.dst_port, e.proto, PORTS,
    );
    // At most one candidate per occupied entry can be taken for this peer.
    // This bound avoids an attacker forcing a full 64K search under the lock.
    for n in 0..=(*v.0.used).min(PORTS - 1) {
        e.nat_src_port = (FIRST + (start as u32 + n) % PORTS) as u16;
        if binding_free(v, e, skip) {
            return 0;
        }
    }
    -ENOSPC
}
unsafe fn translate(
    frame: *mut u8,
    len: usize,
    pkt: &Frame,
    e: &ConntrackEntry,
    reverse: bool,
    dir: u8,
) {
    if e.nat == NAT_SNAT || e.nat == NAT_MASQUERADE {
        if reverse && dir == DIR_IN {
            a20_netfilter_rewrite_set_addr(frame, len, pkt, pkt.src_addr, e.src_addr);
            if e.proto != PROTO_ICMP {
                a20_netfilter_rewrite_set_port(frame, len, pkt, 0, e.src_port);
            }
        } else if !reverse && dir == DIR_OUT {
            a20_netfilter_rewrite_set_addr(frame, len, pkt, e.nat_src_addr, pkt.dst_addr);
            if e.proto != PROTO_ICMP {
                a20_netfilter_rewrite_set_port(frame, len, pkt, 1, e.nat_src_port);
            }
        }
    } else if e.nat == NAT_DNAT {
        if reverse && dir == DIR_OUT {
            a20_netfilter_rewrite_set_addr(frame, len, pkt, e.dst_addr, pkt.dst_addr);
            if e.proto != PROTO_ICMP {
                a20_netfilter_rewrite_set_port(frame, len, pkt, 1, e.dst_port);
            }
        } else if !reverse && dir == DIR_IN {
            a20_netfilter_rewrite_set_addr(frame, len, pkt, pkt.src_addr, e.nat_dst_addr);
            if e.proto != PROTO_ICMP {
                a20_netfilter_rewrite_set_port(frame, len, pkt, 0, e.nat_dst_port);
            }
        }
    }
}

/* ICMP exceptions are RELATED to a live binding, never new flows. We do not
 * refresh timeouts or change TCP state in response to unauthenticated errors.
 * RFC 5508: validate both IP headers, the complete ICMP checksum, the quoted
 * tuple and error direction before touching any byte. Minimum TCP quotes have
 * only eight transport bytes: never require or invent a full TCP header. */
fn wire16(b: &[u8], at: usize) -> u16 {
    u16::from_be_bytes([b[at], b[at + 1]])
}
fn wire32(b: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}
fn put16(b: &mut [u8], at: usize, x: u16) {
    b[at..at + 2].copy_from_slice(&x.to_be_bytes());
}
fn put32(b: &mut [u8], at: usize, x: u32) {
    b[at..at + 4].copy_from_slice(&x.to_be_bytes());
}
fn checksum(b: &[u8]) -> u16 {
    let mut sum = 0u32;
    for x in b.chunks(2) {
        sum += (x[0] as u32) << 8 | *x.get(1).unwrap_or(&0) as u32;
    }
    while sum >> 16 != 0 {
        sum = (sum & 65535) + (sum >> 16);
    }
    !(sum as u16)
}
fn delta16(check: u16, old: u16, new: u16) -> u16 {
    let mut sum = (!check as u32 & 65535) + (!old as u32 & 65535) + new as u32;
    while sum >> 16 != 0 {
        sum = (sum & 65535) + (sum >> 16);
    }
    !(sum as u16)
}
fn quote_rewrite(
    b: &mut [u8],
    at: usize,
    ihl: usize,
    old: (u32, u32, u16, u16),
    new: (u32, u32, u16, u16),
    proto: u8,
) {
    let transport = at + ihl;
    let checksum_at = if proto == PROTO_UDP {
        Some(transport + 6)
    } else if proto == PROTO_TCP && b.len() >= transport + 18 {
        Some(transport + 16)
    } else {
        None
    };
    if let Some(c) = checksum_at {
        let original = wire16(b, c);
        if original != 0 || proto != PROTO_UDP {
            let mut check = original;
            for (before, after) in [(old.0, new.0), (old.1, new.1)] {
                check = delta16(check, (before >> 16) as u16, (after >> 16) as u16);
                check = delta16(check, before as u16, after as u16);
            }
            check = delta16(check, old.2, new.2);
            check = delta16(check, old.3, new.3);
            put16(
                b,
                c,
                if proto == PROTO_UDP && check == 0 {
                    65535
                } else {
                    check
                },
            );
        }
    }
    put32(b, at + 12, new.0);
    put32(b, at + 16, new.1);
    if proto != PROTO_ICMP {
        put16(b, transport, new.2);
        put16(b, transport + 2, new.3);
    }
    put16(b, at + 10, 0);
    let check = checksum(&b[at..at + ihl]);
    put16(b, at + 10, check);
}
unsafe fn translate_icmp_error(v: &View, bytes: &mut [u8], pkt: &Frame, dir: u8) -> i32 {
    let ip = pkt.ip_off as usize;
    if bytes.len() < ip + 20 {
        return -EINVAL;
    }
    let ihl = (bytes[ip] & 15) as usize * 4;
    let end = ip + wire16(bytes, ip + 2) as usize;
    if ihl < 20 || end > bytes.len() || end < ip + ihl + 8 {
        return -EINVAL;
    }
    let icmp = ip + ihl;
    let ty = bytes[icmp];
    let code = bytes[icmp + 1];
    if !matches!(ty, 3 | 11 | 12) {
        return 0;
    }
    if (ty == 3 && code > 15) || (ty == 11 && code > 1) || (ty == 12 && code > 2) {
        return -EINVAL;
    }
    // Fragments must be reassembled before exception parsing/translation.
    if wire16(bytes, ip + 6) & 0x3fff != 0
        || checksum(&bytes[ip..icmp]) != 0
        || checksum(&bytes[icmp..end]) != 0
    {
        return -EINVAL;
    }
    let quote = icmp + 8;
    if end < quote + 20 || bytes[quote] >> 4 != 4 {
        return -EINVAL;
    }
    let qihl = (bytes[quote] & 15) as usize * 4;
    if qihl < 20
        || end < quote + qihl + 8
        || (wire16(bytes, quote + 2) as usize) < qihl + 8
        || wire16(bytes, quote + 6) & 0x1fff != 0
        || checksum(&bytes[quote..quote + qihl]) != 0
    {
        return -EINVAL;
    }
    let protocol = bytes[quote + 9];
    let transport = quote + qihl;
    if protocol == PROTO_TCP && (wire16(bytes, quote + 2) as usize) < qihl + 20 {
        return -EINVAL;
    }
    let (sport, dport) = match protocol {
        PROTO_TCP | PROTO_UDP => (wire16(bytes, transport), wire16(bytes, transport + 2)),
        PROTO_ICMP if matches!(bytes[transport], 0 | 8) => (8, wire16(bytes, transport + 4)),
        _ => return 0,
    };
    // A quote ending halfway through a TCP checksum cannot be repaired.
    if protocol == PROTO_TCP && end == transport + 17 {
        return -EINVAL;
    }
    let tuple = (
        wire32(bytes, quote + 12),
        wire32(bytes, quote + 16),
        sport,
        dport,
    );
    if pkt.dst_addr != tuple.0 {
        return -EINVAL;
    }
    // Reverse the quoted wire packet and use the same collision-protected
    // original/reply indexes as data traffic. Echo keeps its normalised ID
    // fields instead of swapping them as TCP/UDP ports.
    let (probe_source, probe_destination) = if protocol == PROTO_ICMP {
        (ICMP_ECHO_REQUEST, tuple.3)
    } else {
        (tuple.3, tuple.2)
    };
    let Some((index, _)) = ct_find(
        v,
        tuple.1,
        tuple.0,
        probe_source,
        probe_destination,
        protocol,
    ) else {
        return 0;
    };
    let now = ct_now(v);
    let e = v.entry(index);
    if e.proto != protocol
        || e.nat == NAT_NONE
        || now.wrapping_sub(e.last_ms) >= ct_timeout_for(v, &e)
    {
        return 0;
    }
    let (rs, rd, rsp, rdp) = ct_reply_tuple(&e);
    let forward = (
        rd,
        rs,
        if protocol == PROTO_ICMP { 8 } else { rdp },
        if protocol == PROTO_ICMP {
            e.dst_port
        } else {
            rsp
        },
    );
    let original_reply = (
        e.dst_addr,
        e.src_addr,
        if protocol == PROTO_ICMP {
            8
        } else {
            e.dst_port
        },
        if protocol == PROTO_ICMP {
            e.dst_port
        } else {
            e.src_port
        },
    );
    let snat = e.nat == NAT_SNAT || e.nat == NAT_MASQUERADE;
    let (target, outer_source, outer_destination) = if tuple == forward
        && ((snat && dir == DIR_IN) || (!snat && dir == DIR_OUT))
    {
        (
            (e.src_addr, e.dst_addr, e.src_port, e.dst_port),
            if !snat && pkt.src_addr == tuple.1 {
                e.dst_addr
            } else {
                pkt.src_addr
            },
            if snat { e.src_addr } else { pkt.dst_addr },
        )
    } else if tuple == original_reply && ((snat && dir == DIR_OUT) || (!snat && dir == DIR_IN)) {
        (
            (
                rs,
                rd,
                if protocol == PROTO_ICMP { 8 } else { rsp },
                if protocol == PROTO_ICMP {
                    e.dst_port
                } else {
                    rdp
                },
            ),
            if snat && pkt.src_addr == e.src_addr {
                rd
            } else {
                pkt.src_addr
            },
            if !snat { rs } else { pkt.dst_addr },
        )
    } else {
        return 0;
    };
    // No fallible operation remains after this point. Keep MTU/type/code
    // and all quote data intact except translated fields/checksums.
    let b = &mut bytes[..end];
    quote_rewrite(b, quote, qihl, tuple, target, protocol);
    put32(b, ip + 12, outer_source);
    put32(b, ip + 16, outer_destination);
    put16(b, icmp + 2, 0);
    let check = checksum(&b[icmp..end]);
    put16(b, icmp + 2, check);
    put16(b, ip + 10, 0);
    let check = checksum(&b[ip..icmp]);
    put16(b, ip + 10, check);
    return 0;
}

/// A negative result requires the filter to drop the unchanged packet. A new
/// binding is committed before rewriting; resource exhaustion cannot emit an
/// untracked translated packet or overwrite another connection's reply tuple.
#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_process(
    v: *const CtView,
    frame: *mut u8,
    len: usize,
    net_idx: i32,
    dir: u8,
    pkt: *const Frame,
) -> i32 {
    let _guard = CtGuard::acquire();
    if v.is_null() || frame.is_null() || pkt.is_null() || len < 14 {
        return -EINVAL;
    }
    let v = view(v);
    let pkt = &*pkt;
    let bytes = core::slice::from_raw_parts_mut(frame, len);
    let ip = pkt.ip_off as usize;
    // Until the ingress datagram owner can reassemble before policy/NAT,
    // never translate only fragment zero and leak untranslated tails. This
    // check precedes tuple allocation, including first fragments with ports.
    if *v.0.nat_count != 0 && ip != 0 && len >= ip + 8 && bytes[ip] >> 4 == 4 &&
        wire16(bytes, ip + 6) & 0x3fff != 0 { return -EOPNOTSUPP; }
    if pkt.proto == PROTO_ICMP && pkt.icmp_off == 0 {
        return translate_icmp_error(&v, bytes, pkt, dir);
    }
    let echo = pkt.proto == PROTO_ICMP && pkt.icmp_off != 0;
    if pkt.proto != PROTO_TCP && pkt.proto != PROTO_UDP && !echo {
        return 0;
    }
    let (sport, dport) = if echo {
        (ICMP_ECHO_REQUEST, pkt.icmp_id)
    } else {
        (pkt.src_port, pkt.dst_port)
    };
    // Fragment handling belongs before this transaction; no tuple is invented.
    if !echo && (pkt.has_ports == 0 || pkt.l4_off == 0) {
        return 0;
    }
    let st = v.0.dir_stats[if dir == DIR_IN { 0 } else { 1 }];
    if !st.is_null() {
        add_stat(ptr::addr_of_mut!((*st).packets), 1);
        add_stat(ptr::addr_of_mut!((*st).bytes), len as u64);
    }
    if let Some((idx, reverse)) = ct_find(&v, pkt.src_addr, pkt.dst_addr, sport, dport, pkt.proto) {
        let mut e = v.entry(idx);
        // A forwarded packet is first tracked on ingress. Apply SNAT only on
        // its later egress and reindex the newly reserved reply tuple atomically.
        if e.nat == NAT_NONE && !reverse && dir == DIR_OUT {
            let rc = select_nat(&v, &mut e, pkt, dir, net_idx, Some(idx));
            if rc < 0 {
                return rc;
            }
            if e.nat != NAT_NONE {
                ct_unlink(&v, idx);
                ct_link(&v, idx, e);
                e = v.entry(idx);
            }
        }
        e.last_ms = ct_now(&v);
        e.packets = e.packets.wrapping_add(1);
        e.bytes = e.bytes.wrapping_add(len as u64);
        if reverse || (pkt.proto == PROTO_TCP && pkt.tcp_flags & 0x12 == 0x10) {
            e.state = CT_ESTABLISHED;
        }
        v.put_entry(idx, e);
        translate(frame, len, pkt, &e, reverse, dir);
        return 0;
    }
    if *v.0.enabled == 0 {
        // Tracking may be disabled for stateless use, but configured NAT must
        // not silently leak untranslated private addresses.
        return if nat_pick(&v, pkt, dir).is_some() {
            -ENOSPC
        } else {
            0
        };
    }
    let mut e = zero_entry();
    e.src_addr = pkt.src_addr;
    e.dst_addr = pkt.dst_addr;
    e.src_port = sport;
    e.dst_port = dport;
    e.proto = pkt.proto;
    e.dir = dir;
    e.state = CT_NEW;
    e.packets = 1;
    e.bytes = len as u64;
    let rc = select_nat(&v, &mut e, pkt, dir, net_idx, None);
    if rc < 0 {
        return rc;
    }
    let rc = ct_insert(&v, &mut e);
    if rc < 0 {
        return rc;
    }
    translate(frame, len, pkt, &e, false, dir);
    0
}

/* ------------------------------------------------------------- NAT rules */

unsafe fn nat_pick(v: &View, pkt: &Frame, dir: u8) -> Option<NatRule> {
    // CtGuard serializes rules and bindings; no retry can silently skip NAT.
    let n = (*v.0.nat_count).min(v.0.nat_max) as usize;
    for i in 0..n {
        let r = v.rule(i);
        if r.dir == dir
            && (r.proto == PROTO_ANY || r.proto == pkt.proto)
            && (r.src_addr == NO_ADDR || r.src_addr == pkt.src_addr)
            && (r.dst_addr == NO_ADDR || r.dst_addr == pkt.dst_addr)
            && (r.src_port == NO_PORT || r.src_port == pkt.src_port)
            && (r.dst_port == NO_PORT || r.dst_port == pkt.dst_port)
        {
            return Some(r);
        }
    }
    None
}

fn parse_u32(bytes: &[u8], max: u32) -> Result<u32, i32> {
    let mut p: u32 = 0;
    for &c in bytes {
        if !(b'0'..=b'9').contains(&c) {
            return Err(EINVAL);
        }
        p = p.wrapping_mul(10).wrapping_add((c - b'0') as u32);
        if p > max {
            return Err(EINVAL);
        }
    }
    Ok(p)
}

/// Parse one "add" line into a NAT rule. Same keys as a filter rule plus
/// action/to/toport.
#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_nat_parse(
    line: *const u8,
    len: usize,
    out: *mut NatRule,
) -> i32 {
    if line.is_null() || out.is_null() {
        return -EINVAL;
    }
    let mut len = len;
    let line = unsafe { core::slice::from_raw_parts(line, len) };
    while len != 0 && (line[len - 1] == b'\n' || line[len - 1] == b'\r') {
        len -= 1;
    }
    let line = &line[..len];

    let mut r = NatRule {
        dir: 0,
        proto: PROTO_ANY,
        nat: NAT_NONE,
        src_port: NO_PORT,
        dst_port: NO_PORT,
        src_addr: NO_ADDR,
        dst_addr: NO_ADDR,
        nat_addr: NO_ADDR,
        nat_port: 0,
        matched: 0,
        bytes: 0,
    };

    if len == 0 {
        return -EINVAL;
    }

    let mut i;
    if len >= 2 && &line[..2] == b"in" {
        r.dir = DIR_IN;
        i = 2;
    } else if len >= 3 && &line[..3] == b"out" {
        r.dir = DIR_OUT;
        i = 3;
    } else {
        return -EINVAL;
    }
    if i < len && line[i] == b' ' {
        i += 1;
    }

    while i < len {
        let mut klen = 0usize;
        while i + klen < len && line[i + klen] != b'=' {
            klen += 1;
        }
        if i + klen >= len {
            break; /* trailing garbage, as in the filter */
        }
        let key = &line[i..i + klen];
        let vstart = i + klen + 1;
        let mut vlen = 0usize;
        while vstart + vlen < len && line[vstart + vlen] != b' ' && line[vstart + vlen] != b'\t' {
            vlen += 1;
        }
        let val = &line[vstart..vstart + vlen];

        if key == b"proto" {
            r.proto = if val == b"any" {
                PROTO_ANY
            } else if val == b"icmp" {
                PROTO_ICMP
            } else if val == b"tcp" {
                PROTO_TCP
            } else if val == b"udp" {
                PROTO_UDP
            } else if !val.is_empty() && val.len() <= 3 && val[0].is_ascii_digit() {
                match parse_u32(val, 255) {
                    Ok(p) => p as u8,
                    Err(_) => return -EINVAL,
                }
            } else {
                return -EINVAL;
            };
        } else if key == b"src" {
            if val == b"any" {
                r.src_addr = NO_ADDR;
            } else if unsafe { ipv4_from_str(val.as_ptr(), val.len(), &mut r.src_addr) } != 1 {
                return -EINVAL;
            }
        } else if key == b"dst" {
            if val == b"any" {
                r.dst_addr = NO_ADDR;
            } else if unsafe { ipv4_from_str(val.as_ptr(), val.len(), &mut r.dst_addr) } != 1 {
                return -EINVAL;
            }
        } else if key == b"sport" || key == b"dport" {
            let is_src = key == b"sport";
            if val == b"any" {
                if is_src {
                    r.src_port = NO_PORT;
                } else {
                    r.dst_port = NO_PORT;
                }
            } else {
                match parse_u32(val, NO_PORT as u32) {
                    Ok(p) => {
                        if is_src {
                            r.src_port = p as u16;
                        } else {
                            r.dst_port = p as u16;
                        }
                    }
                    Err(_) => return -EINVAL,
                }
            }
        } else if key == b"action" {
            r.nat = if val == b"snat" {
                NAT_SNAT
            } else if val == b"masquerade" {
                NAT_MASQUERADE
            } else if val == b"dnat" {
                NAT_DNAT
            } else {
                return -EINVAL;
            };
        } else if key == b"to" {
            if unsafe { ipv4_from_str(val.as_ptr(), val.len(), &mut r.nat_addr) } != 1 {
                return -EINVAL;
            }
        } else if key == b"toport" {
            match parse_u32(val, NO_PORT as u32) {
                Ok(p) => r.nat_port = p as u16,
                Err(_) => return -EINVAL,
            }
        } else {
            return -EINVAL;
        }

        i = vstart + vlen;
        while i < len && (line[i] == b' ' || line[i] == b'\t') {
            i += 1;
        }
    }

    /* Refuse a rule that would match packets and then do nothing, or that names
     * two conflicting targets. */
    if r.nat == NAT_NONE {
        return -EINVAL;
    }
    if r.nat == NAT_DNAT && r.dir != DIR_IN {
        return -EINVAL;
    }
    if r.nat == NAT_MASQUERADE {
        if r.nat_addr != NO_ADDR || r.nat_port != 0 {
            return -EINVAL;
        }
    } else if r.nat_addr == NO_ADDR {
        return -EINVAL;
    }
    unsafe { *out = r };
    0
}

/// Validate and append one rule under the same guard as packet transactions.
#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_nat_add(v: *const CtView, rule: *const NatRule) -> i32 {
    let _guard = CtGuard::acquire();
    if v.is_null() || rule.is_null() {
        return -EINVAL;
    }
    let r = unsafe { &*rule };
    if r.nat != NAT_SNAT && r.nat != NAT_MASQUERADE && r.nat != NAT_DNAT {
        return -EINVAL;
    }
    if r.dir != DIR_IN && r.dir != DIR_OUT {
        return -EINVAL;
    }
    /* A masquerade with an explicit address is ambiguous, and a plain snat/dnat
     * without one would match and then do nothing. */
    if (r.nat == NAT_SNAT || r.nat == NAT_DNAT) && r.nat_addr == NO_ADDR {
        return -EINVAL;
    }
    if r.nat == NAT_MASQUERADE && r.nat_addr != NO_ADDR {
        return -EINVAL;
    }
    if r.nat_port != 0 && r.proto != PROTO_TCP && r.proto != PROTO_UDP {
        return -EINVAL;
    }
    /* DNAT is an input-hook translation and SNAT/MASQUERADE are output-hook
     * translations: the packet path only applies them in that direction, so a
     * rule installed on the wrong hook would match and then do nothing. */
    if r.nat == NAT_DNAT && r.dir != DIR_IN {
        return -EINVAL;
    }
    if (r.nat == NAT_SNAT || r.nat == NAT_MASQUERADE) && r.dir != DIR_OUT {
        return -EINVAL;
    }

    let v = unsafe { view(v) };
    let count = unsafe { *v.0.nat_count } as usize;
    if count >= v.0.nat_max as usize {
        return -ENOSPC;
    }
    unsafe {
        v.put_rule(count, *r);
        *v.0.nat_count = (count + 1) as u32;
    }
    count as i32
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_nat_del(v: *const CtView, index: u32) -> i32 {
    let _guard = CtGuard::acquire();
    if v.is_null() {
        return -EINVAL;
    }
    let v = unsafe { view(v) };
    let count = unsafe { *v.0.nat_count } as usize;
    if index as usize >= count {
        return -EINVAL;
    }
    unsafe {
        let mut i = index as usize;
        while i + 1 < count {
            v.put_rule(i, v.rule(i + 1));
            i += 1;
        }
        let new_count = count - 1;
        *v.0.nat_count = new_count as u32;
        v.put_rule(new_count, zero_nat_rule());
    }
    0
}

fn zero_nat_rule() -> NatRule {
    NatRule {
        dir: 0,
        proto: 0,
        nat: 0,
        src_port: 0,
        dst_port: 0,
        src_addr: 0,
        dst_addr: 0,
        nat_addr: 0,
        nat_port: 0,
        matched: 0,
        bytes: 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_nat_reset(v: *const CtView) {
    let _guard = CtGuard::acquire();
    if v.is_null() {
        return;
    }
    let v = unsafe { view(v) };
    unsafe {
        for i in 0..v.0.nat_max as usize {
            v.put_rule(i, zero_nat_rule());
        }
        *v.0.nat_count = 0;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_nat_count(v: *const CtView) -> i32 {
    let _guard = CtGuard::acquire();
    if v.is_null() {
        return 0;
    }
    unsafe { AtomicU32::from_ptr((*v).nat_count).load(Ordering::Acquire) as i32 }
}

/// Copy the matching rule under CtGuard; no rule pointer is returned.
#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_nat_pick(
    v: *const CtView,
    pkt: *const Frame,
    dir: u8,
    out: *mut NatRule,
) -> i32 {
    let _guard = CtGuard::acquire();
    if v.is_null() || pkt.is_null() || out.is_null() {
        return 0;
    }
    let v = unsafe { view(v) };
    match unsafe { nat_pick(&v, &*pkt, dir) } {
        Some(r) => {
            unsafe { *out = r };
            1
        }
        None => 0,
    }
}

#[cfg(not(test))]
unsafe fn ipv4_from_str(s: *const u8, len: usize, out: *mut u32) -> i32 {
    unsafe { a20_netfilter_ipv4_from_str(s, len, out) }
}

#[cfg(test)]
unsafe fn ipv4_from_str(s: *const u8, len: usize, out: *mut u32) -> i32 {
    /* Test-only stand-in for netfilter.c's parser: four dot-separated decimal
     * octets, host order. */
    if s.is_null() || out.is_null() || len == 0 {
        return 0;
    }
    let bytes = unsafe { core::slice::from_raw_parts(s, len) };
    let mut v: u32 = 0;
    let mut parts = 0usize;
    let mut cur: u32 = 0;
    let mut digits = 0usize;
    for &c in bytes.iter().chain(core::iter::once(&b'.')) {
        if c == b'.' {
            if digits == 0 || parts == 4 {
                return 0;
            }
            v = (v << 8) | cur;
            parts += 1;
            cur = 0;
            digits = 0;
        } else if c.is_ascii_digit() {
            cur = cur * 10 + (c - b'0') as u32;
            if cur > 255 {
                return 0;
            }
            digits += 1;
        } else {
            return 0;
        }
    }
    if parts != 4 {
        return 0;
    }
    unsafe { *out = v };
    1
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hash_is_order_sensitive_so_both_probes_are_needed() {
        let a = ct_hash(0x0102_0304, 0x0506_0708, 1, 2, PROTO_TCP, 64);
        let b = ct_hash(0x0506_0708, 0x0102_0304, 2, 1, PROTO_TCP, 64);
        /* Not a hard guarantee for every input, but for this pair the buckets
         * differ; the point is that the function is not symmetric by design. */
        assert_ne!(a, b);
        assert!(a < 64 && b < 64);
    }

    #[test]
    fn reply_tuple_follows_the_translation_kind() {
        let mut e = zero_entry();
        e.src_addr = 10;
        e.dst_addr = 20;
        e.src_port = 1000;
        e.dst_port = 80;
        assert_eq!(ct_reply_tuple(&e), (20, 10, 80, 1000));

        e.nat = NAT_DNAT;
        e.nat_dst_addr = 30;
        e.nat_dst_port = 8080;
        assert_eq!(ct_reply_tuple(&e), (30, 10, 8080, 1000));

        e.nat = NAT_SNAT;
        e.nat_src_addr = 40;
        e.nat_src_port = 4000;
        assert_eq!(ct_reply_tuple(&e), (20, 40, 80, 4000));

        /* A zero translated port means "unchanged". */
        e.nat_src_port = 0;
        assert_eq!(ct_reply_tuple(&e), (20, 40, 80, 1000));
    }

    #[test]
    fn icmp_pairing_uses_the_identifier_not_a_swapped_port() {
        let mut e = zero_entry();
        e.src_addr = 1;
        e.dst_addr = 2;
        e.src_port = ICMP_ECHO_REQUEST;
        e.dst_port = 7;
        e.proto = PROTO_ICMP;
        assert_eq!(ct_icmp_is(&e, 1, 2, 7), Some(false));
        assert_eq!(ct_icmp_is(&e, 2, 1, 7), Some(true));
        assert_eq!(ct_icmp_is(&e, 2, 1, 8), None);
        assert_eq!(ct_icmp_is(&e, 1, 2, 8), None);
    }

    #[test]
    fn nat_rule_parser_accepts_the_documented_forms() {
        let mut r = zero_nat_rule();
        let line = b"out proto=tcp dst=10.0.2.15 action=masquerade";
        assert_eq!(
            unsafe { a20_netfilter_nat_parse(line.as_ptr(), line.len(), &mut r) },
            0
        );
        assert_eq!(
            (r.dir, r.proto, r.nat),
            (DIR_OUT, PROTO_TCP, NAT_MASQUERADE)
        );

        let line = b"in proto=tcp dport=18081 action=dnat to=10.0.2.15 toport=18082\n";
        assert_eq!(
            unsafe { a20_netfilter_nat_parse(line.as_ptr(), line.len(), &mut r) },
            0
        );
        assert_eq!((r.dir, r.nat, r.dst_port), (DIR_IN, NAT_DNAT, 18081));
        assert_eq!((r.nat_port, r.nat_addr), (18082, 0x0a00020f));

        /* snat without `to=` is refused at parse time. */
        let line = b"out proto=udp action=snat";
        assert_eq!(
            unsafe { a20_netfilter_nat_parse(line.as_ptr(), line.len(), &mut r) },
            -EINVAL
        );
        /* dnat is input-only. */
        let line = b"out proto=tcp action=dnat to=1.2.3.4";
        assert_eq!(
            unsafe { a20_netfilter_nat_parse(line.as_ptr(), line.len(), &mut r) },
            -EINVAL
        );
        /* masquerade with an address or a port is ambiguous. */
        let line = b"out proto=tcp action=masquerade to=1.2.3.4";
        assert_eq!(
            unsafe { a20_netfilter_nat_parse(line.as_ptr(), line.len(), &mut r) },
            -EINVAL
        );
        let line = b"out proto=tcp action=masquerade toport=9";
        assert_eq!(
            unsafe { a20_netfilter_nat_parse(line.as_ptr(), line.len(), &mut r) },
            -EINVAL
        );
    }

    #[test]
    fn port_parser_refuses_overflow_and_junk() {
        assert_eq!(parse_u32(b"65535", 65535), Ok(65535));
        assert_eq!(parse_u32(b"65536", 65535), Err(EINVAL));
        assert_eq!(parse_u32(b"12x", 65535), Err(EINVAL));
        assert_eq!(parse_u32(b"", 65535), Ok(0));
    }
}
