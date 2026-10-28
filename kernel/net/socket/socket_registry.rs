#![no_std]

//! Socket registry, reference-count ledger and bottom-half scheduling state.
//!
//! The C layer keeps what is genuinely a primitive: the spinlocks, the
//! published slot table (`g_sockets`), `obj_cache` allocation, `resource_ns`
//! release and the destroy sequence. Everything that is a *transition* over
//! that state lives here:
//!
//!   * the per-bucket free bitmap -- claiming a free slot, releasing one, and
//!     the init polarity that pins the slack bits of a bucket narrower than one
//!     bitmap word;
//!   * publishing and releasing a registry slot, including the identity test
//!     that decides whether the slot still belongs to the socket being torn
//!     down and whether the caller still owes the registry's reference;
//!   * the socket reference count -- the acquire, the CAS release loop, the
//!     `ref_magic` canary and the decision of who actually destroys the object;
//!   * the reference ledger (allocs/frees/faults and per-reason fault counts);
//!   * the per-slot bottom-half pending flags and the global pending count that
//!     `sched()` early-outs on;
//!   * the `/proc/net/*` membership and readability predicates.
//!
//! Callers pass borrowed views of the existing C layout (a pointer to
//! `g_sockets`, to `s->in_registry`, to `s->refs`, ...), so no struct ABI
//! changes and the C side keeps owning the critical sections. The lock rule
//! that matters is unchanged: every entry point below runs with the lock the C
//! caller already holds, and none of them takes a lock, allocates or blocks.

use core::ffi::c_void;
use core::ptr;
use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};

/// Canary written into a live socket's `ref_magic`. Mirrors
/// `NET_SOCK_REF_MAGIC` in net/socket/socket_internal.h.
pub const NET_SOCK_REF_MAGIC: u32 = 0x4e53_4f4b; /* 'N','S','O','K' */

/// Reference-ledger fault reasons; mirrors `NET_SOCK_REF_FAULT_REASONS`.
pub const REF_FAULT_ALREADY_FREED: i32 = 0;
pub const REF_FAULT_CANARY_MISMATCH: i32 = 1;
pub const REF_FAULT_REASONS: i32 = 4;

const EBUSY: i32 = 16;
const EINVAL: i32 = 22;
const EOPNOTSUPP: i32 = 95;

const AF_UNSPEC: i32 = 0;
const AF_UNIX: i32 = 1;
const AF_INET: i32 = 2;
const AF_INET6: i32 = 10;
const AF_PACKET: i32 = 17;

const SOCK_STREAM: i32 = 1;
const SOCK_DGRAM: i32 = 2;
const SOCK_RAW: i32 = 3;

const NET_TABLE_TCP: i32 = 0;
const NET_TABLE_UDP: i32 = 1;
const NET_TABLE_UNIX: i32 = 2;

// ------------------------------------------------------------------ *
// Reference-count ledger
// ------------------------------------------------------------------ *

static REF_ALLOCS: AtomicI32 = AtomicI32::new(0);
static REF_FREES: AtomicI32 = AtomicI32::new(0);
static REF_FAULTS: AtomicI32 = AtomicI32::new(0);
static REF_FAULT_N: [AtomicI32; REF_FAULT_REASONS as usize] = [
    AtomicI32::new(0),
    AtomicI32::new(0),
    AtomicI32::new(0),
    AtomicI32::new(0),
];

/// Count a fault and return the negative code the release path hands back.
/// `-1` is "already freed: refs was not positive", `-2` is "canary mismatch:
/// the object was not a live socket". The C caller turns either into the
/// `/proc/net/status` row and, under CONFIG_NET_REF_ASSERT, the panic.
fn ref_fault(reason: i32) -> i32 {
    let r = if reason < 0 || reason >= REF_FAULT_REASONS {
        0
    } else {
        reason
    };
    REF_FAULTS.fetch_add(1, Ordering::Relaxed);
    REF_FAULT_N[r as usize].fetch_add(1, Ordering::Relaxed);
    -(r + 1)
}

