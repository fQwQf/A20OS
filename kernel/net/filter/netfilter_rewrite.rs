//! Pure packet and tuple transformations for the IPv4 NAT path.
//!
//! The conntrack table, its lock, timeout, and reclamation stay in
//! `netfilter_nat.c`. This module has no allocator or scheduler dependency. The
//! C adapter in `netfilter_rewrite.c` marshals parsed metadata into scalar
//! arguments, so Rust does not duplicate target-dependent C struct layouts.
//!
//! Raw packet entry points are `unsafe`: their C caller must provide a
//! non-null pointer valid for `len` bytes, with unique writable access for the
//! duration of a mutating call. Null and lengths above `isize::MAX` are rejected
//! defensively. All offsets derived from the parsed metadata are checked before
//! frame access. The packet-view struct is read by C before the call and is not
//! passed to Rust, so it cannot alias the mutable frame through this interface.

#![no_std]

use core::slice;

const NETFILTER_PROTO_TCP: u8 = 6;
const NETFILTER_PROTO_UDP: u8 = 17;
const NET_NAT_SNAT: u8 = 1;
const NET_NAT_MASQUERADE: u8 = 2;
const NET_NAT_DNAT: u8 = 3;

#[inline]
fn get16(frame: &[u8], at: usize) -> Option<u16> {
    let a = *frame.get(at)?;
    let b = *frame.get(at.checked_add(1)?)?;
    Some(u16::from_be_bytes([a, b]))
}

#[inline]
fn put16(frame: &mut [u8], at: usize, value: u16) -> bool {
    let Some(end) = at.checked_add(2) else {
        return false;
    };
    let Some(dst) = frame.get_mut(at..end) else {
        return false;
    };
    if let Some(byte) = dst.get_mut(0) {
        *byte = (value >> 8) as u8;
    } else {
        return false;
    }
    if let Some(byte) = dst.get_mut(1) {
        *byte = value as u8;
    } else {
        return false;
    }
    true
}

#[inline]
fn put32(frame: &mut [u8], at: usize, value: u32) -> bool {
    let Some(end) = at.checked_add(4) else {
        return false;
    };
    let Some(dst) = frame.get_mut(at..end) else {
        return false;
    };
    if let Some(byte) = dst.get_mut(0) {
        *byte = (value >> 24) as u8;
    } else {
        return false;
    }
    if let Some(byte) = dst.get_mut(1) {
        *byte = (value >> 16) as u8;
    } else {
        return false;
    }
    if let Some(byte) = dst.get_mut(2) {
        *byte = (value >> 8) as u8;
    } else {
        return false;
    }
    if let Some(byte) = dst.get_mut(3) {
        *byte = value as u8;
    } else {
        return false;
    }
    true
}

