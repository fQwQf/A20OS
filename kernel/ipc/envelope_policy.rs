#![no_std]

//! Envelope quota and shadow-table transitions that run while the caller holds
//! the envelope lock. Object lookup, identity pinning, telemetry, registry
//! lifetime, and expiry side effects remain in the C adapter.

const EACCES: i32 = -13;
const EPERM: i32 = -1;
const EINVAL: i32 = -22;
const EMFILE: i32 = -24;

pub const DENY_NONE: i32 = 0;
pub const DENY_RIGHTS: i32 = 1;
pub const DENY_OPS: i32 = 2;
pub const DENY_DATA: i32 = 3;

const EMPTY_VFILE_IDENTITY: u64 = 0;
const REGISTRY_SLOT_BITS: u32 = 10;
const REGISTRY_SLOT_MASK: u32 = (1 << REGISTRY_SLOT_BITS) - 1;
const REGISTRY_MAX_GENERATION: u32 = (i32::MAX as u32) >> REGISTRY_SLOT_BITS;

#[inline]
fn shadow_pointer_valid(entries: *const A20EnvShadowState, capacity: u32) -> bool {
    !entries.is_null()
        && capacity != 0
        && capacity <= 4096
        && (entries as usize) & (core::mem::align_of::<A20EnvShadowState>() - 1) == 0
}

#[inline]
unsafe fn shadow_entry(entries: *mut A20EnvShadowState, index: u32) -> *mut A20EnvShadowState {
    entries.wrapping_add(index as usize)
}

#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct A20EnvShadowState {
    pub vfile_identity: u64,
    pub obj_type: u8,
    pub reserved: [u8; 7],
    pub rights: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20EnvRegistrySlot {
    pub ptr: *mut core::ffi::c_void,
    pub owner_task: *mut core::ffi::c_void,
    pub generation: u32,
    pub owner_pid: i32,
    pub owner_live: u8,
    pub retired: u8,
    pub reserved: u16,
}

#[repr(C)]
pub struct A20EnvUseTransaction {
    pub expired: *mut i32,
    pub expire_tick: u64,
    pub now: u64,
    pub shadows: *mut A20EnvShadowState,
    pub shadow_capacity: u32,
    pub identity: u64,
    pub remaining_ops: *mut u64,
    pub remaining_data: *mut u64,
    pub want_write: i32,
    pub bytes: u64,
    pub use_ok: *mut u64,
    pub deny_rights: *mut u64,
    pub deny_ops: *mut u64,
    pub deny_data: *mut u64,
    pub bytes_charged: *mut u64,
}

const _: [(); ((2 * core::mem::size_of::<usize>() + 12 + core::mem::align_of::<usize>() - 1)
    / core::mem::align_of::<usize>()) * core::mem::align_of::<usize>()] =
    [(); core::mem::size_of::<A20EnvRegistrySlot>()];
const _: [(); 2 * core::mem::size_of::<usize>()] =
    [(); core::mem::offset_of!(A20EnvRegistrySlot, generation)];
const _: [(); 2 * core::mem::size_of::<usize>() + 8] =
    [(); core::mem::offset_of!(A20EnvRegistrySlot, owner_live)];

#[inline]
fn registry_valid(slots: *const A20EnvRegistrySlot, capacity: u32) -> bool {
    !slots.is_null()
        && capacity == 1024
        && (slots as usize) & (core::mem::align_of::<A20EnvRegistrySlot>() - 1) == 0
}

#[inline]
unsafe fn registry_slot(slots: *mut A20EnvRegistrySlot, index: u32) -> *mut A20EnvRegistrySlot {
    slots.wrapping_add(index as usize)
}

#[no_mangle]
pub unsafe extern "C" fn a20_envelope_registry_insert(
    slots: *mut A20EnvRegistrySlot,
    capacity: u32,
    ptr: *mut core::ffi::c_void,
    owner_task: *mut core::ffi::c_void,
    owner_pid: i32,
) -> i64 {
    if !registry_valid(slots, capacity) || ptr.is_null() || owner_task.is_null() || owner_pid < 0 { return EINVAL as i64; }
    let mut index = 0u32;
    while index < capacity {
        let slot = unsafe { &mut *registry_slot(slots, index) };
        if slot.ptr.is_null() && slot.retired == 0 {
            slot.ptr = ptr;
            slot.owner_task = owner_task;
            slot.owner_pid = owner_pid;
            slot.owner_live = 1;
            let id = (slot.generation << REGISTRY_SLOT_BITS) | index;
            return id as i64;
        }
        index = index.wrapping_add(1);
    }
    -16 // -EBUSY
}