/// Initialize the reference count of a freshly allocated socket and count the
/// allocation. Returns 1 when the socket was stamped, 0 when a view was null.
#[no_mangle]
pub unsafe extern "C" fn a20_net_ref_init(refs: *mut i32, magic: *mut u32) -> i32 {
    if refs.is_null() || magic.is_null() {
        return 0;
    }
    // SAFETY: the caller passes the freshly allocated socket's own fields, which
    // no other CPU can reach yet.
    unsafe {
        AtomicI32::from_ptr(refs).store(1, Ordering::Relaxed);
        AtomicU32::from_ptr(magic).store(NET_SOCK_REF_MAGIC, Ordering::Relaxed);
    }
    REF_ALLOCS.fetch_add(1, Ordering::Relaxed);
    1
}

/// Take one reference. Returns the new count, or 0 for a null view.
#[no_mangle]
pub unsafe extern "C" fn a20_net_ref_acquire(refs: *mut i32) -> i32 {
    if refs.is_null() {
        return 0;
    }
    // SAFETY: the caller holds a reference to the socket, so `refs` is live.
    unsafe { AtomicI32::from_ptr(refs).fetch_add(1, Ordering::AcqRel) + 1 }
}

/// Drop one reference.
///
/// Returns 1 when this was the last reference and the caller must destroy the
/// object, 0 when another reference remains, and a negative code when the
/// ledger recorded a fault (`-1` already-freed, `-2` canary mismatch). The
/// caller destroys the object only on 1: the decision is here, the primitive is
/// there.
#[no_mangle]
pub unsafe extern "C" fn a20_net_ref_release(refs: *mut i32, magic: *mut u32) -> i32 {
    if refs.is_null() || magic.is_null() {
        return ref_fault(REF_FAULT_CANARY_MISMATCH);
    }
    // SAFETY: both views belong to the socket the caller holds a reference to.
    let refs = unsafe { AtomicI32::from_ptr(refs) };
    let magic = unsafe { AtomicU32::from_ptr(magic) };
    // The canary is checked before the count on purpose: a free arriving on an
    // object that has already been handed back to obj_cache reads whatever the
    // cache slot now holds, and only the canary distinguishes that from a live
    // socket.
    if magic.load(Ordering::Relaxed) != NET_SOCK_REF_MAGIC {
        return ref_fault(REF_FAULT_CANARY_MISMATCH);
    }
    let mut old = refs.load(Ordering::Acquire);
    loop {
        if old <= 0 {
            return ref_fault(REF_FAULT_ALREADY_FREED);
        }
        match refs.compare_exchange_weak(old, old - 1, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => break,
            Err(current) => old = current,
        }
    }
    if old != 1 {
        return 0;
    }
    magic.store(0, Ordering::Relaxed);
    REF_FREES.fetch_add(1, Ordering::Relaxed);
    1
}

/// Sockets that exist and are not yet freed: allocs - frees.
#[no_mangle]
pub extern "C" fn a20_net_ref_live() -> i32 {
    REF_ALLOCS.load(Ordering::Relaxed) - REF_FREES.load(Ordering::Relaxed)
}

/// Snapshot the ledger for the `/proc/net/status` renderer.
#[no_mangle]
pub unsafe extern "C" fn a20_net_ref_ledger(
    allocs: *mut i32,
    frees: *mut i32,
    faults: *mut i32,
    reasons: *mut i32,
    reasons_len: i32,
) {
    if !allocs.is_null() {
        unsafe { *allocs = REF_ALLOCS.load(Ordering::Relaxed) };
    }
    if !frees.is_null() {
        unsafe { *frees = REF_FREES.load(Ordering::Relaxed) };
    }
    if !faults.is_null() {
        unsafe { *faults = REF_FAULTS.load(Ordering::Relaxed) };
    }
    if reasons.is_null() {
        return;
    }
    let n = reasons_len.clamp(0, REF_FAULT_REASONS);
    for i in 0..n {
        unsafe { *reasons.add(i as usize) = REF_FAULT_N[i as usize].load(Ordering::Relaxed) };
    }
}

#[no_mangle]
pub extern "C" fn a20_net_ref_fault_count(reason: i32) -> i32 {
    if reason < 0 || reason >= REF_FAULT_REASONS {
        return 0;
    }
    REF_FAULT_N[reason as usize].load(Ordering::Relaxed)
}

// ------------------------------------------------------------------ *
// Registry-slot free bitmap
// ------------------------------------------------------------------ *

