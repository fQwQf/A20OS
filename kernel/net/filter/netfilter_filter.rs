#![no_std]

//! IPv4 packet filter: rule table transactions, the rule parser, the frame
//! walk, the matching engine and the hook ordering.
//!
//! The C side keeps the storage (`g_rules`, `g_rule_count`, the two stats
//! banks), the writer lock and the rule-table seqlock, the dotted-quad
//! conversions and the `/proc` renderer.  Rust owns every decision on top of
//! that:
//!
//!   * add/delete/reset/get/count over the table, each a transaction with its
//!     seqlock bracket so a lock-free packet path never observes a half-published
//!     table;
//!   * the `add` line parser, including the keys it refuses;
//!   * the Ethernet/VLAN/IPv4/L4 walk, whose offsets the NAT rewriter reuses so
//!     the matcher and the rewriter cannot disagree about where the L4 header
//!     starts;
//!   * the matching engine, first-match-wins, with the matched rule's counters
//!     bumped *inside* the seqlock bracket so a retry understates a counter
//!     rather than charging the packet to whichever rule slid into the index;
//!   * the hook ordering: filter verdict first, conntrack and NAT only for a
//!     packet that was accepted, because a dropped packet must not create flow
//!     state or consume a NAT binding.
//!
//! Callers hold `g_lwip_lock` for the hooks and `g_netfilter_lock` for the
//! writers, as before; none of these entry points takes a lock of its own and
//! none allocates.

use core::ptr;
use core::sync::atomic::{AtomicU32, Ordering};
#[cfg(target_has_atomic = "64")]
use core::sync::atomic::AtomicU64;

const NO_ADDR: u32 = 0xFFFF_FFFF;
const NO_PORT: u16 = 0xFFFF;

const PROTO_ANY: u8 = 0;
const PROTO_ICMP: u8 = 1;
const PROTO_TCP: u8 = 6;
const PROTO_UDP: u8 = 17;

const ICMP_ECHO_REQUEST: u8 = 8;
const ICMP_ECHO_REPLY: u8 = 0;

const DIR_IN: u8 = 0;
const DIR_OUT: u8 = 1;

const ACCEPT: i32 = 0;
const DROP: i32 = 1;

const EINVAL: i32 = 22;
const ENOSPC: i32 = 28;

const SEQLOCK_READ_ATTEMPTS: u32 = 4;

/// Mirror of `netfilter_rule_t`; every offset is pinned in netfilter.c.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FilterRule {
    pub dir: u8,
    pub proto: u8,
    pub action: u8,
    pub src_port: u16,
    pub dst_port: u16,
    pub src_addr: u32,
    pub dst_addr: u32,
    pub matched: u64,
    pub bytes: u64,
}

/// Mirror of `netfilter_frame_t`; pinned in netfilter.c.
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

/// Mirror of `netfilter_stats_t`; pinned in netfilter.c.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FilterStats {
    pub in_packets: u64,
    pub out_packets: u64,
    pub in_dropped: u64,
    pub out_dropped: u64,
    pub in_accepted: u64,
    pub out_accepted: u64,
}

#[repr(C)]
pub struct FilterView {
    pub rules: *mut FilterRule,
    pub max: u32,
    pub count: *mut u32,
    pub stats_in: *mut FilterStats,
    pub stats_out: *mut FilterStats,
}

unsafe extern "C" {
    fn a20_netfilter_rule_seq_read_begin() -> u32;
    fn a20_netfilter_rule_seq_read_retry(start: u32) -> i32;
    fn a20_netfilter_rule_seq_write_begin();
    fn a20_netfilter_rule_seq_write_end();
    fn a20_netfilter_ipv4_from_str(s: *const u8, len: usize, out: *mut u32) -> i32;
    /// The conntrack/NAT half's entry point; the filter decides *when* it runs.
    fn a20_netfilter_ct_process_tail(frame: *mut u8, len: usize, net_idx: i32,
                                     dir: u8, pkt: *const Frame) -> i32;
}