#[no_mangle]
pub unsafe extern "C" fn a20_envelope_registry_lookup(
    slots: *mut A20EnvRegistrySlot,
    capacity: u32,
    id: i64,
    require_owner: i32,
) -> *mut core::ffi::c_void {
    if !registry_valid(slots, capacity) || id < 0 || id > i32::MAX as i64 { return core::ptr::null_mut(); }
    let raw = id as u32;
    let index = raw & REGISTRY_SLOT_MASK;
    if index >= capacity { return core::ptr::null_mut(); }
    let slot = unsafe { &mut *registry_slot(slots, index) };
    if slot.ptr.is_null() || slot.generation != (raw >> REGISTRY_SLOT_BITS)
        || (require_owner != 0 && slot.owner_live == 0)
    {
        return core::ptr::null_mut();
    }
    slot.ptr
}

#[no_mangle]
pub unsafe extern "C" fn a20_envelope_registry_at(
    slots: *mut A20EnvRegistrySlot,
    capacity: u32,
    index: u32,
) -> *mut core::ffi::c_void {
    if !registry_valid(slots, capacity) || index >= capacity { return core::ptr::null_mut(); }
    unsafe { (*registry_slot(slots, index)).ptr }
}

#[no_mangle]
pub unsafe extern "C" fn a20_envelope_registry_close_owner(
    slots: *mut A20EnvRegistrySlot,
    capacity: u32,
    owner_task: *mut core::ffi::c_void,
) -> *mut core::ffi::c_void {
    if !registry_valid(slots, capacity) || owner_task.is_null() { return core::ptr::null_mut(); }
    let mut index = 0u32;
    while index < capacity {
        let slot = unsafe { &mut *registry_slot(slots, index) };
        if !slot.ptr.is_null() && slot.owner_live != 0 && slot.owner_task == owner_task {
            slot.owner_live = 0;
            return slot.ptr;
        }
        index = index.wrapping_add(1);
    }
    core::ptr::null_mut()
}

#[no_mangle]
pub unsafe extern "C" fn a20_envelope_registry_remove(
    slots: *mut A20EnvRegistrySlot,
    capacity: u32,
    ptr: *mut core::ffi::c_void,
) -> i32 {
    if !registry_valid(slots, capacity) || ptr.is_null() { return EINVAL; }
    let mut index = 0u32;
    while index < capacity {
        let slot = unsafe { &mut *registry_slot(slots, index) };
        if slot.ptr == ptr {
            if slot.owner_live != 0 { return -16; }
            slot.ptr = core::ptr::null_mut();
            slot.owner_task = core::ptr::null_mut();
            slot.owner_pid = 0;
            slot.reserved = 0;
            if slot.generation == REGISTRY_MAX_GENERATION {
                slot.retired = 1;
            } else {
                slot.generation = slot.generation.wrapping_add(1);
            }
            return 0;
        }
        index = index.wrapping_add(1);
    }
    -2 // -ENOENT
}

const _: [(); 24] = [(); core::mem::size_of::<A20EnvShadowState>()];
const _: [(); 16] = [(); core::mem::offset_of!(A20EnvShadowState, rights)];

#[no_mangle]
pub unsafe extern "C" fn a20_envelope_policy_expire(
    expired: *mut i32,
    expire_tick: u64,
    now: u64,
) -> i32 {
    if expired.is_null() { return EINVAL; }
    let state = unsafe { &mut *expired };
    if *state == 0 && expire_tick != 0 && now >= expire_tick {
        *state = 1;
    }
    i32::from(*state != 0)
}