/// Reset one bucket's bitmap to "every slot free", with the bits past the
/// bucket's last slot pinned occupied.
///
/// A bucket narrower than one bitmap word (the embedded profile's 8-slot table
/// has one slot per bucket) would otherwise hand out a bit offset belonging to
/// the next bucket. Returns 1 when the bitmap was initialized, 0 for a null
/// view, and -EINVAL for a geometry that cannot be expressed.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bucket_init(
    bits: *mut u32,
    words: i32,
    slots_per_bucket: i32,
) -> i32 {
    if bits.is_null() || words <= 0 || slots_per_bucket <= 0 || slots_per_bucket > words * 32 {
        return -EINVAL;
    }
    for w in 0..words {
        unsafe { *bits.add(w as usize) = 0 };
    }
    let slack = words * 32 - slots_per_bucket;
    if slack != 0 {
        // slack is in 1..31 for every profile (at least one slot per bucket, at
        // most 32 slots per word), so the shift is never by the word width.
        unsafe { *bits.add((words - 1) as usize) |= u32::MAX << (32 - slack) };
    }
    1
}

/// Claim a free slot inside one bucket: the lowest free bit of the lowest word
/// that has one, returned as a bucket-local slot index, or -1 when the bucket
/// is full.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bucket_claim(bits: *mut u32, words: i32) -> i32 {
    if bits.is_null() || words <= 0 {
        return -1;
    }
    for w in 0..words {
        let word = unsafe { *bits.add(w as usize) };
        let free = !word;
        if free == 0 {
            continue;
        }
        let bit = free.trailing_zeros() as i32;
        unsafe { *bits.add(w as usize) = word | (1u32 << bit) };
        return w * 32 + bit;
    }
    -1
}

/// Release a bucket-local slot. Returns 1 when a free bit was cleared, 0 for a
/// null view or an out-of-range slot.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bucket_release(bits: *mut u32, words: i32, local: i32) -> i32 {
    if bits.is_null() || words <= 0 || local < 0 || local >= words * 32 {
        return 0;
    }
    let w = (local / 32) as usize;
    let bit = (local % 32) as u32;
    unsafe { *bits.add(w) &= !(1u32 << bit) };
    1
}

// ------------------------------------------------------------------ *
// Bottom-half pending flags
// ------------------------------------------------------------------ *

/// Set a slot's pending flag. Returns 1 when the flag transitioned 0 -> 1 (and
/// the global count was bumped), 0 when it was already set, -EINVAL for a bad
/// slot. Called from lwIP callback context with g_lwip_lock held, so it must
/// stay lock-free and non-blocking.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bh_mark(
    pending: *mut i32,
    count: *mut i32,
    slots: i32,
    idx: i32,
) -> i32 {
    if pending.is_null() || count.is_null() || idx < 0 || idx >= slots {
        return -EINVAL;
    }
    // SAFETY: `idx` was range-checked against the profile's slot ceiling, and
    // the array has exactly that many entries.
    let slot = unsafe { AtomicI32::from_ptr(pending.add(idx as usize)) };
    if slot.swap(1, Ordering::AcqRel) == 0 {
        unsafe { AtomicI32::from_ptr(count).fetch_add(1, Ordering::AcqRel) };
        return 1;
    }
    0
}

/// Clear a slot's pending flag. Returns 1 when the flag transitioned 1 -> 0
/// (and the global count was decremented), 0 when it was already clear.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bh_clear(
    pending: *mut i32,
    count: *mut i32,
    slots: i32,
    idx: i32,
) -> i32 {
    if pending.is_null() || count.is_null() || idx < 0 || idx >= slots {
        return 0;
    }
    // SAFETY: range-checked against the profile ceiling.
    let slot = unsafe { AtomicI32::from_ptr(pending.add(idx as usize)) };
    if slot.swap(0, Ordering::AcqRel) != 0 {
        unsafe { AtomicI32::from_ptr(count).fetch_sub(1, Ordering::AcqRel) };
        return 1;
    }
    0
}

/// Is any socket's bottom half pending? The per-switch scan early-outs on this.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bh_any_pending(count: *const i32) -> i32 {
    if count.is_null() {
        return 0;
    }
    // SAFETY: the caller passes the live global pending counter.
    (unsafe { AtomicI32::from_ptr(count.cast_mut()).load(Ordering::Acquire) } != 0) as i32
}