#[inline]
fn csum_delta(old_csum: u16, old_word: u32, new_word: u32) -> u16 {
    let mut sum = u32::from(!old_csum);
    sum += u32::from(!(old_word as u16));
    sum += u32::from(!((old_word >> 16) as u16));
    sum += u32::from(new_word as u16);
    sum += new_word >> 16;
    while sum >> 16 != 0 {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    !(sum as u16)
}

fn l4_ckoff(frame: &[u8], l4_off: u16, proto: u8) -> Option<usize> {
    let l4 = usize::from(l4_off);
    match proto {
        NETFILTER_PROTO_UDP => {
            let end = l4.checked_add(8)?;
            if end <= frame.len() {
                l4.checked_add(6)
            } else {
                None
            }
        }
        NETFILTER_PROTO_TCP => {
            let min_end = l4.checked_add(18)?;
            if min_end > frame.len() {
                return None;
            }
            let doff = usize::from(*frame.get(l4.checked_add(12)?)? >> 4);
            if doff < 5 || l4.checked_add(doff.checked_mul(4)?)? > frame.len() {
                return None;
            }
            l4.checked_add(16)
        }
        _ => None,
    }
}

fn fix_l4(
    frame: &mut [u8],
    l4_off: u16,
    proto: u8,
    old_src: u32,
    old_dst: u32,
    new_src: u32,
    new_dst: u32,
) {
    let Some(ckoff) = l4_ckoff(frame, l4_off, proto) else {
        return;
    };
    let Some(old_ck) = get16(frame, ckoff) else {
        return;
    };
    // Only IPv4 UDP uses zero to disable checksumming. TCP zero is a valid
    // computed checksum and must participate in incremental updates.
    if old_ck == 0 && proto == NETFILTER_PROTO_UDP {
        return;
    }
    let ck = csum_delta(csum_delta(old_ck, old_src, new_src), old_dst, new_dst);
    let wire_ck = if proto == NETFILTER_PROTO_UDP && ck == 0 { 0xffff } else { ck };
    let _ = put16(frame, ckoff, wire_ck);
}

#[no_mangle]
pub extern "C" fn a20_rust_netfilter_csum_delta(
    old_csum: u16,
    old_word: u32,
    new_word: u32,
) -> u16 {
    csum_delta(old_csum, old_word, new_word)
}

/// # Safety
/// `p` must be null or point to at least two readable bytes.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_netfilter_get16(p: *const u8) -> u16 {
    if p.is_null() {
        return 0;
    }
    // SAFETY: the caller guarantees the two readable bytes.
    let hi = unsafe { p.read() };
    // SAFETY: the caller guarantees the second readable byte.
    let lo = unsafe { p.add(1).read() };
    u16::from_be_bytes([hi, lo])
}

/// # Safety
/// `p` must be null or point to at least two writable bytes.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_netfilter_put16(p: *mut u8, value: u16) {
    if p.is_null() {
        return;
    }
    // SAFETY: the caller guarantees two writable bytes. Direct byte stores
    // avoid pulling a Rust panic path into this no-std object.
    unsafe {
        p.write((value >> 8) as u8);
        p.add(1).write(value as u8);
    }
}

/// # Safety
/// `frame` must be null or point to `len` readable bytes. A non-null frame with
/// `len > isize::MAX` is rejected before constructing a slice.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_netfilter_l4_ckoff(
    frame: *const u8,
    len: usize,
    l4_off: u16,
    proto: u8,
) -> u16 {
    if frame.is_null() || len > isize::MAX as usize {
        return 0;
    }
    // SAFETY: the C adapter promises a readable frame of this length.
    let bytes = unsafe { slice::from_raw_parts(frame, len) };
    l4_ckoff(bytes, l4_off, proto)
        .and_then(|off| u16::try_from(off).ok())
        .unwrap_or(0)
}

/// # Safety
/// `frame` must be non-null, valid, and uniquely writable for `len` bytes;
/// `len` must not exceed `isize::MAX`. Packet offsets and addresses are parsed
/// metadata, but are still bounds-checked before the frame is accessed.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_netfilter_set_addr(
    frame: *mut u8,
    len: usize,
    ip_off: u16,
    l4_off: u16,
    proto: u8,
    old_src: u32,
    old_dst: u32,
    new_src: u32,
    new_dst: u32,
) {
    if frame.is_null() || len > isize::MAX as usize {
        return;
    }
    // SAFETY: the C adapter guarantees a unique writable buffer for `len`.
    let bytes = unsafe { slice::from_raw_parts_mut(frame, len) };
    let ip = usize::from(ip_off);
    let Some(ip_end) = ip.checked_add(20) else {
        return;
    };
    if ip_end > bytes.len() {
        return;
    }

    if new_src != old_src {
        if !put32(bytes, ip + 12, new_src) {
            return;
        }
        if let Some(old_ck) = get16(bytes, ip + 10) {
            let _ = put16(bytes, ip + 10, csum_delta(old_ck, old_src, new_src));
        }
    }
    if new_dst != old_dst {
        if !put32(bytes, ip + 16, new_dst) {
            return;
        }
        if let Some(old_ck) = get16(bytes, ip + 10) {
            let _ = put16(bytes, ip + 10, csum_delta(old_ck, old_dst, new_dst));
        }
    }
    if new_src != old_src || new_dst != old_dst {
        fix_l4(bytes, l4_off, proto, old_src, old_dst, new_src, new_dst);
    }
}