struct View(&'static FilterView);

unsafe fn view(v: *const FilterView) -> View {
    View(unsafe { &*v })
}

impl View {
    unsafe fn rules(&self) -> &'static mut [FilterRule] {
        unsafe { core::slice::from_raw_parts_mut(self.0.rules, self.0.max as usize) }
    }
    fn count(&self) -> u32 {
        unsafe { AtomicU32::from_ptr(self.0.count).load(Ordering::Relaxed) }
    }
}

fn zero_rule() -> FilterRule {
    FilterRule {
        dir: 0, proto: 0, action: 0, src_port: 0, dst_port: 0,
        src_addr: 0, dst_addr: 0, matched: 0, bytes: 0,
    }
}

fn bump(counter: *mut u64, by: u64) {
    #[cfg(target_has_atomic = "64")]
    unsafe {
        AtomicU64::from_ptr(counter).fetch_add(by, Ordering::Relaxed);
    }
    /* The hook path holds g_lwip_lock, so on profiles without 64-bit atomics a
     * plain read-modify-write has the same publication discipline. */
    #[cfg(not(target_has_atomic = "64"))]
    unsafe {
        *counter = (*counter).wrapping_add(by);
    }
}

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

fn stat_field(stats: *mut FilterStats, which: u32) -> *mut u64 {
    if stats.is_null() {
        return ptr::null_mut();
    }
    unsafe {
        match which {
            0 => ptr::addr_of_mut!((*stats).in_packets),
            1 => ptr::addr_of_mut!((*stats).out_packets),
            2 => ptr::addr_of_mut!((*stats).in_dropped),
            3 => ptr::addr_of_mut!((*stats).out_dropped),
            4 => ptr::addr_of_mut!((*stats).in_accepted),
            _ => ptr::addr_of_mut!((*stats).out_accepted),
        }
    }
}

/* ------------------------------------------------------------ rule parser */

fn parse_port(bytes: &[u8]) -> Result<u16, i32> {
    let mut p: u32 = 0;
    for &c in bytes {
        if !(b'0'..=b'9').contains(&c) {
            return Err(EINVAL);
        }
        p = p.wrapping_mul(10).wrapping_add((c - b'0') as u32);
        if p > NO_PORT as u32 {
            return Err(EINVAL);
        }
    }
    Ok(p as u16)
}

/// Parse one "add" line into a filter rule.
#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_parse_rule(line: *const u8, len: usize,
                                                  out: *mut FilterRule) -> i32 {
    if line.is_null() || out.is_null() {
        return -EINVAL;
    }
    let mut len = len;
    let bytes = unsafe { core::slice::from_raw_parts(line, len) };
    while len != 0 && (bytes[len - 1] == b'\n' || bytes[len - 1] == b'\r') {
        len -= 1;
    }
    let line = &bytes[..len];
    let mut r = FilterRule {
        dir: 0, proto: PROTO_ANY, action: ACCEPT as u8, src_port: NO_PORT,
        dst_port: NO_PORT, src_addr: NO_ADDR, dst_addr: NO_ADDR,
        matched: 0, bytes: 0,
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
            break; /* trailing garbage, as before */
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
                let mut p: u32 = 0;
                for &c in val {
                    if !c.is_ascii_digit() {
                        return -EINVAL;
                    }
                    p = p.wrapping_mul(10).wrapping_add((c - b'0') as u32);
                }
                if p > 255 {
                    return -EINVAL;
                }
                p as u8
            } else {
                return -EINVAL;
            };
        } else if key == b"src" {
            if val == b"any" {
                r.src_addr = NO_ADDR;
            } else {
                let mut a = 0u32;
                if unsafe { ipv4_from_str(val.as_ptr(), val.len(), &mut a) } != 1
                {
                    return -EINVAL;
                }
                r.src_addr = a;
            }
        } else if key == b"dst" {
            if val == b"any" {
                r.dst_addr = NO_ADDR;
            } else {
                let mut a = 0u32;
                if unsafe { ipv4_from_str(val.as_ptr(), val.len(), &mut a) } != 1
                {
                    return -EINVAL;
                }
                r.dst_addr = a;
            }
        } else if key == b"sport" {
            if val == b"any" {
                r.src_port = NO_PORT;
            } else {
                match parse_port(val) {
                    Ok(p) => r.src_port = p,
                    Err(_) => return -EINVAL,
                }
            }
        } else if key == b"dport" {
            if val == b"any" {
                r.dst_port = NO_PORT;
            } else {
                match parse_port(val) {
                    Ok(p) => r.dst_port = p,
                    Err(_) => return -EINVAL,
                }
            }
        } else if key == b"action" {
            r.action = if val == b"accept" {
                ACCEPT as u8
            } else if val == b"drop" {
                DROP as u8
            } else {
                return -EINVAL;
            };
        } else {
            return -EINVAL;
        }

        i = vstart + vlen;
        while i < len && (line[i] == b' ' || line[i] == b'\t') {
            i += 1;
        }
    }
    unsafe { *out = r };
    0
}