/// Is the flag of one slot set? The scan asks this rather than reading the
/// array directly, so the atomic access stays in one place.
#[no_mangle]
pub unsafe extern "C" fn a20_net_bh_slot_pending(
    pending: *const i32,
    slots: i32,
    idx: i32,
) -> i32 {
    if pending.is_null() || idx < 0 || idx >= slots {
        return 0;
    }
    // SAFETY: range-checked against the profile ceiling.
    (unsafe { AtomicI32::from_ptr(pending.add(idx as usize).cast_mut()).load(Ordering::Acquire) } != 0)
        as i32
}

// ------------------------------------------------------------------ *
// Registry slot publish / release
// ------------------------------------------------------------------ *

/// Publish `s` into `table[idx]`, set `in_registry`/`reg_idx`, clear the slot's
/// pending flag and take the registry's reference.
///
/// Returns 0 on success, -EINVAL for a null view or out-of-range index, and
/// -EBUSY when the slot is already occupied: a claim that does not match the
/// table it was taken from is a registry bug, and failing here turns it into a
/// refused registration instead of a silent overwrite of another socket's slot.
#[no_mangle]
pub unsafe extern "C" fn a20_net_slot_publish(
    table: *mut *mut c_void,
    slots: i32,
    idx: i32,
    s: *mut c_void,
    in_registry: *mut i32,
    reg_idx: *mut i32,
    refs: *mut i32,
    pending: *mut i32,
    pending_count: *mut i32,
) -> i32 {
    if table.is_null() || s.is_null() || in_registry.is_null() || reg_idx.is_null() {
        return -EINVAL;
    }
    if idx < 0 || idx >= slots {
        return -EINVAL;
    }
    let slot = unsafe { &mut *table.add(idx as usize) };
    if !slot.is_null() {
        return -EBUSY;
    }
    *slot = s;
    unsafe {
        *in_registry = 1;
        *reg_idx = idx;
        a20_net_bh_clear(pending, pending_count, slots, idx);
        a20_net_ref_acquire(refs);
    }
    0
}

/// Release the registry slot a socket's `reg_idx` names.
///
/// Returns 1 when `idx` named a slot at all -- which is exactly the condition
/// under which the caller still owes the registry's reference and must drop it
/// with one more `net_socket_free()` -- and 0 for a socket that never owned a
/// slot (an AF_UNIX accepted child, or one mid-creation). The slot and its
/// bitmap bit are cleared only when the identity test says the slot is still
/// this socket's, and the pending flag is cleared in the same breath;
/// `in_registry`/`reg_idx` are cleared either way, because the socket is no
/// longer table-resident by any reading.
///
/// The reference the slot holds is deliberately *not* dropped here: dropping it
/// can free the object, and that must not happen with a bucket lock held.
#[no_mangle]
pub unsafe extern "C" fn a20_net_slot_release(
    table: *mut *mut c_void,
    slots: i32,
    slots_per_bucket: i32,
    idx: i32,
    s: *const c_void,
    in_registry: *mut i32,
    reg_idx: *mut i32,
    bits: *mut u32,
    words: i32,
    pending: *mut i32,
    pending_count: *mut i32,
) -> i32 {
    if in_registry.is_null() || reg_idx.is_null() {
        return 0;
    }
    let valid = !table.is_null() && idx >= 0 && idx < slots;
    if valid {
        let slot = unsafe { &mut *table.add(idx as usize) };
        if *slot == s.cast_mut() {
            *slot = ptr::null_mut();
            unsafe { a20_net_bh_clear(pending, pending_count, slots, idx) };
            // The bitmap is indexed by the bucket-local slot, not the global
            // one: one bitmap word belongs to exactly one bucket.
            unsafe { a20_net_bucket_release(bits, words, idx % slots_per_bucket) };
        }
    }
    unsafe {
        *in_registry = 0;
        *reg_idx = -1;
    }
    valid as i32
}

// ------------------------------------------------------------------ *
// /proc/net table membership and readability
// ------------------------------------------------------------------ *

