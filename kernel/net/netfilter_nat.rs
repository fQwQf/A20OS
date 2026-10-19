#![no_std]

//! Conntrack table and NAT rule state machine for the IPv4 data plane.
//!
//! The C side keeps the storage (the two static arrays and their counters), the
//! locks and the real I/O: `g_lwip_lock` around every entry point, the NAT
//! table's seqlock, the timer read, the in-place frame rewrite and checksum
//! fixup, and the netif lookup MASQUERADE needs.  Everything that is a
//! *decision* over that state is here:
//!
//!   * the bucket hash, the forward and reply chains, the ICMP-echo pairing and
//!     the two-probe lookup, including the reply-direction chain that is the
//!     only place a translated reply can be found;
//!   * slot allocation, the LRU victim at capacity, insertion, unlinking and the
//!     idle-timeout sweep with its resume point;
//!   * the lifetime and per-direction counters, the snapshot/count/inject
//!     accessors and the enable flag;
//!   * the NAT rule table's add/delete/reset transactions and the rule parser;
//!   * the packet-path decision: tracking eligibility, tuple normalisation for
//!     ICMP echo, entry lookup versus creation, the state transition, and which
//!     rewrite a later packet of a translated flow gets.
//!
//! The structs are mirrored field for field from `net/netfilter.h`, and
//! `netfilter_nat.c` static-asserts every offset and size, so a layout change
//! there is a build error rather than a silent corruption here.
//!
//! Pre-existing behaviour preserved verbatim: `net_nat_rule_t.matched` is never
//! incremented (the renderer reads it, the picker does not bump it), and a
//! translated flow's later packets are rewritten from the entry rather than from
//! the rule table, including same-direction retransmissions.

use core::ptr;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

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
    /// Three idle limits in ticks, lazily derived from `to_ms`.
    pub to_ticks: *mut u32,
    pub time_ready: *mut i32,
    pub sweeps: *mut u32,
    /// TICKS_PER_SEC as the target defines it; a runtime value on riscv64.
    pub ticks_per_sec: u64,
}

unsafe extern "C" {
    fn a20_netfilter_now_ticks() -> u32;
    fn a20_netfilter_nat_seq_read_begin() -> u32;
    fn a20_netfilter_nat_seq_read_retry(start: u32) -> i32;
    fn a20_netfilter_nat_seq_write_begin();
    fn a20_netfilter_nat_seq_write_end();
    fn a20_netfilter_rewrite_set_addr(frame: *mut u8, len: usize,
                                      pkt: *const Frame, new_src: u32,
                                      new_dst: u32);
    fn a20_netfilter_rewrite_set_port(frame: *mut u8, len: usize,
                                      pkt: *const Frame, is_src: i32,
                                      new_port: u16);
    fn a20_netfilter_masq_addr(net_idx: i32) -> u32;
    fn a20_netfilter_ipv4_from_str(s: *const u8, len: usize, out: *mut u32) -> i32;
}

/* ------------------------------------------------------------- view access */

struct View(&'static CtView);

impl View {
    unsafe fn entries(&self) -> &'static mut [ConntrackEntry] {
        unsafe { core::slice::from_raw_parts_mut(self.0.entries, self.0.max as usize) }
    }
    unsafe fn heads(&self) -> &'static mut [u16] {
        unsafe { core::slice::from_raw_parts_mut(self.0.head, self.0.buckets as usize) }
    }
    unsafe fn rheads(&self) -> &'static mut [u16] {
        unsafe { core::slice::from_raw_parts_mut(self.0.rhead, self.0.buckets as usize) }
    }
    unsafe fn nat_rules(&self) -> &'static mut [NatRule] {
        unsafe { core::slice::from_raw_parts_mut(self.0.nat, self.0.nat_max as usize) }
    }
}

unsafe fn view(v: *const CtView) -> View {
    View(unsafe { &*v })
}

unsafe fn used_slot(e: &ConntrackEntry) -> bool {
    e.proto != 0
}