/* --------------------------------------------------------- table transactions */

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_rule_count(v: *const FilterView) -> i32 {
    if v.is_null() {
        return 0;
    }
    let view = unsafe { view(v) };
    // Acquire: a lock-free reader must not see a count published ahead of the
    // slot it describes.
    unsafe { AtomicU32::from_ptr(view.0.count).load(Ordering::Acquire) as i32 }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_rule_add(v: *const FilterView,
                                                rule: *const FilterRule) -> i32 {
    if v.is_null() || rule.is_null() {
        return -EINVAL;
    }
    let view = unsafe { view(v) };
    let count = view.count();
    if count >= view.0.max {
        return -ENOSPC;
    }
    unsafe {
        a20_netfilter_rule_seq_write_begin();
        view.rules()[count as usize] = *rule;
        AtomicU32::from_ptr(view.0.count).store(count + 1, Ordering::Relaxed);
        a20_netfilter_rule_seq_write_end();
    }
    count as i32
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_rule_del(v: *const FilterView,
                                                index: u32) -> i32 {
    if v.is_null() {
        return -EINVAL;
    }
    let view = unsafe { view(v) };
    let count = view.count();
    if index >= count {
        return -EINVAL;
    }
    unsafe {
        a20_netfilter_rule_seq_write_begin();
        let rules = view.rules();
        let mut i = index as usize;
        while i + 1 < count as usize {
            rules[i] = rules[i + 1];
            i += 1;
        }
        let new_count = count - 1;
        AtomicU32::from_ptr(view.0.count).store(new_count, Ordering::Relaxed);
        rules[new_count as usize] = zero_rule();
        a20_netfilter_rule_seq_write_end();
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_rule_reset(v: *const FilterView) {
    if v.is_null() {
        return;
    }
    let view = unsafe { view(v) };
    unsafe {
        a20_netfilter_rule_seq_write_begin();
        for r in view.rules().iter_mut() {
            *r = zero_rule();
        }
        AtomicU32::from_ptr(view.0.count).store(0, Ordering::Relaxed);
        a20_netfilter_rule_seq_write_end();
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_rule_get(v: *const FilterView, index: u32,
                                                out: *mut FilterRule) -> i32 {
    if v.is_null() || out.is_null() {
        return -EINVAL;
    }
    let view = unsafe { view(v) };
    if index >= view.count() {
        return -EINVAL;
    }
    unsafe { *out = view.rules()[index as usize] };
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_rule_stats(v: *const FilterView,
                                                  out: *mut FilterStats) {
    if v.is_null() || out.is_null() {
        return;
    }
    let view = unsafe { view(v) };
    let mut s = FilterStats::default();
    for stats in [view.0.stats_in, view.0.stats_out] {
        if stats.is_null() {
            continue;
        }
        for (which, slot) in [
            (0u32, &mut s.in_packets),
            (1, &mut s.out_packets),
            (2, &mut s.in_dropped),
            (3, &mut s.out_dropped),
            (4, &mut s.in_accepted),
            (5, &mut s.out_accepted),
        ] {
            /* Sum over both direction banks, exactly as the C accessor did:
             * an assignment would let the second bank's zeros overwrite the
             * first bank's counts. */
            let v = load_stat(stat_field(stats, which));
            *slot = slot.wrapping_add(v);
        }
    }
    unsafe { *out = s };
}

/* ---------------------------------------------------------------- the path */

/// Ethernet + optional VLAN + IPv4 + L4 walk.  Ports only for TCP/UDP, echo
/// identifiers only for ICMP echo; a non-first fragment stops after the IPv4
/// header.
#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_parse_frame(f: *const u8, len: usize,
                                                   out: *mut Frame) -> i32 {
    if out.is_null() {
        return 0;
    }
    unsafe { *out = core::mem::zeroed() };
    if f.is_null() {
        return 0;
    }
    let frame = unsafe { core::slice::from_raw_parts(f, len) };
    let mut off = 14usize;
    if len < off {
        return 0;
    }
    let mut ethertype = ((frame[12] as u16) << 8) | frame[13] as u16;
    while ethertype == 0x8100 || ethertype == 0x88a8 {
        if len < off + 4 {
            return 0;
        }
        ethertype = ((frame[off + 2] as u16) << 8) | frame[off + 3] as u16;
        off += 4;
    }
    if ethertype != 0x0800 {
        return 0;
    }
    if len < off + 20 {
        return 0;
    }
    let ip = &frame[off..];
    if (ip[0] >> 4) != 4 {
        return 0;
    }
    let ihl = (ip[0] & 0x0f) as usize * 4;
    if ihl < 20 || len < off + ihl {
        return 0;
    }
    let total = u16::from_be_bytes([ip[2], ip[3]]) as usize;
    if total < ihl || total > ip.len() || off > u16::MAX as usize {
        return 0;
    }
    /* Ethernet padding is not transport data. In particular it cannot supply
     * an otherwise truncated ICMP identifier or a port for a filtering rule. */
    let ip = &ip[..total];
    let transport = &ip[ihl..];
    let frag = (((ip[6] as u16) << 8) | ip[7] as u16) & 0x1fff;
    let mut pkt = Frame {
        src_addr: ((ip[12] as u32) << 24) | ((ip[13] as u32) << 16)
            | ((ip[14] as u32) << 8) | ip[15] as u32,
        dst_addr: ((ip[16] as u32) << 24) | ((ip[17] as u32) << 16)
            | ((ip[18] as u32) << 8) | ip[19] as u32,
        src_port: NO_PORT,
        dst_port: NO_PORT,
        proto: ip[9],
        has_ports: 0,
        ip_off: off as u16,
        l4_off: 0,
        tcp_flags: 0,
        icmp_off: 0,
        icmp_type: 0,
        icmp_id: 0,
    };
    unsafe { *out = pkt };
    if frag != 0 {
        return 1;
    }
    let minimum = if pkt.proto == PROTO_TCP { 20 } else { 8 };
    if (pkt.proto == PROTO_TCP || pkt.proto == PROTO_UDP) && transport.len() >= minimum {
        let l4 = transport;
        pkt.src_port = ((l4[0] as u16) << 8) | l4[1] as u16;
        pkt.dst_port = ((l4[2] as u16) << 8) | l4[3] as u16;
        pkt.l4_off = (off + ihl) as u16;
        pkt.has_ports = 1;
        if pkt.proto == PROTO_TCP {
            pkt.tcp_flags = l4[13] as u16;
        }
        unsafe { *out = pkt };
    } else if pkt.proto == PROTO_ICMP && transport.len() >= 8 {
        let l4 = transport;
        let ty = l4[0];
        if ty == ICMP_ECHO_REQUEST || ty == ICMP_ECHO_REPLY {
            pkt.icmp_type = ty;
            pkt.icmp_id = ((l4[4] as u16) << 8) | l4[5] as u16;
            pkt.icmp_off = (off + ihl) as u16;
            unsafe { *out = pkt };
        }
    }
    1
}

/// True when the table holds at least one rule, sampled under the seqlock so a
/// just-installed rule cannot be missed.  A table that will not settle is
/// treated as populated: that costs a scan rather than skipping a filter.
unsafe fn table_populated(v: &View) -> bool {
    for _ in 0..SEQLOCK_READ_ATTEMPTS {
        let seq0 = unsafe { a20_netfilter_rule_seq_read_begin() };
        if seq0 == 0 {
            continue;
        }
        let n = v.count();
        if unsafe { a20_netfilter_rule_seq_read_retry(seq0) } == 0 {
            return n != 0;
        }
    }
    true
}

fn rule_matches(r: &FilterRule, pkt: &Frame, dir: u8) -> bool {
    r.dir == dir
        && (r.proto == PROTO_ANY || r.proto == pkt.proto)
        && (r.src_addr == NO_ADDR || r.src_addr == pkt.src_addr)
        && (r.dst_addr == NO_ADDR || r.dst_addr == pkt.dst_addr)
        && (r.src_port == NO_PORT || r.src_port == pkt.src_port)
        && (r.dst_port == NO_PORT || r.dst_port == pkt.dst_port)
}

/// Verdict only.  `pkt` is None when the frame did not parse as IPv4, in which
/// case no rule can match it and the answer is the default policy.
unsafe fn eval_filter(v: *const FilterView, len: usize, dir: u8,
                      pkt: *const Frame) -> i32 {
    if v.is_null() {
        return ACCEPT;
    }
    let view = unsafe { view(v) };
    let stats = if dir == DIR_IN { view.0.stats_in } else { view.0.stats_out };
    let packets = stat_field(stats, if dir == DIR_IN { 0 } else { 1 });
    bump(packets, 1);

    if pkt.is_null() {
        return ACCEPT;
    }
    let pkt = unsafe { &*pkt };

    let mut hit: Option<usize> = None;
    let mut action = ACCEPT;
    if unsafe { table_populated(&view) } {
        for _ in 0..SEQLOCK_READ_ATTEMPTS {
            let seq0 = unsafe { a20_netfilter_rule_seq_read_begin() };
            if seq0 == 0 {
                continue;
            }
            let n = view.count();
            let mut found: Option<usize> = None;
            {
                let rules = unsafe { view.rules() };
                let n = (n as usize).min(rules.len());
                for i in 0..n {
                    if rule_matches(&rules[i], pkt, dir) {
                        found = Some(i);
                        break;
                    }
                }
            }
            if unsafe { a20_netfilter_rule_seq_read_retry(seq0) } != 0 {
                continue; /* moved under us; rescan */
            }
            hit = found;
            if let Some(i) = found {
                let rules = unsafe { view.rules() };
                action = rules[i].action as i32;
                /* Charged inside the bracket: a retry simply does not charge,
                 * which understates a counter instead of misattributing it. */
                bump(ptr::addr_of_mut!(rules[i].matched), 1);
                bump(ptr::addr_of_mut!(rules[i].bytes), len as u64);
            }
            break;
        }
    }

    if hit.is_some() && action == DROP {
        return DROP;
    }
    ACCEPT
}

unsafe fn record_verdict(v: *const FilterView, dir: u8, verdict: i32) {
    if v.is_null() { return; }
    let view = unsafe { view(v) };
    let stats = if dir == DIR_IN { view.0.stats_in } else { view.0.stats_out };
    let base = if verdict == DROP { 2 } else { 4 };
    let field = base + if dir == DIR_OUT { 1 } else { 0 };
    bump(stat_field(stats, field), 1);
}

#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_eval(v: *const FilterView, len: usize,
                                            dir: u8, pkt: *const Frame) -> i32 {
    let verdict = unsafe { eval_filter(v, len, dir, pkt) };
    unsafe { record_verdict(v, dir, verdict) };
    verdict
}

/// The hook: filter first, conntrack and NAT only for an accepted packet.  A
/// dropped packet must not create flow state or consume a NAT binding, so the
/// order is not negotiable; a frame that is not IPv4 is counted and accepted.
#[no_mangle]
pub unsafe extern "C" fn a20_netfilter_process(v: *const FilterView,
                                               frame: *mut u8, len: usize,
                                               net_idx: i32, dir: u8) -> i32 {
    if frame.is_null() || len == 0 {
        return ACCEPT;
    }
    let mut pkt = Frame {
        src_addr: 0, dst_addr: 0, src_port: 0, dst_port: 0, proto: 0,
        has_ports: 0, ip_off: 0, l4_off: 0, tcp_flags: 0, icmp_off: 0,
        icmp_type: 0, icmp_id: 0,
    };
    let parsed = unsafe { a20_netfilter_parse_frame(frame, len, &mut pkt) } != 0;
    let verdict = unsafe {
        eval_filter(v, len, dir, if parsed { &pkt } else { ptr::null() })
    };
    /* Acceptance is final only after conntrack/NAT. Charging it before the
     * tail made a full state table drop packets while reporting zero drops. */
    let final_verdict = if verdict != DROP && parsed &&
        unsafe { a20_netfilter_ct_process_tail(frame, len, net_idx, dir, &pkt) } < 0 {
        DROP
    } else { verdict };
    unsafe { record_verdict(v, dir, final_verdict) };
    final_verdict
}

#[cfg(not(test))]
unsafe fn ipv4_from_str(s: *const u8, len: usize, out: *mut u32) -> i32 {
    unsafe { a20_netfilter_ipv4_from_str(s, len, out) }
}

#[cfg(test)]
unsafe fn ipv4_from_str(s: *const u8, len: usize, out: *mut u32) -> i32 {
    /* Test-only stand-in for the shared parser: four dotted decimal octets,
     * host order. */
    if s.is_null() || out.is_null() || len == 0 {
        return 0;
    }
    let b = unsafe { core::slice::from_raw_parts(s, len) };
    let mut v: u32 = 0;
    let mut parts = 0usize;
    let mut cur: u32 = 0;
    let mut digits = 0usize;
    for &c in b.iter().chain(core::iter::once(&b'.')) {
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
    fn transport_bounds_use_ip_length_and_short_echo_never_panics() {
        for payload in 0..=8 {
            let mut frame = [0u8; 64];
            frame[12..14].copy_from_slice(&0x0800u16.to_be_bytes());
            frame[14] = 0x45;
            frame[16..18].copy_from_slice(&((20 + payload) as u16).to_be_bytes());
            frame[23] = PROTO_ICMP;
            frame[34] = ICMP_ECHO_REQUEST;
            frame[38..40].copy_from_slice(&0x1234u16.to_be_bytes());
            let mut parsed = unsafe { core::mem::zeroed::<Frame>() };
            assert_eq!(unsafe { a20_netfilter_parse_frame(
                frame.as_ptr(), 34 + payload, &mut parsed,
            ) }, 1);
            assert_eq!(parsed.icmp_off != 0, payload == 8);
            /* Keeping Ethernet padding must not change the parsed result. */
            assert_eq!(unsafe { a20_netfilter_parse_frame(
                frame.as_ptr(), frame.len(), &mut parsed,
            ) }, 1);
            assert_eq!(parsed.icmp_off != 0, payload == 8);
        }
    }

    #[test]
    fn truncated_ipv4_and_padding_cannot_invent_ports() {
        let mut frame = [0u8; 64];
        frame[12..14].copy_from_slice(&0x0800u16.to_be_bytes());
        frame[14] = 0x45;
        frame[23] = PROTO_UDP;
        frame[34..38].copy_from_slice(&[0, 53, 0x12, 0x34]);
        let mut parsed = unsafe { core::mem::zeroed::<Frame>() };
        frame[16..18].copy_from_slice(&20u16.to_be_bytes());
        assert_eq!(unsafe { a20_netfilter_parse_frame(frame.as_ptr(), 64, &mut parsed) }, 1);
        assert_eq!(parsed.has_ports, 0);
        frame[16..18].copy_from_slice(&60u16.to_be_bytes());
        assert_eq!(unsafe { a20_netfilter_parse_frame(frame.as_ptr(), 64, &mut parsed) }, 0);
    }

    #[test]
    fn parser_refuses_unknown_keys_and_bad_values() {
        let mut r = zero_rule();
        let good = b"in proto=tcp dst=10.0.2.15 dport=80 action=drop";
        assert_eq!(unsafe { a20_netfilter_parse_rule(good.as_ptr(), good.len(), &mut r) }, 0);
        assert_eq!((r.dir, r.proto, r.dst_port, r.action),
                   (DIR_IN, PROTO_TCP, 80, DROP as u8));
        assert_eq!(r.src_addr, NO_ADDR);
        assert_eq!(r.src_port, NO_PORT);

        let bad = b"in proto=tcp bogus=1";
        assert_eq!(unsafe { a20_netfilter_parse_rule(bad.as_ptr(), bad.len(), &mut r) },
                   -EINVAL);
        let bad = b"sideways action=drop";
        assert_eq!(unsafe { a20_netfilter_parse_rule(bad.as_ptr(), bad.len(), &mut r) },
                   -EINVAL);
        let bad = b"in dport=70000 action=drop";
        assert_eq!(unsafe { a20_netfilter_parse_rule(bad.as_ptr(), bad.len(), &mut r) },
                   -EINVAL);
        let bad = b"in action=maybe";
        assert_eq!(unsafe { a20_netfilter_parse_rule(bad.as_ptr(), bad.len(), &mut r) },
                   -EINVAL);
        let bad = b"in proto=999 action=drop";
        assert_eq!(unsafe { a20_netfilter_parse_rule(bad.as_ptr(), bad.len(), &mut r) },
                   -EINVAL);
    }

    #[test]
    fn matching_is_first_match_and_wildcards_anchor_on_their_fields() {
        let pkt = Frame {
            src_addr: 0x0a000202, dst_addr: 0x0a00020f, src_port: 40000,
            dst_port: 80, proto: PROTO_TCP, has_ports: 1, ip_off: 14,
            l4_off: 34, tcp_flags: 0x10, icmp_off: 0, icmp_type: 0, icmp_id: 0,
        };
        let mut r = zero_rule();
        r.dir = DIR_IN;
        r.proto = PROTO_TCP;
        r.src_addr = NO_ADDR;
        r.dst_addr = NO_ADDR;
        r.src_port = NO_PORT;
        r.dst_port = 80;
        r.action = DROP as u8;
        assert!(rule_matches(&r, &pkt, DIR_IN));
        assert!(!rule_matches(&r, &pkt, DIR_OUT));

        let mut wild = zero_rule();
        wild.dir = DIR_IN;
        wild.proto = PROTO_ANY;
        wild.src_addr = NO_ADDR;
        wild.dst_addr = NO_ADDR;
        wild.src_port = NO_PORT;
        wild.dst_port = NO_PORT;
        assert!(rule_matches(&wild, &pkt, DIR_IN));

        /* A rule that anchors on a port the packet does not carry fails even
         * when everything else is wild. */
        wild.dst_port = 443;
        assert!(!rule_matches(&wild, &pkt, DIR_IN));
        wild.dst_port = NO_PORT;
        wild.src_addr = 0x01020304;
        assert!(!rule_matches(&wild, &pkt, DIR_IN));
    }
}