/// Which sockets belong in one of the `/proc/net/{tcp,udp,unix}` tables.
/// Membership is by domain and type, not by "does this socket still own a
/// pcb": a loopback connect() drops both lwIP pcbs while the connection is
/// live, so a pcb test would hide exactly the connections a loopback-only stack
/// makes.
#[no_mangle]
pub extern "C" fn a20_net_table_claims(kind: i32, domain: i32, sock_type: i32) -> i32 {
    match kind {
        NET_TABLE_TCP => {
            ((domain == AF_INET || domain == AF_INET6) && sock_type == SOCK_STREAM) as i32
        }
        NET_TABLE_UDP => ((domain == AF_INET || domain == AF_INET6)
            && (sock_type == SOCK_DGRAM || sock_type == SOCK_RAW)) as i32,
        NET_TABLE_UNIX => (domain == AF_UNIX) as i32,
        _ => 0,
    }
}

/// The full row filter for a table walk: namespace, teardown and membership.
/// `family` narrows to AF_INET or AF_INET6, or AF_UNSPEC for "either".
#[no_mangle]
pub extern "C" fn a20_net_table_walk_match(
    kind: i32,
    family: i32,
    domain: i32,
    sock_type: i32,
    closed: i32,
    ns_match: i32,
) -> i32 {
    if ns_match == 0 || closed != 0 {
        return 0;
    }
    if a20_net_table_claims(kind, domain, sock_type) == 0 {
        return 0;
    }
    if family != AF_UNSPEC && domain != family {
        return 0;
    }
    1
}