/// FNV-1a over the tuple's wire bytes, folded to a bucket. The order is fixed
/// (src first) and deliberately not symmetric: a tuple and its reverse land in
/// different buckets, and the lookup compensates by probing both.
fn ct_hash(src: u32, dst: u32, sport: u16, dport: u16, proto: u8,
            buckets: u32) -> usize {
    let words: [u8; 13] = [
        (src >> 24) as u8, (src >> 16) as u8, (src >> 8) as u8, src as u8,
        (dst >> 24) as u8, (dst >> 16) as u8, (dst >> 8) as u8, dst as u8,
        (sport >> 8) as u8, sport as u8,
        (dport >> 8) as u8, dport as u8,
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
    (rsrc, rdst, rsport, rdport)
}

unsafe fn ct_find(v: &View, src: u32, dst: u32, sport: u16, dport: u16,
                  proto: u8) -> Option<(usize, bool)> {
    let buckets = v.0.buckets;
    let b0 = ct_hash(src, dst, sport, dport, proto, buckets);
    let b1 = ct_hash(dst, src, dport, sport, proto, buckets);
    let entries = unsafe { v.entries() };
    let heads = unsafe { v.heads() };
    for pass in 0..2 {
        let b = if pass == 0 { b0 } else { b1 };
        let mut idx = heads[b];
        while idx != NONE {
            let e = &entries[idx as usize];
            let next = e.hash_next;
            if e.proto == proto {
                if proto == PROTO_ICMP {
                    if let Some(rev) = ct_icmp_is(e, src, dst, dport) {
                        return Some((idx as usize, rev));
                    }
                } else if e.src_addr == src && e.dst_addr == dst
                    && e.src_port == sport && e.dst_port == dport
                {
                    return Some((idx as usize, false));
                } else if e.src_addr == dst && e.dst_addr == src
                    && e.src_port == dport && e.dst_port == sport
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
    let rheads = unsafe { v.rheads() };
    let mut idx = rheads[b0];
    while idx != NONE {
        let e = &entries[idx as usize];
        let next = e.rhash_next;
        if e.proto == proto {
            let (rsrc, rdst, rsport, rdport) = ct_reply_tuple(e);
            if proto == PROTO_ICMP {
                if rsrc == src && rdst == dst && e.dst_port == dport {
                    return Some((idx as usize, true));
                }
            } else if rsrc == src && rdst == dst && rsport == sport
                && rdport == dport
            {
                return Some((idx as usize, true));
            }
        }
        idx = next;
    }
    None
}

unsafe fn ct_unlink_from(v: &View, head: &mut [u16], b: usize, idx: usize,
                         reply: bool) {
    let entries = unsafe { v.entries() };
    let mut prev: u16 = NONE;
    let mut i = head[b];
    while i != NONE {
        let next = if reply {
            entries[i as usize].rhash_next
        } else {
            entries[i as usize].hash_next
        };
        if i as usize != idx {
            prev = i;
            i = next;
            continue;
        }
        if prev == NONE {
            head[b] = next;
        } else if reply {
            entries[prev as usize].rhash_next = next;
        } else {
            entries[prev as usize].hash_next = next;
        }
        if reply {
            entries[i as usize].rhash_next = NONE;
        } else {
            entries[i as usize].hash_next = NONE;
        }
        return;
    }
}

unsafe fn ct_unlink(v: &View, idx: usize) {
    let entries = unsafe { v.entries() };
    let e = entries[idx];
    let bf = ct_hash(e.src_addr, e.dst_addr, e.src_port, e.dst_port, e.proto,
                     v.0.buckets);
    unsafe { ct_unlink_from(v, v.heads(), bf, idx, false) };
    let (rsrc, rdst, rsport, rdport) = ct_reply_tuple(&e);
    let br = ct_hash(rsrc, rdst, rsport, rdport, e.proto, v.0.buckets);
    unsafe { ct_unlink_from(v, v.rheads(), br, idx, true) };
}

/// Least recently used live entry; ties broken by lower index.
unsafe fn ct_lru(v: &View) -> Option<usize> {
    let entries = unsafe { v.entries() };
    let mut best: Option<usize> = None;
    let mut best_ms = 0u32;
    for i in 0..entries.len() {
        if !unsafe { used_slot(&entries[i]) } {
            continue;
        }
        let ms = entries[i].last_ms;
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

unsafe fn ct_now(v: &View) -> u32 {
    if unsafe { *v.0.time_ready } == 0 {
        unsafe { ct_time_init(v) };
    }
    unsafe { a20_netfilter_now_ticks() }
}

unsafe fn ct_time_init(v: &View) {
    let mut hz = v.0.ticks_per_sec;
    if hz == 0 {
        hz = 1000;
    }
    for i in 0..3 {
        let default_ms = match i {
            TO_INDEX_TCP_NEW => DEFAULT_TCP_NEW_MS,
            TO_INDEX_TCP_ESTABLISHED => DEFAULT_TCP_ESTABLISHED_MS,
            _ => DEFAULT_UDP_MS,
        };
        let ms = unsafe { *v.0.to_ms.add(i) };
        let ms = if ms != 0 { ms } else { default_ms };
        let mut ticks = (ms as u64).wrapping_mul(hz) as u32 / 1000;
        /* A zero limit would mean "expires instantly" or "never expires"
         * depending on the comparison; force one tick so neither is reachable. */
        if ticks == 0 {
            ticks = 1;
        }
        unsafe { *v.0.to_ticks.add(i) = ticks };
    }
    unsafe { *v.0.time_ready = 1 };
}

unsafe fn ct_timeout_for(v: &View, e: &ConntrackEntry) -> u32 {
    let idx = if e.proto == PROTO_UDP || e.proto == PROTO_ICMP {
        TO_INDEX_UDP
    } else if e.state == CT_ESTABLISHED {
        TO_INDEX_TCP_ESTABLISHED
    } else {
        TO_INDEX_TCP_NEW
    };
    unsafe { *v.0.to_ticks.add(idx) }
}

unsafe fn ct_insert(v: &View, entry: &mut ConntrackEntry) -> i32 {
    entry.last_ms = unsafe { ct_now(v) };
    entry.hash_next = NONE;

    let entries = unsafe { v.entries() };
    let max = entries.len();
    let used = unsafe { *v.0.used };
    let was_full = used as usize >= max;
    let slot;
    if !was_full {
        let mut s = max;
        for i in 0..max {
            if !unsafe { used_slot(&entries[i]) } {
                s = i;
                break;
            }
        }
        if s == max {
            return -ENOSPC; /* the counter and the slots disagreed */
        }
        slot = s;
    } else {
        /* At capacity the table degrades into a bounded LRU cache rather than a
         * failure: a busy router keeps its newest flows and forgets its
         * oldest. */
        match unsafe { ct_lru(v) } {
            Some(s) => {
                unsafe { ct_unlink(v, s) };
                let lifetime = unsafe { &mut *v.0.lifetime };
                lifetime.evicted = lifetime.evicted.wrapping_add(1);
                slot = s;
            }
            None => return -ENOSPC,
        }
    }

    let b = ct_hash(entry.src_addr, entry.dst_addr, entry.src_port,
                    entry.dst_port, entry.proto, v.0.buckets);
    let (rsrc, rdst, rsport, rdport) = ct_reply_tuple(entry);
    let rb = ct_hash(rsrc, rdst, rsport, rdport, entry.proto, v.0.buckets);
    entries[slot] = *entry;
    entries[slot].hash_next = v.heads()[b];
    v.heads()[b] = slot as u16;
    entries[slot].rhash_next = v.rheads()[rb];
    v.rheads()[rb] = slot as u16;
    if !was_full {
        unsafe { *v.0.used = used + 1 };
    }
    let lifetime = unsafe { &mut *v.0.lifetime };
    lifetime.created = lifetime.created.wrapping_add(1);
    lifetime.tracked = unsafe { *v.0.used as u64 };
    0
}

/* ------------------------------------------------------------ entry points */

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_init(v: *const CtView) {
    if v.is_null() {
        return;
    }
    let v = unsafe { view(v) };
    let heads = unsafe { v.heads() };
    for h in heads.iter_mut() {
        *h = NONE;
    }
    let rheads = unsafe { v.rheads() };
    for h in rheads.iter_mut() {
        *h = NONE;
    }
    let entries = unsafe { v.entries() };
    for e in entries.iter_mut() {
        *e = zero_entry();
    }
    unsafe {
        *v.0.used = 0;
        *v.0.sweep = 0;
        *v.0.enabled = 1;
    }
}

fn zero_entry() -> ConntrackEntry {
    ConntrackEntry {
        src_addr: 0, dst_addr: 0, src_port: 0, dst_port: 0,
        proto: 0, state: 0, nat: 0, dir: 0,
        nat_src_addr: 0, nat_dst_addr: 0, nat_src_port: 0, nat_dst_port: 0,
        last_ms: 0, packets: 0, bytes: 0, hash_next: 0, rhash_next: 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_count(v: *const CtView) -> i32 {
    if v.is_null() {
        return 0;
    }
    unsafe { *(*v).used as i32 }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_sweeps(v: *const CtView) -> u32 {
    if v.is_null() {
        return 0;
    }
    // SAFETY: the view points at the live counter.
    unsafe { AtomicU32::from_ptr((*v).sweeps).load(Ordering::Relaxed) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_lru_victim(v: *const CtView) -> i32 {
    if v.is_null() {
        return -1;
    }
    let v = unsafe { view(v) };
    match unsafe { ct_lru(&v) } {
        Some(slot) => unsafe { v.entries()[slot].src_port as i32 },
        None => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_inject(v: *const CtView, src: u32,
                                                 dst: u32, sport: u16,
                                                 dport: u16, proto: u8,
                                                 state: u8) -> i32 {
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
pub unsafe extern "C" fn a20_netfilter_ct_snapshot(v: *const CtView,
                                                   out: *mut ConntrackEntry,
                                                   max: u32) -> i32 {
    if v.is_null() || out.is_null() || max == 0 {
        return 0;
    }
    let v = unsafe { view(v) };
    let entries = unsafe { v.entries() };
    let mut n = 0usize;
    for i in 0..entries.len() {
        if n >= max as usize {
            break;
        }
        if unsafe { used_slot(&entries[i]) } {
            unsafe { *out.add(n) = entries[i] };
            n += 1;
        }
    }
    n as i32
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_flush(v: *const CtView) {
    if v.is_null() {
        return;
    }
    let v = unsafe { view(v) };
    let heads = unsafe { v.heads() };
    for h in heads.iter_mut() {
        *h = NONE;
    }
    let rheads = unsafe { v.rheads() };
    for h in rheads.iter_mut() {
        *h = NONE;
    }
    let entries = unsafe { v.entries() };
    for e in entries.iter_mut() {
        *e = zero_entry();
    }
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
pub unsafe extern "C" fn a20_netfilter_ct_get_stats(v: *const CtView,
                                                    out: *mut ConntrackStats) {
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
        stats.packets += unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*s).packets))
            .load(Ordering::Relaxed) };
        stats.bytes += unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*s).bytes))
            .load(Ordering::Relaxed) };
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
    if v.is_null() {
        return;
    }
    /* Entries are kept when tracking is switched off: deleting them would
     * silently break every flow relying on a NAT binding. */
    unsafe { *(*v).enabled = if on != 0 { 1 } else { 0 } };
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_enabled(v: *const CtView) -> i32 {
    if v.is_null() {
        return 0;
    }
    unsafe { *(*v).enabled }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_set_timeouts(v: *const CtView,
                                                       a: u32, b: u32,
                                                       c: u32) {
    if v.is_null() {
        return;
    }
    let view = unsafe { &*v };
    unsafe {
        *view.to_ms.add(0) = a;
        *view.to_ms.add(1) = b;
        *view.to_ms.add(2) = c;
        /* Force the conversion to be redone on the next read rather than
         * duplicating it here. */
        *view.time_ready = 0;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_expire(v: *const CtView,
                                                 max_scan: u32) -> u32 {
    if v.is_null() {
        return 0;
    }
    let v = unsafe { view(v) };
    let entries = unsafe { v.entries() };
    let max = entries.len();
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
        if unsafe { used_slot(&entries[i]) } {
            let limit = unsafe { ct_timeout_for(&v, &entries[i]) };
            /* Unsigned subtract: correct across the tick counter's wrap. */
            if now.wrapping_sub(entries[i].last_ms) < limit {
                step += 1;
                continue;
            }
            unsafe { ct_unlink(&v, i) };
            entries[i] = zero_entry();
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

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_ct_process(v: *const CtView,
                                                  frame: *mut u8, len: usize,
                                                  net_idx: i32, dir: u8,
                                                  pkt: *const Frame) {
    if v.is_null() || frame.is_null() || pkt.is_null() || len < 14 {
        return;
    }
    let v = unsafe { view(v) };
    let pkt = unsafe { &*pkt };

    /* TCP and UDP are tracked, and so -- minimally -- is ICMP echo. */
    let icmp_echo = pkt.proto == PROTO_ICMP && pkt.icmp_off != 0;
    if pkt.proto != PROTO_TCP && pkt.proto != PROTO_UDP && !icmp_echo {
        return;
    }

    /* For ICMP echo the two 16-bit tuple fields carry the type (normalised to
     * the request value) and the identifier. */
    let (t_sport, t_dport) = if icmp_echo {
        (ICMP_ECHO_REQUEST, pkt.icmp_id)
    } else {
        (pkt.src_port, pkt.dst_port)
    };

    /* A non-first fragment carries no ports: no tuple to key on and no header
     * to rewrite against, so it passes untranslated. */
    if !icmp_echo && (pkt.has_ports == 0 || pkt.l4_off == 0) {
        return;
    }

    let st = v.0.dir_stats[if dir == DIR_IN { 0 } else { 1 }];
    if !st.is_null() {
        unsafe {
            AtomicU64::from_ptr(ptr::addr_of_mut!((*st).packets))
                .fetch_add(1, Ordering::Relaxed);
            AtomicU64::from_ptr(ptr::addr_of_mut!((*st).bytes))
                .fetch_add(len as u64, Ordering::Relaxed);
        }
    }

    if let Some((idx, is_reverse)) =
        unsafe { ct_find(&v, pkt.src_addr, pkt.dst_addr, t_sport, t_dport, pkt.proto) }
    {
        let entries = unsafe { v.entries() };
        let e = &mut entries[idx];
        e.last_ms = unsafe { ct_now(&v) };
        e.packets = e.packets.wrapping_add(1);
        unsafe {
            AtomicU64::from_ptr(ptr::addr_of_mut!(e.bytes))
                .fetch_add(len as u64, Ordering::Relaxed);
        }
        if is_reverse {
            e.state = CT_ESTABLISHED;
        } else if pkt.proto == PROTO_TCP && (pkt.tcp_flags & 0x10) != 0
            && (pkt.tcp_flags & 0x02) == 0
        {
            e.state = CT_ESTABLISHED;
        }

        /* Every later packet of the flow is translated from the entry, not from
         * the rules: a rule may have been deleted or reordered since, and a
         * same-direction retransmission must be translated too. */
        let (nat, e_src, e_dst, e_sport, e_dport, nat_src, nat_sport, nat_dst,
             nat_dport) =
            (e.nat, e.src_addr, e.dst_addr, e.src_port, e.dst_port,
             e.nat_src_addr, e.nat_src_port, e.nat_dst_addr, e.nat_dst_port);
        if nat == NAT_SNAT || nat == NAT_MASQUERADE {
            if is_reverse {
                unsafe {
                    a20_netfilter_rewrite_set_addr(frame, len, pkt, pkt.src_addr, e_src);
                    a20_netfilter_rewrite_set_port(frame, len, pkt, 0, e_sport);
                }
            } else {
                unsafe {
                    a20_netfilter_rewrite_set_addr(frame, len, pkt, nat_src, pkt.dst_addr);
                    a20_netfilter_rewrite_set_port(frame, len, pkt, 1, nat_sport);
                }
            }
        } else if nat == NAT_DNAT {
            if is_reverse {
                unsafe {
                    a20_netfilter_rewrite_set_addr(frame, len, pkt, e_dst, pkt.dst_addr);
                    a20_netfilter_rewrite_set_port(frame, len, pkt, 1, e_dport);
                }
            } else {
                unsafe {
                    a20_netfilter_rewrite_set_addr(frame, len, pkt, pkt.src_addr, nat_dst);
                    a20_netfilter_rewrite_set_port(frame, len, pkt, 0, nat_dport);
                }
            }
        }
        return;
    }

    if unsafe { *v.0.enabled } == 0 {
        return;
    }

    let mut e = zero_entry();
    e.src_addr = pkt.src_addr;
    e.dst_addr = pkt.dst_addr;
    e.src_port = t_sport;
    e.dst_port = t_dport;
    e.proto = pkt.proto;
    e.dir = dir;
    e.state = CT_NEW;
    e.nat = NAT_NONE;
    e.packets = 1;
    e.bytes = len as u64;

    if let Some(rule) = unsafe { nat_pick(&v, pkt, dir) } {
        if rule.nat == NAT_DNAT && dir == DIR_IN {
            e.nat = NAT_DNAT;
            e.nat_dst_addr = rule.nat_addr;
            e.nat_dst_port = rule.nat_port;
            unsafe {
                a20_netfilter_rewrite_set_addr(frame, len, pkt, pkt.src_addr,
                                               rule.nat_addr);
                a20_netfilter_rewrite_set_port(frame, len, pkt, 0, rule.nat_port);
            }
        } else if (rule.nat == NAT_SNAT || rule.nat == NAT_MASQUERADE)
            && dir == DIR_OUT
        {
            let target = if rule.nat == NAT_MASQUERADE {
                unsafe { a20_netfilter_masq_addr(net_idx) }
            } else {
                rule.nat_addr
            };
            if target != 0 {
                e.nat = rule.nat;
                e.nat_src_addr = target;
                e.nat_src_port = rule.nat_port;
                unsafe {
                    a20_netfilter_rewrite_set_addr(frame, len, pkt, target,
                                                   pkt.dst_addr);
                    a20_netfilter_rewrite_set_port(frame, len, pkt, 1,
                                                   rule.nat_port);
                }
            }
        }
    }

    /* Tracking is best-effort: if the insert fails the packet has already been
     * translated (or not) and must not be dropped because the bookkeeping had
     * nowhere to go. */
    let _ = unsafe { ct_insert(&v, &mut e) };
}

/* ------------------------------------------------------------- NAT rules */

unsafe fn nat_pick(v: &View, pkt: &Frame, dir: u8) -> Option<NatRule> {
    let mut found: Option<NatRule> = None;
    for _attempt in 0..4 {
        let seq0 = unsafe { a20_netfilter_nat_seq_read_begin() };
        if seq0 == 0 {
            continue;
        }
        let rules = unsafe { v.nat_rules() };
        let n = unsafe { *v.0.nat_count } as usize;
        let mut hit: Option<NatRule> = None;
        if n <= rules.len() {
            for i in 0..n {
                let r = &rules[i];
                if r.dir != dir {
                    continue;
                }
                if r.proto != PROTO_ANY && r.proto != pkt.proto {
                    continue;
                }
                if r.src_addr != NO_ADDR && r.src_addr != pkt.src_addr {
                    continue;
                }
                if r.dst_addr != NO_ADDR && r.dst_addr != pkt.dst_addr {
                    continue;
                }
                if r.src_port != NO_PORT && r.src_port != pkt.src_port {
                    continue;
                }
                if r.dst_port != NO_PORT && r.dst_port != pkt.dst_port {
                    continue;
                }
                hit = Some(*r);
                break;
            }
        }
        if unsafe { a20_netfilter_nat_seq_read_retry(seq0) } != 0 {
            continue;
        }
        found = hit;
        break;
    }
    found
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
pub unsafe extern "C" fn a20_netfilter_nat_parse(line: *const u8, len: usize,
                                                 out: *mut NatRule) -> i32 {
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
        dir: 0, proto: PROTO_ANY, nat: NAT_NONE, src_port: NO_PORT,
        dst_port: NO_PORT, src_addr: NO_ADDR, dst_addr: NO_ADDR,
        nat_addr: NO_ADDR, nat_port: 0, matched: 0, bytes: 0,
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
        while vstart + vlen < len && line[vstart + vlen] != b' '
            && line[vstart + vlen] != b'\t'
        {
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
            } else if unsafe { ipv4_from_str(val.as_ptr(), val.len(), &mut r.src_addr) } != 1
            {
                return -EINVAL;
            }
        } else if key == b"dst" {
            if val == b"any" {
                r.dst_addr = NO_ADDR;
            } else if unsafe { ipv4_from_str(val.as_ptr(), val.len(), &mut r.dst_addr) } != 1
            {
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
            if unsafe { ipv4_from_str(val.as_ptr(), val.len(), &mut r.nat_addr) } != 1
            {
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

/// Validate and append one rule. The caller holds g_netfilter_lock; the seqlock
/// bracket is taken here so the table transition and its publication are one
/// transaction.
#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_nat_add(v: *const CtView,
                                               rule: *const NatRule) -> i32 {
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
    /* DNAT is an input-hook translation. */
    if r.nat == NAT_DNAT && r.dir != DIR_IN {
        return -EINVAL;
    }

    let v = unsafe { view(v) };
    let count = unsafe { *v.0.nat_count } as usize;
    if count >= v.0.nat_max as usize {
        return -ENOSPC;
    }
    unsafe {
        a20_netfilter_nat_seq_write_begin();
        let rules = v.nat_rules();
        rules[count] = *r;
        *v.0.nat_count = (count + 1) as u32;
        a20_netfilter_nat_seq_write_end();
    }
    count as i32
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_nat_del(v: *const CtView,
                                               index: u32) -> i32 {
    if v.is_null() {
        return -EINVAL;
    }
    let v = unsafe { view(v) };
    let count = unsafe { *v.0.nat_count } as usize;
    if index as usize >= count {
        return -EINVAL;
    }
    unsafe {
        a20_netfilter_nat_seq_write_begin();
        let rules = v.nat_rules();
        let mut i = index as usize;
        while i + 1 < count {
            rules[i] = rules[i + 1];
            i += 1;
        }
        let new_count = count - 1;
        *v.0.nat_count = new_count as u32;
        rules[new_count] = zero_nat_rule();
        a20_netfilter_nat_seq_write_end();
    }
    0
}

fn zero_nat_rule() -> NatRule {
    NatRule {
        dir: 0, proto: 0, nat: 0, src_port: 0, dst_port: 0,
        src_addr: 0, dst_addr: 0, nat_addr: 0, nat_port: 0,
        matched: 0, bytes: 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_nat_reset(v: *const CtView) {
    if v.is_null() {
        return;
    }
    let v = unsafe { view(v) };
    unsafe {
        a20_netfilter_nat_seq_write_begin();
        let rules = v.nat_rules();
        for r in rules.iter_mut() {
            *r = zero_nat_rule();
        }
        *v.0.nat_count = 0;
        a20_netfilter_nat_seq_write_end();
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_nat_count(v: *const CtView) -> i32 {
    if v.is_null() {
        return 0;
    }
    unsafe { AtomicU32::from_ptr((*v).nat_count).load(Ordering::Acquire) as i32 }
}

/// The rule a new flow matches, or None. Reads under the seqlock, retrying when
/// a writer ran.
#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_nat_pick(v: *const CtView,
                                                pkt: *const Frame, dir: u8,
                                                out: *mut NatRule) -> i32 {
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
        assert_eq!(unsafe { a20_netfilter_nat_parse(line.as_ptr(), line.len(), &mut r) }, 0);
        assert_eq!((r.dir, r.proto, r.nat), (DIR_OUT, PROTO_TCP, NAT_MASQUERADE));

        let line = b"in proto=tcp dport=18081 action=dnat to=10.0.2.15 toport=18082\n";
        assert_eq!(unsafe { a20_netfilter_nat_parse(line.as_ptr(), line.len(), &mut r) }, 0);
        assert_eq!((r.dir, r.nat, r.dst_port), (DIR_IN, NAT_DNAT, 18081));
        assert_eq!((r.nat_port, r.nat_addr), (18082, 0x0a00020f));

        /* snat without `to=` is refused at parse time. */
        let line = b"out proto=udp action=snat";
        assert_eq!(unsafe { a20_netfilter_nat_parse(line.as_ptr(), line.len(), &mut r) },
                   -EINVAL);
        /* dnat is input-only. */
        let line = b"out proto=tcp action=dnat to=1.2.3.4";
        assert_eq!(unsafe { a20_netfilter_nat_parse(line.as_ptr(), line.len(), &mut r) },
                   -EINVAL);
        /* masquerade with an address or a port is ambiguous. */
        let line = b"out proto=tcp action=masquerade to=1.2.3.4";
        assert_eq!(unsafe { a20_netfilter_nat_parse(line.as_ptr(), line.len(), &mut r) },
                   -EINVAL);
        let line = b"out proto=tcp action=masquerade toport=9";
        assert_eq!(unsafe { a20_netfilter_nat_parse(line.as_ptr(), line.len(), &mut r) },
                   -EINVAL);
    }

    #[test]
    fn port_parser_refuses_overflow_and_junk() {
        assert_eq!(parse_u32(b"65535", 65535), Ok(65535));
        assert_eq!(parse_u32(b"65536", 65535), Err(EINVAL));
        assert_eq!(parse_u32(b"12x", 65535), Err(EINVAL));
        assert_eq!(parse_u32(b"", 65535), Ok(0));
    }
}