#[no_mangle]
pub unsafe extern "C" fn a20_envelope_policy_charge_acquire(
    remaining_ops: *mut u64,
) -> i32 {
    if remaining_ops.is_null() { return EINVAL; }
    let ops = unsafe { &mut *remaining_ops };
    if *ops == 0 { return EACCES; }
    *ops -= 1;
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_envelope_policy_charge_propagation(
    propagation_types: u32,
    kind: u32,
    remaining_ops: *mut u64,
) -> i32 {
    if remaining_ops.is_null() || kind >= 32 { return EINVAL; }
    if propagation_types & (1u32 << kind) == 0 { return EPERM; }
    unsafe { a20_envelope_policy_charge_acquire(remaining_ops) }
}

/// The SCM_RIGHTS callback's single locked state transition: mark expiry and
/// deny before propagation or op-budget mutation, otherwise apply both checks.
#[no_mangle]
pub unsafe extern "C" fn a20_envelope_policy_charge_propagation_at(
    expired: *mut i32,
    expire_tick: u64,
    now: u64,
    propagation_types: u32,
    kind: u32,
    remaining_ops: *mut u64,
) -> i32 {
    if unsafe { a20_envelope_policy_expire(expired, expire_tick, now) } != 0 {
        return EACCES;
    }
    unsafe { a20_envelope_policy_charge_propagation(propagation_types, kind, remaining_ops) }
}

/// Preserve the established C transaction order: direction check, op debit,
/// data-limit check, then data debit. Thus a data-budget denial still spends
/// the op budget, matching the existing envelope ABI contract.
#[no_mangle]
pub unsafe extern "C" fn a20_envelope_policy_charge_use(
    remaining_ops: *mut u64,
    remaining_data: *mut u64,
    shadow_rights: u64,
    tracked: i32,
    want_write: i32,
    bytes: u64,
) -> i32 {
    if remaining_ops.is_null() || remaining_data.is_null() { return EINVAL; }
    if tracked != 0 && want_write >= 0 {
        let need = if want_write != 0 { 2u64 } else { 1u64 };
        if shadow_rights & need == 0 { return DENY_RIGHTS; }
    }
    let ops = unsafe { &mut *remaining_ops };
    if *ops == 0 { return DENY_OPS; }
    *ops -= 1;
    if bytes != 0 {
        let data = unsafe { &mut *remaining_data };
        if *data < bytes { return DENY_DATA; }
        *data -= bytes;
    }
    DENY_NONE
}

/// Own the locked use-charge transaction: expiry, vfile-identity rights,
/// operation/data budget ordering, and exactly one matching result counter.
#[no_mangle]
pub unsafe extern "C" fn a20_envelope_policy_use_transaction(
    txn: *mut A20EnvUseTransaction,
) -> i32 {
    if txn.is_null() { return EINVAL; }
    let txn = unsafe { &mut *txn };
    if txn.expired.is_null() || txn.shadows.is_null()
        || txn.remaining_ops.is_null() || txn.remaining_data.is_null()
        || txn.use_ok.is_null() || txn.deny_rights.is_null()
        || txn.deny_ops.is_null() || txn.deny_data.is_null()
        || txn.bytes_charged.is_null()
    { return EINVAL; }

    if unsafe { a20_envelope_policy_expire(txn.expired, txn.expire_tick, txn.now) } != 0 {
        return EACCES;
    }
    let mut rights = 0u64;
    let mut tracked = 0i32;
    if unsafe { a20_envelope_policy_shadow_lookup(txn.shadows, txn.shadow_capacity,
                                                   txn.identity, &mut rights, &mut tracked) } != 0 {
        return EACCES;
    }
    let decision = unsafe { a20_envelope_policy_charge_use(
        txn.remaining_ops, txn.remaining_data, rights, tracked, txn.want_write, txn.bytes,
    ) };
    match decision {
        DENY_RIGHTS => unsafe { *txn.deny_rights = (*txn.deny_rights).wrapping_add(1); },
        DENY_OPS => unsafe { *txn.deny_ops = (*txn.deny_ops).wrapping_add(1); },
        DENY_DATA => unsafe { *txn.deny_data = (*txn.deny_data).wrapping_add(1); },
        DENY_NONE => unsafe {
            *txn.bytes_charged = (*txn.bytes_charged).wrapping_add(txn.bytes);
            *txn.use_ok = (*txn.use_ok).wrapping_add(1);
        },
        _ => return EACCES,
    }
    if decision == DENY_NONE { 0 } else { EACCES }
}

#[no_mangle]
pub unsafe extern "C" fn a20_envelope_policy_shadow_init(
    entries: *mut A20EnvShadowState,
    capacity: u32,
) -> i32 {
    if !shadow_pointer_valid(entries, capacity) {
        return EINVAL;
    }
    let mut index = 0u32;
    while index < capacity {
        unsafe {
            shadow_entry(entries, index).write(A20EnvShadowState {
                vfile_identity: EMPTY_VFILE_IDENTITY,
                ..A20EnvShadowState::default()
            });
        }
        index = index.wrapping_add(1);
    }
    0
}

/// Install authorization by stable vfile identity. The first grant is
/// immutable for this envelope/vfile pair: later alias acquisitions may not
/// broaden or narrow authority already shared by dup/fork aliases.
#[no_mangle]
pub unsafe extern "C" fn a20_envelope_policy_shadow_upsert(
    entries: *mut A20EnvShadowState,
    capacity: u32,
    identity: u64,
    obj_type: u8,
    rights: u64,
) -> i32 {
    if !shadow_pointer_valid(entries, capacity) || identity == EMPTY_VFILE_IDENTITY {
        return EINVAL;
    }
    let mut slot = u32::MAX;
    let mut index = 0u32;
    while index < capacity {
        let entry = unsafe { shadow_entry(entries, index).read() };
        if entry.vfile_identity == identity {
            return index as i32;
        }
        if slot == u32::MAX && entry.vfile_identity == EMPTY_VFILE_IDENTITY {
            slot = index;
        }
        index = index.wrapping_add(1);
    }
    if slot == u32::MAX { return EMFILE; }
    unsafe { shadow_entry(entries, slot).write(A20EnvShadowState {
        vfile_identity: identity,
        obj_type,
        reserved: [0; 7],
        rights,
    }); }
    slot as i32
}

/// Keep the old acquisition ordering exactly: a full shadow table is detected
/// after consuming one op, while an exhausted op budget changes no table row.
#[no_mangle]
pub unsafe extern "C" fn a20_envelope_policy_acquire_shadow(
    remaining_ops: *mut u64,
    entries: *mut A20EnvShadowState,
    capacity: u32,
    identity: u64,
    obj_type: u8,
    rights: u64,
) -> i32 {
    if remaining_ops.is_null() { return EINVAL; }
    let charge = unsafe { a20_envelope_policy_charge_acquire(remaining_ops) };
    if charge != 0 { return charge; }
    let inserted = unsafe {
        a20_envelope_policy_shadow_upsert(entries, capacity, identity, obj_type, rights)
    };
    if inserted < 0 { inserted } else { 0 }
}

#[no_mangle]
pub unsafe extern "C" fn a20_envelope_policy_shadow_lookup(
    entries: *const A20EnvShadowState,
    capacity: u32,
    identity: u64,
    rights: *mut u64,
    tracked: *mut i32,
) -> i32 {
    if !shadow_pointer_valid(entries, capacity) || rights.is_null() || tracked.is_null()
        || (rights as usize) & (core::mem::align_of::<u64>() - 1) != 0
        || (tracked as usize) & (core::mem::align_of::<i32>() - 1) != 0
    {
        return EINVAL;
    }
    unsafe { rights.write(0); tracked.write(0); }
    let mut index = 0u32;
    while index < capacity {
        let entry = unsafe { shadow_entry(entries as *mut A20EnvShadowState, index).read() };
        if entry.vfile_identity == identity {
            unsafe { rights.write(entry.rights); tracked.write(1); }
            break;
        }
        index = index.wrapping_add(1);
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_envelope_policy_shadow_count(
    entries: *const A20EnvShadowState,
    capacity: u32,
) -> i32 {
    if !shadow_pointer_valid(entries, capacity) {
        return EINVAL;
    }
    let mut count = 0i32;
    let mut index = 0u32;
    while index < capacity {
        if unsafe { shadow_entry(entries as *mut A20EnvShadowState, index).read().vfile_identity } != EMPTY_VFILE_IDENTITY {
            count = count.wrapping_add(1);
        }
        index = index.wrapping_add(1);
    }
    count
}

/// Remove the registry contents when the last envelope reference is destroyed.
/// Numeric per-fd deletion is deliberately not provided: it cannot safely
/// account for shared files_structs, fork aliases, or reused local slots.
#[no_mangle]
pub unsafe extern "C" fn a20_envelope_policy_shadow_clear(
    entries: *mut A20EnvShadowState,
    capacity: u32,
) -> i32 {
    unsafe { a20_envelope_policy_shadow_init(entries, capacity) }
}

/// Reclaim a vfile's authorization when its final reference is released.
#[no_mangle]
pub unsafe extern "C" fn a20_envelope_policy_shadow_remove(
    entries: *mut A20EnvShadowState,
    capacity: u32,
    identity: u64,
) -> i32 {
    if !shadow_pointer_valid(entries, capacity) || identity == EMPTY_VFILE_IDENTITY {
        return EINVAL;
    }
    let mut removed = 0i32;
    let mut index = 0u32;
    while index < capacity {
        let entry = unsafe { shadow_entry(entries, index).read() };
        if entry.vfile_identity == identity {
            unsafe {
                shadow_entry(entries, index).write(A20EnvShadowState {
                    vfile_identity: EMPTY_VFILE_IDENTITY,
                    ..A20EnvShadowState::default()
                });
            }
            removed += 1;
        }
        index = index.wrapping_add(1);
    }
    removed
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn expiration_is_monotonic_and_inclusive() {
        let mut expired = 0;
        assert_eq!(unsafe { a20_envelope_policy_expire(&mut expired, 8, 7) }, 0);
        assert_eq!(unsafe { a20_envelope_policy_expire(&mut expired, 8, 8) }, 1);
        assert_eq!(unsafe { a20_envelope_policy_expire(&mut expired, 0, 0) }, 1);
    }

    #[test]
    fn propagation_and_use_preserve_legacy_order() {
        let mut ops = 2;
        assert_eq!(unsafe { a20_envelope_policy_charge_propagation(1 << 3, 3, &mut ops) }, 0);
        assert_eq!(ops, 1);
        assert_eq!(unsafe { a20_envelope_policy_charge_propagation(0, 3, &mut ops) }, EPERM);
        let mut data = 3;
        assert_eq!(unsafe { a20_envelope_policy_charge_use(&mut ops, &mut data, 1, 1, 1, 4) }, DENY_RIGHTS);
        assert_eq!((ops, data), (1, 3));
        assert_eq!(unsafe { a20_envelope_policy_charge_use(&mut ops, &mut data, 2, 1, 1, 4) }, DENY_DATA);
        assert_eq!((ops, data), (0, 3));
    }

    #[test]
    fn use_transaction_commits_one_result_and_keeps_failure_order() {
        let mut shadows = [A20EnvShadowState::default(); 2];
        shadows[0] = A20EnvShadowState { vfile_identity: 7, obj_type: 4, reserved: [0; 7], rights: 1 };
        let (mut expired, mut ops, mut data) = (0, 2, 3);
        let (mut use_ok, mut deny_rights, mut deny_ops, mut deny_data, mut charged) = (0, 0, 0, 0, 0);
        let mut txn = A20EnvUseTransaction {
            expired: &mut expired, expire_tick: 0, now: 0, shadows: shadows.as_mut_ptr(),
            shadow_capacity: 2, identity: 7, remaining_ops: &mut ops, remaining_data: &mut data,
            want_write: 1, bytes: 2, use_ok: &mut use_ok, deny_rights: &mut deny_rights,
            deny_ops: &mut deny_ops, deny_data: &mut deny_data, bytes_charged: &mut charged,
        };
        assert_eq!(unsafe { a20_envelope_policy_use_transaction(&mut txn) }, EACCES);
        assert_eq!((ops, data, deny_rights, deny_ops, deny_data, use_ok, charged), (2, 3, 1, 0, 0, 0, 0));

        txn.want_write = 0;
        txn.bytes = 4;
        assert_eq!(unsafe { a20_envelope_policy_use_transaction(&mut txn) }, EACCES);
        assert_eq!((ops, data, deny_rights, deny_ops, deny_data, use_ok, charged), (1, 3, 1, 0, 1, 0, 0));

        txn.identity = 9; // untracked/grandfathered authorities remain usable
        txn.bytes = 2;
        assert_eq!(unsafe { a20_envelope_policy_use_transaction(&mut txn) }, 0);
        assert_eq!((ops, data, deny_rights, deny_ops, deny_data, use_ok, charged), (0, 1, 1, 0, 1, 1, 2));

        ops = 1;
        txn.expire_tick = 5;
        txn.now = 5;
        assert_eq!(unsafe { a20_envelope_policy_use_transaction(&mut txn) }, EACCES);
        assert_eq!((expired, ops, data, deny_rights, deny_ops, deny_data, use_ok, charged), (1, 1, 1, 1, 0, 1, 1, 2));
    }

    #[test]
    fn shadow_authority_follows_vfile_identity_and_releases() {
        let mut entries = [A20EnvShadowState::default(); 3];
        assert_eq!(unsafe { a20_envelope_policy_shadow_init(entries.as_mut_ptr(), 3) }, 0);
        assert_eq!(unsafe { a20_envelope_policy_shadow_upsert(entries.as_mut_ptr(), 3, 0x101, 4, 1) }, 0);
        /* An fd alias is the same open-file identity; a new object in that fd
         * slot has a different identity and does not inherit its rights. */
        let (mut rights, mut tracked) = (0, 0);
        assert_eq!(unsafe { a20_envelope_policy_shadow_lookup(entries.as_ptr(), 3, 0x101, &mut rights, &mut tracked) }, 0);
        assert_eq!((rights, tracked), (1, 1));
        /* A later alias transfer cannot promote a READ grant to READ|WRITE,
         * and a restrictive alias cannot revoke authority from existing
         * aliases. */
        assert_eq!(unsafe { a20_envelope_policy_shadow_upsert(entries.as_mut_ptr(), 3, 0x101, 4, 3) }, 0);
        assert_eq!(unsafe { a20_envelope_policy_shadow_upsert(entries.as_mut_ptr(), 3, 0x101, 4, 0) }, 0);
        assert_eq!(unsafe { a20_envelope_policy_shadow_lookup(entries.as_ptr(), 3, 0x101, &mut rights, &mut tracked) }, 0);
        assert_eq!((rights, tracked), (1, 1));
        let (mut ops, mut data) = (2, 8);
        assert_eq!(unsafe { a20_envelope_policy_charge_use(&mut ops, &mut data, rights, tracked, 1, 0) }, DENY_RIGHTS);
        assert_eq!((ops, data), (2, 8));
        assert_eq!(unsafe { a20_envelope_policy_charge_use(&mut ops, &mut data, rights, tracked, 0, 0) }, DENY_NONE);
        assert_eq!((ops, data), (1, 8));
        assert_eq!(unsafe { a20_envelope_policy_shadow_lookup(entries.as_ptr(), 3, 0x202, &mut rights, &mut tracked) }, 0);
        assert_eq!((rights, tracked), (0, 0));
        assert_eq!(unsafe { a20_envelope_policy_shadow_upsert(entries.as_mut_ptr(), 3, 0x202, 4, 2) }, 1);
        assert_eq!(unsafe { a20_envelope_policy_shadow_remove(entries.as_mut_ptr(), 3, 0x101) }, 1);
        assert_eq!(unsafe { a20_envelope_policy_shadow_count(entries.as_ptr(), 3) }, 1);
        assert_eq!(unsafe { a20_envelope_policy_shadow_upsert(entries.as_mut_ptr(), 3, 0x303, 4, 1) }, 0);
        assert_eq!(unsafe { a20_envelope_policy_shadow_remove(entries.as_mut_ptr(), 3, 0x202) }, 1);
        assert_eq!(unsafe { a20_envelope_policy_shadow_remove(entries.as_mut_ptr(), 3, 0x303) }, 1);
        assert_eq!(unsafe { a20_envelope_policy_shadow_count(entries.as_ptr(), 3) }, 0);
        assert_eq!(unsafe { a20_envelope_policy_shadow_upsert(entries.as_mut_ptr(), 3, 0, 4, 1) }, EINVAL);
    }

    #[test]
    fn registry_owner_close_reuse_and_stale_ids() {
        let empty = A20EnvRegistrySlot {
            ptr: core::ptr::null_mut(), owner_task: core::ptr::null_mut(), generation: 0, owner_pid: 0,
            owner_live: 0, retired: 0, reserved: 0,
        };
        let mut slots = [empty; 1024];
        let first = 0x1000usize as *mut core::ffi::c_void;
        let second = 0x2000usize as *mut core::ffi::c_void;
        let owner_a = 0x3000usize as *mut core::ffi::c_void;
        let owner_b = 0x4000usize as *mut core::ffi::c_void;
        let id = unsafe { a20_envelope_registry_insert(slots.as_mut_ptr(), 1024, first, owner_a, 41) };
        assert_eq!(id, 0);
        assert_eq!(unsafe { a20_envelope_registry_lookup(slots.as_mut_ptr(), 1024, id, 1) }, first);
        /* A reused numeric PID must not close the old task's owner slot. */
        assert!(unsafe { a20_envelope_registry_close_owner(slots.as_mut_ptr(), 1024, owner_b) }.is_null());
        assert_eq!(unsafe { a20_envelope_registry_close_owner(slots.as_mut_ptr(), 1024, owner_a) }, first);
        assert!(unsafe { a20_envelope_registry_lookup(slots.as_mut_ptr(), 1024, id, 1) }.is_null());
        assert_eq!(unsafe { a20_envelope_registry_at(slots.as_mut_ptr(), 1024, 0) }, first);
        assert_eq!(unsafe { a20_envelope_registry_remove(slots.as_mut_ptr(), 1024, first) }, 0);
        let replacement = unsafe { a20_envelope_registry_insert(slots.as_mut_ptr(), 1024, second, owner_b, 41) };
        assert_eq!(replacement, 1024);
        assert!(unsafe { a20_envelope_registry_lookup(slots.as_mut_ptr(), 1024, id, 0) }.is_null());
        assert_eq!(unsafe { a20_envelope_registry_lookup(slots.as_mut_ptr(), 1024, replacement, 1) }, second);
    }

    #[test]
    fn registry_full_table_is_bounded() {
        let empty = A20EnvRegistrySlot {
            ptr: core::ptr::null_mut(), owner_task: core::ptr::null_mut(), generation: 0, owner_pid: 0,
            owner_live: 0, retired: 0, reserved: 0,
        };
        let mut slots = [empty; 1024];
        let mut index = 0usize;
        while index < 1024 {
            let ptr = (0x1000usize + index * 16) as *mut core::ffi::c_void;
            assert_eq!(unsafe { a20_envelope_registry_insert(slots.as_mut_ptr(), 1024, ptr, 0x8000usize as *mut core::ffi::c_void, index as i32 + 1) }, index as i64);
            index += 1;
        }
        assert_eq!(unsafe { a20_envelope_registry_insert(slots.as_mut_ptr(), 1024, 0x9000usize as *mut core::ffi::c_void, 0x8000usize as *mut core::ffi::c_void, 2000) }, -16);
    }

    #[test]
    fn registry_retires_slot_before_handle_wrap() {
        let empty = A20EnvRegistrySlot {
            ptr: core::ptr::null_mut(), owner_task: core::ptr::null_mut(), generation: 0,
            owner_pid: 0, owner_live: 0, retired: 0, reserved: 0,
        };
        let mut slots = [empty; 1024];
        slots[0].generation = REGISTRY_MAX_GENERATION;
        let first = 0x3000usize as *mut core::ffi::c_void;
        let second = 0x4000usize as *mut core::ffi::c_void;
        let owner = 0x5000usize as *mut core::ffi::c_void;
        let last = unsafe { a20_envelope_registry_insert(slots.as_mut_ptr(), 1024, first, owner, 1) };
        assert_eq!(last, ((REGISTRY_MAX_GENERATION as i64) << REGISTRY_SLOT_BITS));
        assert_eq!(unsafe { a20_envelope_registry_close_owner(slots.as_mut_ptr(), 1024, owner) }, first);
        assert_eq!(unsafe { a20_envelope_registry_remove(slots.as_mut_ptr(), 1024, first) }, 0);
        assert_eq!(slots[0].retired, 1);
        assert_eq!(unsafe { a20_envelope_registry_insert(slots.as_mut_ptr(), 1024, second, owner, 2) }, 1);
    }
}