/// Whether a socket's readability can be reported as a byte count.
/// AF_PACKET captures whole L2 frames with no stream position, so there is no
/// honest number; everything else can answer FIONREAD.
#[no_mangle]
pub extern "C" fn a20_net_rx_available_gate(domain: i32) -> i32 {
    if domain == AF_PACKET {
        return -EOPNOTSUPP;
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[repr(C)]
    struct Socket {
        refs: i32,
        ref_magic: u32,
        in_registry: i32,
        reg_idx: i32,
    }

    fn live_socket() -> Socket {
        let mut s = Socket { refs: 0, ref_magic: 0, in_registry: 0, reg_idx: 0 };
        unsafe { a20_net_ref_init(&mut s.refs, &mut s.ref_magic) };
        s
    }

    #[test]
    fn reference_release_decides_who_destroys() {
        let mut s = live_socket();
        assert_eq!(unsafe { a20_net_ref_acquire(&mut s.refs) }, 2);
        assert_eq!(unsafe { a20_net_ref_release(&mut s.refs, &mut s.ref_magic) }, 0);
        assert_eq!(s.refs, 1);
        assert_eq!(unsafe { a20_net_ref_release(&mut s.refs, &mut s.ref_magic) }, 1);
        assert_eq!(s.ref_magic, 0, "the freeing release clears the canary");
    }

    #[test]
    fn double_free_and_canary_mismatch_are_distinct_faults() {
        let before = a20_net_ref_fault_count(REF_FAULT_ALREADY_FREED);
        let mut s = live_socket();
        assert_eq!(unsafe { a20_net_ref_release(&mut s.refs, &mut s.ref_magic) }, 1);
        assert_eq!(
            unsafe { a20_net_ref_release(&mut s.refs, &mut s.ref_magic) },
            -(REF_FAULT_CANARY_MISMATCH + 1),
            "the canary is checked before the count, so a second free reports the canary"
        );
        let mut t = Socket { refs: 3, ref_magic: 0, in_registry: 0, reg_idx: 0 };
        assert_eq!(
            unsafe { a20_net_ref_release(&mut t.refs, &mut t.ref_magic) },
            -(REF_FAULT_CANARY_MISMATCH + 1)
        );
        /* A live object whose count was driven to zero by a lost reference is
         * the already-freed case; the canary still matches. */
        let mut u = live_socket();
        u.refs = 0;
        assert_eq!(
            unsafe { a20_net_ref_release(&mut u.refs, &mut u.ref_magic) },
            -(REF_FAULT_ALREADY_FREED + 1)
        );
        assert!(a20_net_ref_fault_count(REF_FAULT_ALREADY_FREED) > before);
    }

    #[test]
    fn ledger_tracks_live_sockets() {
        let mut s = live_socket();
        let live_before = a20_net_ref_live();
        assert_eq!(unsafe { a20_net_ref_release(&mut s.refs, &mut s.ref_magic) }, 1);
        assert_eq!(a20_net_ref_live(), live_before - 1);
        let mut allocs = 0;
        let mut frees = 0;
        let mut faults = 0;
        let mut reasons = [0i32; REF_FAULT_REASONS as usize];
        unsafe {
            a20_net_ref_ledger(&mut allocs, &mut frees, &mut faults, reasons.as_mut_ptr(), 4)
        };
        assert!(allocs >= 1 && frees >= 1);
    }

    #[test]
    fn bitmap_claim_release_round_trips_and_pins_slack() {
        /* 8 slots in one word: the high 24 bits must start occupied so a claim
         * can never hand out a bit belonging to the next bucket. */
        let mut bits = [0u32; 1];
        assert_eq!(unsafe { a20_net_bucket_init(bits.as_mut_ptr(), 1, 8) }, 1);
        assert_eq!(bits[0], 0xffff_ff00);
        for expect in 0..8 {
            assert_eq!(unsafe { a20_net_bucket_claim(bits.as_mut_ptr(), 1) }, expect);
        }
        assert_eq!(unsafe { a20_net_bucket_claim(bits.as_mut_ptr(), 1) }, -1);
        assert_eq!(unsafe { a20_net_bucket_release(bits.as_mut_ptr(), 1, 3) }, 1);
        assert_eq!(unsafe { a20_net_bucket_claim(bits.as_mut_ptr(), 1) }, 3);
        /* A full-width bucket has no slack at all. */
        let mut wide = [0u32; 16];
        assert_eq!(unsafe { a20_net_bucket_init(wide.as_mut_ptr(), 16, 512) }, 1);
        assert_eq!(wide[15], 0);
        assert_eq!(unsafe { a20_net_bucket_claim(wide.as_mut_ptr(), 16) }, 0);
        assert_eq!(unsafe { a20_net_bucket_claim(wide.as_mut_ptr(), 16) }, 1);
        assert_eq!(wide[0], 0b11);
        /* A second word is reached only when the first is full. */
        assert_eq!(unsafe { a20_net_bucket_release(bits.as_mut_ptr(), 1, 0) }, 1);
        assert_eq!(unsafe { a20_net_bucket_claim(bits.as_mut_ptr(), 1) }, 0);
    }

    #[test]
    fn publish_and_release_slot_keep_table_and_bitmap_agreeing() {
        let mut table = [ptr::null_mut::<c_void>(); 4];
        let mut bits = [0u32; 1];
        let mut pending = [0i32; 4];
        let mut count = 0i32;
        assert_eq!(unsafe { a20_net_bucket_init(bits.as_mut_ptr(), 1, 4) }, 1);
        let mut s = live_socket();
        let sptr = (&mut s as *mut Socket).cast::<c_void>();
        assert_eq!(
            unsafe {
                a20_net_slot_publish(table.as_mut_ptr(), 4, 2, sptr, &mut s.in_registry,
                                     &mut s.reg_idx, &mut s.refs, pending.as_mut_ptr(),
                                     &mut count)
            },
            0
        );
        assert_eq!((s.in_registry, s.reg_idx), (1, 2));
        assert_eq!(s.refs, 2, "publishing takes the registry's reference");
        assert!(table[2] == sptr);
        /* A second publish onto the same slot is refused, not an overwrite. */
        let mut other = live_socket();
        assert_eq!(
            unsafe {
                a20_net_slot_publish(table.as_mut_ptr(), 4, 2,
                                     (&mut other as *mut Socket).cast::<c_void>(),
                                     &mut other.in_registry, &mut other.reg_idx,
                                     &mut other.refs, pending.as_mut_ptr(), &mut count)
            },
            -EBUSY
        );
        assert_eq!(unsafe { a20_net_bh_mark(pending.as_mut_ptr(), &mut count, 4, 2) }, 1);
        assert_eq!(count, 1);
        assert_eq!(unsafe { a20_net_bh_mark(pending.as_mut_ptr(), &mut count, 4, 2) }, 0);
        assert_eq!(count, 1);
        /* Releasing clears the slot, the bit and the pending flag. */
        assert_eq!(
            unsafe {
                a20_net_slot_release(table.as_mut_ptr(), 4, 4, 2, sptr, &mut s.in_registry,
                                     &mut s.reg_idx, bits.as_mut_ptr(), 1,
                                     pending.as_mut_ptr(), &mut count)
            },
            1
        );
        assert!(table[2].is_null());
        assert_eq!((s.in_registry, s.reg_idx), (0, -1));
        assert_eq!((bits[0] & 0b100, count), (0, 0));
        /* The released slot is reusable, and claim still hands out the lowest
         * free bit rather than the one just released. */
        assert_eq!(unsafe { a20_net_bucket_claim(bits.as_mut_ptr(), 1) }, 0);
        assert_eq!(bits[0] & 0b100, 0);
        assert_eq!(bits[0] & 0b001, 1);
    }

    #[test]
    fn release_of_a_socket_without_a_slot_reports_no_reference() {
        let mut table = [ptr::null_mut::<c_void>(); 4];
        let mut bits = [0u32; 1];
        let mut pending = [0i32; 4];
        let mut count = 0i32;
        assert_eq!(unsafe { a20_net_bucket_init(bits.as_mut_ptr(), 1, 4) }, 1);
        let mut s = live_socket();
        s.reg_idx = -1;
        assert_eq!(
            unsafe {
                a20_net_slot_release(table.as_mut_ptr(), 4, 4, s.reg_idx,
                                     (&mut s as *mut Socket).cast::<c_void>(),
                                     &mut s.in_registry, &mut s.reg_idx, bits.as_mut_ptr(), 1,
                                     pending.as_mut_ptr(), &mut count)
            },
            0,
            "an AF_UNIX accepted child owns no slot and owes no registry reference"
        );
        assert_eq!(bits[0], 0xffff_fff0, "no bit was touched");
        /* A stale index that no longer names this socket leaves the slot alone
         * but still reports that the socket had been registered. */
        let mut other = live_socket();
        table[1] = (&mut other as *mut Socket).cast::<c_void>();
        s.reg_idx = 1;
        assert_eq!(
            unsafe {
                a20_net_slot_release(table.as_mut_ptr(), 4, 4, 1,
                                     (&mut s as *mut Socket).cast::<c_void>(),
                                     &mut s.in_registry, &mut s.reg_idx, bits.as_mut_ptr(), 1,
                                     pending.as_mut_ptr(), &mut count)
            },
            1
        );
        assert!(table[1] == (&mut other as *mut Socket).cast::<c_void>());
        assert_eq!(bits[0], 0xffff_fff0);
    }

    #[test]
    fn table_membership_follows_domain_type_and_family() {
        assert_eq!(a20_net_table_claims(NET_TABLE_TCP, AF_INET, SOCK_STREAM), 1);
        assert_eq!(a20_net_table_claims(NET_TABLE_TCP, AF_INET, SOCK_DGRAM), 0);
        assert_eq!(a20_net_table_claims(NET_TABLE_TCP, AF_UNIX, SOCK_STREAM), 0);
        assert_eq!(a20_net_table_claims(NET_TABLE_UDP, AF_INET6, SOCK_RAW), 1);
        assert_eq!(a20_net_table_claims(NET_TABLE_UDP, AF_INET, SOCK_STREAM), 0);
        assert_eq!(a20_net_table_claims(NET_TABLE_UNIX, AF_UNIX, SOCK_STREAM), 1);
        assert_eq!(a20_net_table_claims(NET_TABLE_UNIX, AF_PACKET, SOCK_RAW), 0);
        /* Family narrowing and the teardown/namespace filters. */
        assert_eq!(
            a20_net_table_walk_match(NET_TABLE_TCP, AF_INET6, AF_INET, SOCK_STREAM, 0, 1),
            0
        );
        assert_eq!(
            a20_net_table_walk_match(NET_TABLE_TCP, AF_UNSPEC, AF_INET6, SOCK_STREAM, 0, 1),
            1
        );
        assert_eq!(
            a20_net_table_walk_match(NET_TABLE_TCP, AF_UNSPEC, AF_INET, SOCK_STREAM, 1, 1),
            0
        );
        assert_eq!(
            a20_net_table_walk_match(NET_TABLE_TCP, AF_UNSPEC, AF_INET, SOCK_STREAM, 0, 0),
            0
        );
    }

    #[test]
    fn rx_available_gate_rejects_only_packet_sockets() {
        assert_eq!(a20_net_rx_available_gate(AF_INET), 0);
        assert_eq!(a20_net_rx_available_gate(AF_UNIX), 0);
        assert_eq!(a20_net_rx_available_gate(AF_PACKET), -EOPNOTSUPP);
    }
}