/// # Safety
/// `frame` must be non-null, valid, and uniquely writable for `len` bytes;
/// `len` must not exceed `isize::MAX`. The L4 offset is checked before access.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_netfilter_set_port(
    frame: *mut u8,
    len: usize,
    l4_off: u16,
    proto: u8,
    is_src: i32,
    new_port: u16,
) {
    if frame.is_null() || len > isize::MAX as usize || new_port == 0 || l4_off == 0 {
        return;
    }
    // SAFETY: the C adapter guarantees a unique writable buffer for `len`.
    let bytes = unsafe { slice::from_raw_parts_mut(frame, len) };
    let l4 = usize::from(l4_off);
    let port_at = match l4.checked_add(if is_src != 0 { 0 } else { 2 }) {
        Some(at) => at,
        None => return,
    };
    let Some(old_port) = get16(bytes, port_at) else {
        return;
    };
    if new_port == old_port || !put16(bytes, port_at, new_port) {
        return;
    }

    let Some(ckoff) = l4_ckoff(bytes, l4_off, proto) else {
        return;
    };
    let Some(old_ck) = get16(bytes, ckoff) else {
        return;
    };
    if old_ck != 0 || proto != NETFILTER_PROTO_UDP {
        let ck = csum_delta(old_ck, u32::from(old_port) << 16, u32::from(new_port) << 16);
        let wire_ck = if proto == NETFILTER_PROTO_UDP && ck == 0 { 0xffff } else { ck };
        let _ = put16(bytes, ckoff, wire_ck);
    }
}

extern "C" {
    fn a20_lwip_netif_ipv4(net_idx: i32) -> u32;
}

/// # Safety
/// The caller must hold the lwIP lock that serializes netif state and keep it
/// held for both lookups. The C packet hook already calls this while holding
/// that lock; calling this entry outside that context would race netif updates.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_netfilter_masq_addr(net_idx: i32) -> u32 {
    // An unnumbered egress cannot borrow another interface's identity. The
    // transaction rejects zero before touching the packet.
    if net_idx < 0 { return 0; }
    unsafe { a20_lwip_netif_ipv4(net_idx) }
}

#[inline]
fn pack_tuple(addr: u32, port: u16) -> u64 {
    (u64::from(addr) << 16) | u64::from(port)
}

#[no_mangle]
pub extern "C" fn a20_rust_netfilter_reply_source(
    nat: u8,
    dst_addr: u32,
    dst_port: u16,
    nat_dst_addr: u32,
    nat_dst_port: u16,
) -> u64 {
    if nat == NET_NAT_DNAT {
        pack_tuple(
            if nat_dst_addr != 0 { nat_dst_addr } else { dst_addr },
            if nat_dst_port != 0 { nat_dst_port } else { dst_port },
        )
    } else {
        pack_tuple(dst_addr, dst_port)
    }
}

#[no_mangle]
pub extern "C" fn a20_rust_netfilter_reply_destination(
    nat: u8,
    src_addr: u32,
    src_port: u16,
    nat_src_addr: u32,
    nat_src_port: u16,
) -> u64 {
    if nat == NET_NAT_SNAT || nat == NET_NAT_MASQUERADE {
        pack_tuple(
            if nat_src_addr != 0 { nat_src_addr } else { src_addr },
            if nat_src_port != 0 { nat_src_port } else { src_port },
        )
    } else {
        pack_tuple(src_addr, src_port)
    }
}
