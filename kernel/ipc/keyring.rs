#![cfg_attr(not(test), no_std)]

//! Rust-owned Linux key/keyring objects, serial index, membership graph,
//! references, expiry, credential decisions, and command behavior.
//!
//! The C ABI adapter (`keyring_rust_bridge.c`) is deliberately mechanical:
//! exported `keyring_*` names forward to this module, while bridge helpers
//! expose the current task's credentials/session slot, the irqsave lock,
//! monotonic time, and slab allocation. User pointers remain in
//! `kernel/abi/linux/sys_keyring.c`.
//!
//! # Safety and ownership invariants
//!
//! The one irqsave lock protects the serial map, object fields, membership
//! lists, refcounts, and graph-walk scratch storage. `Object` and `Item` nodes
//! are allocated and freed only through the C slab bridge. The serial map is
//! a non-owning index; each live object has a positive ownership count from a
//! task session slot, a persistent user-ring pin, or a containing ring item.
//! Removing the final ownership reference removes the map entry before
//! iteratively reclaiming members. A ring has at most one item for each object,
//! and ring-to-ring links cannot make a graph cycle. This makes final put
//! reclamation finite and keeps every indexed object alive.
//!
//! Every exported function that reads or mutates shared state acquires
//! `LockGuard`; bridge helpers are called only while the caller holds that
//! lock except for current-task/credential reads, allocation, and monotonic
//! time. C callers must pass live kernel buffers and task pointers according
//! to the function contract. User pointers never cross this boundary. Object
//! payloads and names are fully allocated/copied before publication; a failed
//! member allocation leaves the old graph unchanged, and add-key rolls back
//! its serial-map insertion if linking fails. Session replacement publishes
//! the new ring before dropping the old task reference; the old map entry is
//! retained until the final child/member reference goes away.

use core::ffi::{c_char, c_void};
use core::{mem, ptr};

type Serial = i32;

const ENOMEM: i32 = 12;
const EPERM: i32 = 1;
const EINVAL: i32 = 22;
const ENOKEY: i32 = 126;
const EKEYEXPIRED: i32 = 127;
const ENOSPC: i32 = 28;
const ENOENT: i32 = 2;
const ESRCH: i32 = 3;
const EIO: i32 = 5;
const EDEADLK: i32 = 35;

const KEY_SPEC_THREAD_KEYRING: Serial = -1;
const KEY_SPEC_PROCESS_KEYRING: Serial = -2;
const KEY_SPEC_SESSION_KEYRING: Serial = -3;
const KEY_SPEC_USER_KEYRING: Serial = -4;
const KEY_SPEC_USER_SESSION_KEYRING: Serial = -5;
const KEY_POS_ALL: u32 = 0x3f00_0000;
const KEY_USR_ALL: u32 = 0x003f_0000;
const KEYRING_MAX_OBJECTS: i32 = 4096;
const KEYRING_ITEM_CACHE_MAX: usize = 8;
const KEYRING_SEARCH_DEPTH: i32 = 4;
const CAP_SYS_ADMIN: i32 = 21;

const OBJECT_KEY: u32 = 1;
const OBJECT_KEYRING: u32 = 2;

#[repr(C)]
struct Object {
    next: *mut Object,
    kind: u32,
    serial: Serial,
    refs: i32,
    uid: i64,
    gid: i64,
    perm: u32,
    expiry_ns: u64,
    description: *mut u8,
    key_type: *mut u8,
    payload: *mut u8,
    payload_len: usize,
    first_item: *mut Item,
    last_item: *mut Item,
    item_count: usize,
    free_items: *mut Item,
    free_item_count: usize,
    graph_visit: u32,
    persistent_user: i32,
    owner_uid: u64,
}

#[repr(C)]
struct Item {
    next: *mut Item,
    object: *mut Object,
}

unsafe extern "C" {
    fn a20_keyring_current_task() -> *mut c_void;
    fn a20_keyring_current_cred_snapshot(
        euid_out: *mut i64,
        egid_out: *mut i64,
    ) -> *mut c_void;
    fn a20_keyring_task_uid(task: *mut c_void) -> i64;
    fn a20_keyring_task_euid(task: *mut c_void) -> i64;
    fn a20_keyring_task_gid(task: *mut c_void) -> i64;
    fn a20_keyring_task_has_cap(task: *mut c_void, cap: i32) -> i32;
    fn a20_keyring_session_get(task: *mut c_void) -> *mut c_void;
    fn a20_keyring_session_set(task: *mut c_void, ring: *mut c_void);
    fn a20_keyring_lock() -> u64;
    fn a20_keyring_lock_current(flags_out: *mut u64) -> *mut c_void;
    fn a20_keyring_unlock(flags: u64);
    fn a20_keyring_now_ns() -> u64;
    fn a20_keyring_alloc(size: usize) -> *mut c_void;
    fn a20_keyring_free(ptr: *mut c_void);
}

static mut SERIAL_MAP: *mut Object = ptr::null_mut();
static mut NEXT_SERIAL: Serial = 1;
static mut OBJECT_COUNT: i32 = 0;
static mut GRAPH_GENERATION: u32 = 0;
static mut GRAPH_STACK: [*mut Object; KEYRING_MAX_OBJECTS as usize] =
    [ptr::null_mut(); KEYRING_MAX_OBJECTS as usize];
#[cfg(test)]
static POOL_CACHE_RECLAIMED: core::sync::atomic::AtomicUsize =
    core::sync::atomic::AtomicUsize::new(0);

struct LockGuard(u64);

impl LockGuard {
    #[inline]
    unsafe fn acquire() -> Self {
        // The C helper keeps the architecture-specific IRQ state handling.
        Self(unsafe { a20_keyring_lock() })
    }

    #[inline]
    unsafe fn acquire_current() -> Option<(*mut c_void, Self)> {
        let mut flags = 0;
        let task = unsafe { a20_keyring_lock_current(&mut flags) };
        if task.is_null() {
            None
        } else {
            Some((task, Self(flags)))
        }
    }
}

impl Drop for LockGuard {
    fn drop(&mut self) {
        // Every call site holds this single subsystem lock for all map and
        // membership graph accesses.
        unsafe { a20_keyring_unlock(self.0) }
    }
}

unsafe fn alloc_zeroed<T>() -> *mut T {
    let raw = unsafe { a20_keyring_alloc(mem::size_of::<T>()) }.cast::<T>();
    if !raw.is_null() {
        unsafe { ptr::write_bytes(raw, 0, 1) };
    }
    raw
}

unsafe fn alloc_bytes(n: usize) -> *mut u8 {
    if n == 0 {
        return ptr::null_mut();
    }
    unsafe { a20_keyring_alloc(n) }.cast::<u8>()
}

unsafe fn free<T>(p: *mut T) {
    if !p.is_null() {
        unsafe { a20_keyring_free(p.cast()) };
    }
}

unsafe fn c_len(s: *const c_char) -> usize {
    if s.is_null() {
        return 0;
    }
    let mut n = 0usize;
    while unsafe { *s.add(n) } != 0 {
        n += 1;
    }
    n
}

unsafe fn c_dup(s: *const c_char) -> *mut u8 {
    if s.is_null() {
        return ptr::null_mut();
    }
    let len = unsafe { c_len(s) };
    let out = unsafe { alloc_bytes(len.saturating_add(1)) };
    if out.is_null() {
        return out;
    }
    unsafe { ptr::copy_nonoverlapping(s.cast::<u8>(), out, len + 1) };
    out
}

unsafe fn c_eq(a: *const u8, b: *const c_char) -> bool {
    if a.is_null() || b.is_null() {
        return false;
    }
    let mut i = 0usize;
    loop {
        let x = unsafe { *a.add(i) };
        let y = unsafe { *b.add(i) as u8 };
        if x != y {
            return false;
        }
        if x == 0 {
            return true;
        }
        i += 1;
    }
}

unsafe fn object_new(kind: u32, desc: *const c_char, uid: i64, gid: i64) -> *mut Object {
    let o = unsafe { alloc_zeroed::<Object>() };
    if o.is_null() {
        return o;
    }
    unsafe {
        (*o).kind = kind;
        (*o).uid = uid;
        (*o).gid = gid;
        (*o).perm = KEY_POS_ALL | KEY_USR_ALL;
        (*o).description = c_dup(desc);
    }
    if unsafe { (*o).description }.is_null() {
        unsafe { free(o) };
        return ptr::null_mut();
    }
    o
}

unsafe fn map_unlink(o: *mut Object) -> bool {
    let mut pp = ptr::addr_of_mut!(SERIAL_MAP);
    while !unsafe { *pp }.is_null() {
        if unsafe { *pp } == o {
            unsafe {
                *pp = (*o).next;
                (*o).next = ptr::null_mut();
                OBJECT_COUNT -= 1;
            }
            return true;
        }
        pp = unsafe { ptr::addr_of_mut!((**pp).next) };
    }
    false
}

unsafe fn object_put(o: *mut Object) {
    if o.is_null() {
        return;
    }
    let Some(refs) = (unsafe { (*o).refs }).checked_sub(1) else {
        return;
    };
    if refs < 0 {
        return;
    }
    unsafe { (*o).refs = refs };
    if unsafe { (*o).refs } > 0 {
        return;
    }
    unsafe { map_unlink(o) };
    unsafe { object_reclaim(o) };
}

unsafe fn object_get(o: *mut Object) -> bool {
    if o.is_null() {
        return false;
    }
    let Some(refs) = (unsafe { (*o).refs }).checked_add(1) else {
        return false;
    };
    if refs < 0 {
        return false;
    }
    unsafe { (*o).refs = refs };
    true
}

unsafe fn object_destroy(o: *mut Object) {
    if o.is_null() {
        return;
    }
    if unsafe { (*o).kind } == OBJECT_KEYRING {
        let mut item = unsafe { (*o).first_item };
        while !item.is_null() {
            let next = unsafe { (*item).next };
            unsafe {
                free(item);
            }
            item = next;
        }
        let mut cached = unsafe { (*o).free_items };
        while !cached.is_null() {
            let next = unsafe { (*cached).next };
            unsafe { free(cached) };
            #[cfg(test)]
            POOL_CACHE_RECLAIMED.fetch_add(1, core::sync::atomic::Ordering::Relaxed);
            cached = next;
        }
    } else {
        unsafe {
            free((*o).payload);
            free((*o).key_type);
        }
    }
    unsafe {
        free((*o).description);
        free(o);
    }
}

/* Detached item nodes carry no object reference and are protected by the
 * global keyring lock. A small per-ring cache avoids allocator churn for
 * repeated link/unlink pairs without growing unboundedly. */
unsafe fn take_cached_item(ring: *mut Object) -> *mut Item {
    let item = unsafe { (*ring).free_items };
    if !item.is_null() {
        unsafe {
            (*ring).free_items = (*item).next;
            (*ring).free_item_count -= 1;
            (*item).next = ptr::null_mut();
            (*item).object = ptr::null_mut();
        }
    }
    item
}

unsafe fn recycle_item(ring: *mut Object, item: *mut Item) {
    unsafe { (*item).object = ptr::null_mut() };
    if unsafe { (*ring).free_item_count } < KEYRING_ITEM_CACHE_MAX {
        unsafe {
            (*item).next = (*ring).free_items;
            (*ring).free_items = item;
            (*ring).free_item_count += 1;
        }
    } else {
        unsafe { free(item) };
    }
}

unsafe fn object_reclaim(root: *mut Object) {
    // Use the serial-map `next` link as an intrusive worklist after each
    // object has been removed from the map. A deep acyclic keyring graph thus
    // consumes constant kernel stack, even during final task teardown.
    let mut pending = root;
    while !pending.is_null() {
        let current = pending;
        pending = unsafe { (*current).next };
        unsafe { (*current).next = ptr::null_mut() };
        if unsafe { (*current).kind } == OBJECT_KEYRING {
            let mut item = unsafe { (*current).first_item };
            while !item.is_null() {
                let next_item = unsafe { (*item).next };
                let child = unsafe { (*item).object };
                if !child.is_null() {
                    let child_refs = unsafe { (*child).refs };
                    if child_refs >= 0 {
                        let refs = child_refs.saturating_sub(1);
                        unsafe { (*child).refs = refs };
                        if refs == 0 {
                            unsafe {
                                map_unlink(child);
                                (*child).next = pending;
                                pending = child;
                            }
                        }
                    }
                }
                unsafe { free(item) };
                item = next_item;
            }
            unsafe {
                (*current).first_item = ptr::null_mut();
                (*current).last_item = ptr::null_mut();
                (*current).item_count = 0;
            }
        }
        unsafe { object_destroy(current) };
    }
}

unsafe fn serial_exists(serial: Serial) -> bool {
    let mut o = unsafe { SERIAL_MAP };
    while !o.is_null() {
        if unsafe { (*o).serial } == serial {
            return true;
        }
        o = unsafe { (*o).next };
    }
    false
}

unsafe fn serial_alloc() -> Serial {
    if unsafe { OBJECT_COUNT } >= KEYRING_MAX_OBJECTS {
        return 0;
    }
    // Bound search by the number of occupied slots plus one. Serial values
    // wrap like Linux's positive key serial namespace without aliasing a live
    // object.
    let attempts = unsafe { OBJECT_COUNT }.saturating_add(1);
    for _ in 0..attempts {
        let candidate = unsafe { NEXT_SERIAL };
        unsafe {
            NEXT_SERIAL = NEXT_SERIAL.wrapping_add(1);
            if NEXT_SERIAL <= 0 {
                NEXT_SERIAL = 1;
            }
        }
        if candidate > 0 && !unsafe { serial_exists(candidate) } {
            return candidate;
        }
    }
    0
}

unsafe fn map_insert(o: *mut Object) {
    unsafe {
        (*o).next = SERIAL_MAP;
        SERIAL_MAP = o;
        OBJECT_COUNT += 1;
    }
}

unsafe fn lookup(serial: Serial) -> *mut Object {
    let mut o = unsafe { SERIAL_MAP };
    while !o.is_null() {
        if unsafe { (*o).serial } == serial {
            return o;
        }
        o = unsafe { (*o).next };
    }
    ptr::null_mut()
}

#[inline(always)]
unsafe fn ring_link(ring: *mut Object, o: *mut Object) -> i32 {
    if ring.is_null() || o.is_null() || unsafe { (*ring).kind } != OBJECT_KEYRING {
        return -EINVAL;
    }
    if unsafe { (*ring).serial } == unsafe { (*o).serial } {
        return -EINVAL;
    }
    let mut item = unsafe { (*ring).first_item };
    while !item.is_null() {
        if unsafe { (*item).object } == o {
            return 0;
        }
        item = unsafe { (*item).next };
    }
    if unsafe { (*o).kind } == OBJECT_KEYRING && unsafe { graph_reaches(o, ring) } {
        return -EDEADLK;
    }
    let mut reused = unsafe { take_cached_item(ring) };
    if reused.is_null() {
        reused = unsafe { alloc_zeroed::<Item>() };
    }
    let new_item = reused;
    if new_item.is_null() {
        return -ENOMEM;
    }
    if !unsafe { object_get(o) } {
        unsafe { recycle_item(ring, new_item) };
        return -ENOSPC;
    }
    unsafe {
        (*new_item).object = o;
        if (*ring).last_item.is_null() {
            (*ring).first_item = new_item;
        } else {
            (*(*ring).last_item).next = new_item;
        }
        (*ring).last_item = new_item;
        (*ring).item_count += 1;
    }
    0
}

unsafe fn graph_reaches(start: *mut Object, target: *mut Object) -> bool {
    if start == target {
        return true;
    }
    let mut generation = unsafe { GRAPH_GENERATION.wrapping_add(1) };
    if generation == 0 {
        let mut object = unsafe { SERIAL_MAP };
        while !object.is_null() {
            unsafe { (*object).graph_visit = 0 };
            object = unsafe { (*object).next };
        }
        generation = 1;
    }
    unsafe { GRAPH_GENERATION = generation };
    let mut count = 1usize;
    unsafe {
        (*start).graph_visit = generation;
        GRAPH_STACK[0] = start;
    }
    while count > 0 {
        count -= 1;
        let ring = unsafe { GRAPH_STACK[count] };
        let mut item = unsafe { (*ring).first_item };
        while !item.is_null() {
            let child = unsafe { (*item).object };
            if !child.is_null() && unsafe { (*child).kind } == OBJECT_KEYRING {
                if child == target {
                    return true;
                }
                if unsafe { (*child).graph_visit } != generation {
                    if count == KEYRING_MAX_OBJECTS as usize {
                        // Conservative failure protects the no-cycle
                        // invariant if map accounting is ever corrupted.
                        return true;
                    }
                    unsafe {
                        (*child).graph_visit = generation;
                        GRAPH_STACK[count] = child;
                    }
                    count += 1;
                }
            }
            item = unsafe { (*item).next };
        }
    }
    false
}

unsafe fn ring_unlink(ring: *mut Object, o: *mut Object) -> i32 {
    if ring.is_null() || o.is_null() || unsafe { (*ring).kind } != OBJECT_KEYRING {
        return -EINVAL;
    }
    let first = unsafe { (*ring).first_item };
    if !first.is_null() && unsafe { (*first).object == o && (*first).next.is_null() } {
        unsafe {
            (*ring).first_item = ptr::null_mut();
            (*ring).last_item = ptr::null_mut();
            (*ring).item_count -= 1;
            (*first).object = ptr::null_mut();
            (*first).next = ptr::null_mut();
            object_put(o);
            recycle_item(ring, first);
        }
        return 0;
    }
    let mut prev: *mut Item = ptr::null_mut();
    let mut item = unsafe { (*ring).first_item };
    while !item.is_null() {
        let next = unsafe { (*item).next };
        if unsafe { (*item).object } == o {
            unsafe {
                if prev.is_null() {
                    (*ring).first_item = next;
                } else {
                    (*prev).next = next;
                }
                if (*ring).last_item == item {
                    (*ring).last_item = prev;
                }
                (*ring).item_count -= 1;
                (*item).next = ptr::null_mut();
                (*item).object = ptr::null_mut();
                object_put(o);
                recycle_item(ring, item);
            }
            return 0;
        }
        prev = item;
        item = next;
    }
    -ENOENT
}

unsafe fn user_ring(uid: i64, gid: i64, create: bool) -> *mut Object {
    let mut o = unsafe { SERIAL_MAP };
    while !o.is_null() {
        if unsafe { (*o).kind == OBJECT_KEYRING
            && (*o).persistent_user != 0
            && (*o).owner_uid == uid as u64 }
        {
            return o;
        }
        o = unsafe { (*o).next };
    }
    if !create || uid < 0 {
        return ptr::null_mut();
    }
    let name = b"_uid\0";
    let ring = unsafe { object_new(OBJECT_KEYRING, name.as_ptr().cast(), uid, gid) };
    if ring.is_null() {
        return ring;
    }
    let serial = unsafe { serial_alloc() };
    if serial == 0 {
        unsafe { object_destroy(ring) };
        return ptr::null_mut();
    }
    unsafe {
        (*ring).serial = serial;
        (*ring).refs = 1;
        (*ring).persistent_user = 1;
        (*ring).owner_uid = uid as u64;
        map_insert(ring);
    }
    ring
}

unsafe fn session_ring(task: *mut c_void, create: bool) -> *mut Object {
    if task.is_null() {
        return ptr::null_mut();
    }
    let slot = unsafe { a20_keyring_session_get(task) }.cast::<Object>();
    if !slot.is_null() || !create {
        return slot;
    }
    let uid = unsafe { a20_keyring_task_uid(task) };
    let gid = unsafe { a20_keyring_task_gid(task) };
    let name = b"_ses\0";
    let ring = unsafe { object_new(OBJECT_KEYRING, name.as_ptr().cast(), uid, gid) };
    if ring.is_null() {
        return ring;
    }
    let serial = unsafe { serial_alloc() };
    if serial == 0 {
        unsafe { object_destroy(ring) };
        return ptr::null_mut();
    }
    unsafe {
        (*ring).serial = serial;
        (*ring).refs = 1;
        map_insert(ring);
        a20_keyring_session_set(task, ring.cast());
    }
    ring
}

#[inline(always)]
unsafe fn resolve_ring(task: *mut c_void, id: Serial, create: bool) -> *mut Object {
    match id {
        KEY_SPEC_THREAD_KEYRING | KEY_SPEC_PROCESS_KEYRING | KEY_SPEC_SESSION_KEYRING => {
            unsafe { session_ring(task, create) }
        }
        KEY_SPEC_USER_KEYRING | KEY_SPEC_USER_SESSION_KEYRING => {
            if task.is_null() {
                return ptr::null_mut();
            }
            unsafe {
                user_ring(
                    a20_keyring_task_uid(task),
                    a20_keyring_task_gid(task),
                    create,
                )
            }
        }
        _ if id > 0 => {
            let o = unsafe { lookup(id) };
            if !o.is_null() && unsafe { (*o).kind } == OBJECT_KEYRING {
                o
            } else {
                ptr::null_mut()
            }
        }
        _ => ptr::null_mut(),
    }
}

unsafe fn resolve_object(task: *mut c_void, id: Serial, create: bool) -> *mut Object {
    if id < 0 {
        unsafe { resolve_ring(task, id, create) }
    } else {
        unsafe { lookup(id) }
    }
}

unsafe fn is_expired(o: *mut Object, now: u64) -> bool {
    !o.is_null() && unsafe { (*o).expiry_ns != 0 && now >= (*o).expiry_ns }
}

unsafe fn find(
    ring: *mut Object,
    ty: *const c_char,
    desc: *const c_char,
    direct_only: bool,
    depth: i32,
    now: u64,
) -> *mut Object {
    if ring.is_null() || ty.is_null() || desc.is_null() || depth < 0 {
        return ptr::null_mut();
    }
    let mut item = unsafe { (*ring).first_item };
    while !item.is_null() {
        let o = unsafe { (*item).object };
        if !o.is_null() && !unsafe { is_expired(o, now) } {
            if unsafe { (*o).kind } == OBJECT_KEY {
                if unsafe { c_eq((*o).key_type, ty) && c_eq((*o).description, desc) } {
                    return o;
                }
            } else if !direct_only && depth > 0 {
                let nested = unsafe { find(o, ty, desc, false, depth - 1, now) };
                if !nested.is_null() {
                    return nested;
                }
            }
        }
        item = unsafe { (*item).next };
    }
    ptr::null_mut()
}

unsafe fn current_task() -> *mut c_void {
    unsafe { a20_keyring_current_task() }
}

unsafe fn has_admin(task: *mut c_void) -> bool {
    unsafe { a20_keyring_task_has_cap(task, CAP_SYS_ADMIN) != 0 }
}

unsafe fn attr_allowed(task: *mut c_void, o: *mut Object) -> bool {
    unsafe { has_admin(task) || (*o).uid == a20_keyring_task_euid(task) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_add_key(
    ty: *const c_char,
    desc: *const c_char,
    payload: *const c_void,
    plen: usize,
    ringid: Serial,
) -> Serial {
    let mut euid = -1;
    let mut egid = -1;
    let task = unsafe { a20_keyring_current_cred_snapshot(&mut euid, &mut egid) };
    if task.is_null() {
        return -ESRCH;
    }
    if ty.is_null() || desc.is_null() {
        return -EINVAL;
    }
    let key = unsafe {
        object_new(
            OBJECT_KEY,
            desc,
            euid,
            egid,
        )
    };
    if key.is_null() {
        return -ENOMEM;
    }
    unsafe {
        (*key).key_type = c_dup(ty);
        if (*key).key_type.is_null() {
            object_destroy(key);
            return -ENOMEM;
        }
        if plen > 0 {
            if payload.is_null() {
                object_destroy(key);
                return -EINVAL;
            }
            (*key).payload = alloc_bytes(plen);
            if (*key).payload.is_null() {
                object_destroy(key);
                return -ENOMEM;
            }
            ptr::copy_nonoverlapping(payload.cast::<u8>(), (*key).payload, plen);
            (*key).payload_len = plen;
        }
    }
    let now = unsafe { a20_keyring_now_ns() };
    let _guard = unsafe { LockGuard::acquire() };
    let ring = unsafe { resolve_ring(task, ringid, true) };
    if ring.is_null() {
        unsafe { object_destroy(key) };
        return -ENOKEY;
    }
    let existing = unsafe { find(ring, ty, desc, true, 0, now) };
    if !existing.is_null() {
        unsafe {
            free((*existing).payload);
            (*existing).payload = (*key).payload;
            (*key).payload = ptr::null_mut();
            (*existing).payload_len = (*key).payload_len;
            (*existing).uid = euid;
            (*existing).gid = egid;
            (*existing).expiry_ns = 0;
            let serial = (*existing).serial;
            object_destroy(key);
            return serial;
        }
    }
    let serial = unsafe { serial_alloc() };
    if serial == 0 {
        unsafe { object_destroy(key) };
        return -ENOSPC;
    }
    unsafe {
        (*key).serial = serial;
        map_insert(key);
        let ret = ring_link(ring, key);
        if ret < 0 {
            map_unlink(key);
            object_destroy(key);
            return ret;
        }
    }
    serial
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_request_key(
    ty: *const c_char,
    desc: *const c_char,
    dest_ringid: Serial,
) -> Serial {
    let (task, _guard) = match unsafe { LockGuard::acquire_current() } {
        Some(state) => state,
        None => return -ESRCH,
    };
    let now = unsafe { a20_keyring_now_ns() };
    let session = unsafe { resolve_ring(task, KEY_SPEC_SESSION_KEYRING, false) };
    let mut key = if session.is_null() {
        ptr::null_mut()
    } else {
        unsafe { find(session, ty, desc, false, KEYRING_SEARCH_DEPTH, now) }
    };
    if key.is_null() {
        let user = unsafe { resolve_ring(task, KEY_SPEC_USER_KEYRING, false) };
        if !user.is_null() {
            key = unsafe { find(user, ty, desc, false, 1, now) };
        }
    }
    if key.is_null() {
        return -ENOKEY;
    }
    if dest_ringid != 0 {
        let dest = unsafe { resolve_ring(task, dest_ringid, true) };
        if !dest.is_null() {
            let _ = unsafe { ring_link(dest, key) };
        }
    }
    unsafe { (*key).serial }
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_get_keyring_id(id: Serial, create: i32) -> Serial {
    let (task, _guard) = match unsafe { LockGuard::acquire_current() } {
        Some(state) => state,
        None => return -ESRCH,
    };
    let ring = unsafe { resolve_ring(task, id, create != 0) };
    if ring.is_null() { -ENOKEY } else { unsafe { (*ring).serial } }
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_join_session(name: *const c_char) -> Serial {
    let task = unsafe { current_task() };
    if task.is_null() {
        return -ESRCH;
    }
    let default_name = b"_ses\0";
    let name = if name.is_null() { default_name.as_ptr().cast() } else { name };
    let new_ring = unsafe {
        object_new(
            OBJECT_KEYRING,
            name,
            a20_keyring_task_uid(task),
            a20_keyring_task_gid(task),
        )
    };
    if new_ring.is_null() {
        return -ENOMEM;
    }
    let _guard = unsafe { LockGuard::acquire() };
    let serial = unsafe { serial_alloc() };
    if serial == 0 {
        unsafe { object_destroy(new_ring) };
        return -ENOSPC;
    }
    unsafe {
        (*new_ring).serial = serial;
        (*new_ring).refs = 1;
        map_insert(new_ring);
        let old = a20_keyring_session_get(task).cast::<Object>();
        a20_keyring_session_set(task, new_ring.cast());
        if !old.is_null() {
            // Keep the old ring serial-resolvable while a child or member
            // retains it; object_put unlinks it only on the final reference.
            object_put(old);
        }
    }
    serial
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_chown(id: Serial, uid: i32, gid: i32) -> i32 {
    let (task, _guard) = match unsafe { LockGuard::acquire_current() } {
        Some(state) => state,
        None => return -ESRCH,
    };
    let o = unsafe { resolve_object(task, id, false) };
    if o.is_null() {
        return -ENOKEY;
    }
    if !unsafe { attr_allowed(task, o) } {
        return -EPERM;
    }
    unsafe {
        if uid != -1 { (*o).uid = uid as i64; }
        if gid != -1 { (*o).gid = gid as i64; }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_setperm(id: Serial, perm: u32) -> i32 {
    let (task, _guard) = match unsafe { LockGuard::acquire_current() } {
        Some(state) => state,
        None => return -ESRCH,
    };
    let o = unsafe { resolve_object(task, id, false) };
    if o.is_null() { return -ENOKEY; }
    if !unsafe { attr_allowed(task, o) } { return -EPERM; }
    unsafe { (*o).perm = perm; }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_set_timeout(id: Serial, secs: usize) -> i32 {
    let (task, _guard) = match unsafe { LockGuard::acquire_current() } {
        Some(state) => state,
        None => return -ESRCH,
    };
    let o = unsafe { resolve_object(task, id, false) };
    if o.is_null() { return -ENOKEY; }
    if !unsafe { attr_allowed(task, o) } { return -EPERM; }
    unsafe {
        (*o).expiry_ns = if secs == 0 { 0 } else {
            a20_keyring_now_ns()
                .saturating_add((secs as u64).saturating_mul(1_000_000_000))
        };
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_link(id: Serial, ringid: Serial) -> i32 {
    let (task, _guard) = match unsafe { LockGuard::acquire_current() } {
        Some(state) => state,
        None => return -ESRCH,
    };
    let o = unsafe { resolve_object(task, id, true) };
    let ring = unsafe { resolve_ring(task, ringid, true) };
    if o.is_null() || ring.is_null() { return -ENOKEY; }
    unsafe { ring_link(ring, o) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_unlink(id: Serial, ringid: Serial) -> i32 {
    let (task, _guard) = match unsafe { LockGuard::acquire_current() } {
        Some(state) => state,
        None => return -ESRCH,
    };
    let o = unsafe { resolve_object(task, id, true) };
    let ring = unsafe { resolve_ring(task, ringid, true) };
    if o.is_null() || ring.is_null() { return -ENOKEY; }
    unsafe { ring_unlink(ring, o) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_search(
    ringid: Serial,
    ty: *const c_char,
    desc: *const c_char,
) -> Serial {
    let (task, _guard) = match unsafe { LockGuard::acquire_current() } {
        Some(state) => state,
        None => return -ESRCH,
    };
    let now = unsafe { a20_keyring_now_ns() };
    let ring = unsafe { resolve_ring(task, ringid, false) };
    let key = unsafe { find(ring, ty, desc, false, KEYRING_SEARCH_DEPTH, now) };
    if key.is_null() { -ENOKEY } else { unsafe { (*key).serial } }
}

fn write_byte(buf: *mut u8, cap: usize, used: &mut usize, b: u8) {
    if !buf.is_null() && *used < cap {
        unsafe { *buf.add(*used) = b };
    }
    *used += 1;
}

unsafe fn write_cstr(buf: *mut u8, cap: usize, used: &mut usize, s: *const u8) {
    if s.is_null() { return; }
    let mut i = 0usize;
    loop {
        let b = unsafe { *s.add(i) };
        if b == 0 { break; }
        write_byte(buf, cap, used, b);
        i += 1;
    }
}

fn write_i64(buf: *mut u8, cap: usize, used: &mut usize, value: i64) {
    let mut digits = [0u8; 20];
    let negative = value < 0;
    let mut n = if negative { value.wrapping_neg() as u64 } else { value as u64 };
    let mut count = 0usize;
    loop {
        digits[count] = b'0' + (n % 10) as u8;
        count += 1;
        n /= 10;
        if n == 0 { break; }
    }
    if negative { write_byte(buf, cap, used, b'-'); }
    while count > 0 {
        count -= 1;
        write_byte(buf, cap, used, digits[count]);
    }
}

fn write_hex32(buf: *mut u8, cap: usize, used: &mut usize, value: u32) {
    for shift in (0..8).rev() {
        let digit = ((value >> (shift * 4)) & 15) as u8;
        write_byte(buf, cap, used, if digit < 10 { b'0' + digit } else { b'a' + digit - 10 });
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_describe(
    id: Serial,
    buf: *mut c_char,
    bufsz: usize,
) -> isize {
    let (task, _guard) = match unsafe { LockGuard::acquire_current() } {
        Some(state) => state,
        None => return -(ESRCH as isize),
    };
    let o = unsafe { resolve_object(task, id, false) };
    if o.is_null() { return -(ENOKEY as isize); }
    let kind = if unsafe { (*o).kind } == OBJECT_KEY { unsafe { (*o).key_type } } else { b"keyring\0".as_ptr() };
    if buf.is_null() || bufsz == 0 {
        // Preserve the C helper's historical length-query behavior.  The
        // value assumes Linux uid/gid wire widths (sign plus ten digits),
        // while a rendered call returns snprintf's actual byte count.
        let kind_len = unsafe { c_len(kind.cast()) };
        let desc_len = unsafe { c_len((*o).description.cast()) };
        return (kind_len + 1 + 11 + 1 + 11 + 1 + 8 + 1 + desc_len) as isize;
    }
    let cap = if bufsz > 0 { bufsz - 1 } else { 0 };
    let out = buf.cast::<u8>();
    let mut used = 0usize;
    unsafe { write_cstr(out, cap, &mut used, kind); }
    write_byte(out, cap, &mut used, b';');
    unsafe { write_i64(out, cap, &mut used, (*o).uid); }
    write_byte(out, cap, &mut used, b';');
    unsafe { write_i64(out, cap, &mut used, (*o).gid); }
    write_byte(out, cap, &mut used, b';');
    unsafe { write_hex32(out, cap, &mut used, (*o).perm); }
    write_byte(out, cap, &mut used, b';');
    unsafe { write_cstr(out, cap, &mut used, (*o).description); }
    if !buf.is_null() && bufsz > 0 {
        unsafe { *buf.cast::<u8>().add(core::cmp::min(used, cap)) = 0; }
    }
    if used > isize::MAX as usize { -(EIO as isize) } else { used as isize }
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_read(id: Serial, buf: *mut c_void, bufsz: usize) -> isize {
    let (task, _guard) = match unsafe { LockGuard::acquire_current() } {
        Some(state) => state,
        None => return -(ESRCH as isize),
    };
    let now = unsafe { a20_keyring_now_ns() };
    let o = unsafe { resolve_object(task, id, false) };
    if o.is_null() { return -(ENOKEY as isize); }
    if unsafe { is_expired(o, now) } { return -(EKEYEXPIRED as isize); }
    if unsafe { (*o).kind } == OBJECT_KEY {
        let len = unsafe { (*o).payload_len };
        if !buf.is_null() && bufsz >= len && len > 0 {
            unsafe { ptr::copy_nonoverlapping((*o).payload, buf.cast::<u8>(), len); }
        }
        return len as isize;
    }
    let len = unsafe { (*o).item_count }.saturating_mul(mem::size_of::<Serial>());
    if !buf.is_null() && bufsz >= len {
        let mut item = unsafe { (*o).first_item };
        let mut i = 0usize;
        while !item.is_null() {
            unsafe {
                *buf.cast::<Serial>().add(i) = (*(*item).object).serial;
                item = (*item).next;
            }
            i += 1;
        }
    }
    len as isize
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_inherit(child: *mut c_void, parent: *mut c_void) {
    if child.is_null() || parent.is_null() { return; }
    let _guard = unsafe { LockGuard::acquire() };
    let ring = unsafe { a20_keyring_session_get(parent) }.cast::<Object>();
    if ring.is_null() { return; }
    if !unsafe { object_get(ring) } { return; }
    unsafe {
        a20_keyring_session_set(child, ring.cast());
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_keyring_release_task(task: *mut c_void) {
    if task.is_null() { return; }
    let _guard = unsafe { LockGuard::acquire() };
    let ring = unsafe { a20_keyring_session_get(task) }.cast::<Object>();
    if ring.is_null() { return; }
    unsafe {
        a20_keyring_session_set(task, ptr::null_mut());
        object_put(ring);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::alloc::{alloc, dealloc, Layout};
    use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};

    #[repr(C)]
    struct FakeTask {
        uid: i64,
        euid: i64,
        gid: i64,
        egid: i64,
        admin: i32,
        session: *mut c_void,
    }

    #[repr(C, align(8))]
    struct AllocationHeader {
        size: usize,
    }

    static NOW: AtomicU64 = AtomicU64::new(1_000_000_000);
    static ALLOC_COUNT: AtomicUsize = AtomicUsize::new(0);
    static FAIL_ALLOC_AT: AtomicUsize = AtomicUsize::new(0);
    static CURRENT: core::sync::atomic::AtomicPtr<c_void> =
        core::sync::atomic::AtomicPtr::new(ptr::null_mut());
    static FORCE_NO_CURRENT: core::sync::atomic::AtomicBool =
        core::sync::atomic::AtomicBool::new(false);
    static CRED_SNAPSHOT_CALLS: AtomicUsize = AtomicUsize::new(0);
    static CURRENT_LOCK_CALLS: AtomicUsize = AtomicUsize::new(0);
    static UNLOCK_CALLS: AtomicUsize = AtomicUsize::new(0);
    static mut TASK: FakeTask = FakeTask { uid: 1000, euid: 1000, gid: 1000, egid: 1000, admin: 0, session: ptr::null_mut() };

    #[no_mangle]
    extern "C" fn a20_keyring_current_task() -> *mut c_void {
        let selected = CURRENT.load(Ordering::Relaxed);
        if selected.is_null() { ptr::addr_of_mut!(TASK).cast() } else { selected }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_keyring_current_cred_snapshot(
        euid_out: *mut i64,
        egid_out: *mut i64,
    ) -> *mut c_void {
        CRED_SNAPSHOT_CALLS.fetch_add(1, Ordering::Relaxed);
        if FORCE_NO_CURRENT.load(Ordering::Relaxed)
            || euid_out.is_null()
            || egid_out.is_null()
        {
            return ptr::null_mut();
        }
        let task = a20_keyring_current_task();
        if task.is_null() {
            return ptr::null_mut();
        }
        unsafe {
            *euid_out = a20_keyring_task_euid(task);
            *egid_out = (*(task.cast::<FakeTask>())).egid;
        }
        task
    }
    #[no_mangle]
    unsafe extern "C" fn a20_keyring_task_uid(t: *mut c_void) -> i64 { unsafe { (*(t.cast::<FakeTask>())).uid } }
    #[no_mangle]
    unsafe extern "C" fn a20_keyring_task_euid(t: *mut c_void) -> i64 { unsafe { (*(t.cast::<FakeTask>())).euid } }
    #[no_mangle]
    unsafe extern "C" fn a20_keyring_task_gid(t: *mut c_void) -> i64 { unsafe { (*(t.cast::<FakeTask>())).gid } }
    #[no_mangle]
    unsafe extern "C" fn a20_keyring_task_has_cap(t: *mut c_void, _: i32) -> i32 { unsafe { (*(t.cast::<FakeTask>())).admin } }
    #[no_mangle]
    unsafe extern "C" fn a20_keyring_session_get(t: *mut c_void) -> *mut c_void { unsafe { (*(t.cast::<FakeTask>())).session } }
    #[no_mangle]
    unsafe extern "C" fn a20_keyring_session_set(t: *mut c_void, r: *mut c_void) { unsafe { (*(t.cast::<FakeTask>())).session = r; } }
    #[no_mangle]
    extern "C" fn a20_keyring_lock() -> u64 { 0 }
    #[no_mangle]
    unsafe extern "C" fn a20_keyring_lock_current(flags_out: *mut u64) -> *mut c_void {
        if FORCE_NO_CURRENT.load(Ordering::Relaxed) || flags_out.is_null() {
            return ptr::null_mut();
        }
        let task = a20_keyring_current_task();
        if task.is_null() {
            return ptr::null_mut();
        }
        CURRENT_LOCK_CALLS.fetch_add(1, Ordering::Relaxed);
        unsafe { *flags_out = 0 };
        task
    }
    #[no_mangle]
    extern "C" fn a20_keyring_unlock(_: u64) {
        UNLOCK_CALLS.fetch_add(1, Ordering::Relaxed);
    }
    #[no_mangle]
    extern "C" fn a20_keyring_now_ns() -> u64 { NOW.load(Ordering::Relaxed) }
    #[no_mangle]
    unsafe extern "C" fn a20_keyring_alloc(n: usize) -> *mut c_void {
        let ordinal = ALLOC_COUNT.fetch_add(1, Ordering::Relaxed) + 1;
        if FAIL_ALLOC_AT.load(Ordering::Relaxed) == ordinal { return ptr::null_mut(); }
        let size = n.max(1);
        let total = size + core::mem::size_of::<AllocationHeader>();
        let block = unsafe { alloc(Layout::from_size_align(total, 8).unwrap()) };
        if block.is_null() { return block.cast(); }
        unsafe { block.cast::<AllocationHeader>().write(AllocationHeader { size }); }
        unsafe { block.add(core::mem::size_of::<AllocationHeader>()).cast() }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_keyring_free(p: *mut c_void) {
        if !p.is_null() {
            let block = unsafe { p.cast::<u8>().sub(core::mem::size_of::<AllocationHeader>()) };
            let size = unsafe { (*block.cast::<AllocationHeader>()).size };
            let total = size + core::mem::size_of::<AllocationHeader>();
            unsafe { dealloc(block, Layout::from_size_align(total, 8).unwrap()) };
        }
    }

    #[test]
    fn actual_entrypoints_cover_keyring_contract() {
        unsafe {
            SERIAL_MAP = ptr::null_mut();
            NEXT_SERIAL = 1;
            OBJECT_COUNT = 0;
            GRAPH_GENERATION = 0;
            (*ptr::addr_of_mut!(TASK)).session = ptr::null_mut();
            (*ptr::addr_of_mut!(TASK)).uid = 1000;
            (*ptr::addr_of_mut!(TASK)).euid = 1000;
            (*ptr::addr_of_mut!(TASK)).gid = 1000;
            (*ptr::addr_of_mut!(TASK)).egid = 1000;
            (*ptr::addr_of_mut!(TASK)).admin = 0;
        }
        CURRENT_LOCK_CALLS.store(0, Ordering::Relaxed);
        UNLOCK_CALLS.store(0, Ordering::Relaxed);
        FORCE_NO_CURRENT.store(true, Ordering::Relaxed);
        assert_eq!(unsafe { a20_rust_keyring_search(1, b"user\0".as_ptr().cast(), b"missing\0".as_ptr().cast()) }, -ESRCH);
        assert_eq!(CURRENT_LOCK_CALLS.load(Ordering::Relaxed), 0);
        assert_eq!(UNLOCK_CALLS.load(Ordering::Relaxed), 0);
        FORCE_NO_CURRENT.store(false, Ordering::Relaxed);
        let ring = unsafe { a20_rust_keyring_get_keyring_id(KEY_SPEC_SESSION_KEYRING, 1) };
        assert!(ring > 0);
        assert_eq!(CURRENT_LOCK_CALLS.load(Ordering::Relaxed), 1);
        assert_eq!(UNLOCK_CALLS.load(Ordering::Relaxed), 1);
        assert_eq!(unsafe { a20_rust_keyring_search(i32::MAX, b"user\0".as_ptr().cast(), b"missing\0".as_ptr().cast()) }, -ENOKEY);
        assert_eq!(CURRENT_LOCK_CALLS.load(Ordering::Relaxed), 2);
        assert_eq!(UNLOCK_CALLS.load(Ordering::Relaxed), 2);
        let allocs_before_missing_current = ALLOC_COUNT.load(Ordering::Relaxed);
        CRED_SNAPSHOT_CALLS.store(0, Ordering::Relaxed);
        FORCE_NO_CURRENT.store(true, Ordering::Relaxed);
        assert_eq!(unsafe { a20_rust_keyring_add_key(b"user\0".as_ptr().cast(), b"no-task\0".as_ptr().cast(), ptr::null(), 0, ring) }, -ESRCH);
        assert_eq!(CRED_SNAPSHOT_CALLS.load(Ordering::Relaxed), 1);
        assert_eq!(ALLOC_COUNT.load(Ordering::Relaxed), allocs_before_missing_current);
        FORCE_NO_CURRENT.store(false, Ordering::Relaxed);
        let count_before_oom = unsafe { OBJECT_COUNT };
        ALLOC_COUNT.store(0, Ordering::Relaxed);
        FAIL_ALLOC_AT.store(2, Ordering::Relaxed); // description copy after object allocation
        assert_eq!(unsafe { a20_rust_keyring_add_key(b"user\0".as_ptr().cast(), b"oom-key\0".as_ptr().cast(), ptr::null(), 0, ring) }, -ENOMEM);
        FAIL_ALLOC_AT.store(0, Ordering::Relaxed);
        assert_eq!(unsafe { OBJECT_COUNT }, count_before_oom);
        let payload = b"abc";
        let key = unsafe { a20_rust_keyring_add_key(b"user\0".as_ptr().cast(), b"alpha\0".as_ptr().cast(), payload.as_ptr().cast(), 3, ring) };
        assert!(key > 0);
        unsafe {
            (*ptr::addr_of_mut!(TASK)).euid = 1201;
            (*ptr::addr_of_mut!(TASK)).egid = 1302;
        }
        assert_eq!(unsafe { a20_rust_keyring_add_key(b"user\0".as_ptr().cast(), b"alpha\0".as_ptr().cast(), payload.as_ptr().cast(), 3, ring) }, key);
        let mut captured_desc = [0i8; 128];
        let captured_len = unsafe { a20_rust_keyring_describe(key, captured_desc.as_mut_ptr(), captured_desc.len()) };
        assert!(captured_len > 0);
        assert_eq!(unsafe { std::ffi::CStr::from_ptr(captured_desc.as_ptr()) }.to_str().unwrap(), "user;1201;1302;3f3f0000;alpha");
        assert_eq!(unsafe { a20_rust_keyring_search(ring, b"user\0".as_ptr().cast(), b"alpha\0".as_ptr().cast()) }, key);
        assert_eq!(unsafe { a20_rust_keyring_request_key(b"user\0".as_ptr().cast(), b"alpha\0".as_ptr().cast(), 0) }, key);
        let mut read = [0u8; 8];
        assert_eq!(unsafe { a20_rust_keyring_read(key, read.as_mut_ptr().cast(), read.len()) }, 3);
        assert_eq!(&read[..3], payload);
        let mut desc = [0i8; 128];
        let described = unsafe { a20_rust_keyring_describe(key, desc.as_mut_ptr(), desc.len()) };
        assert!(described > 0);
        assert_eq!(unsafe { std::ffi::CStr::from_ptr(desc.as_ptr()) }.to_str().unwrap(), "user;1201;1302;3f3f0000;alpha");
        assert_eq!(unsafe { a20_rust_keyring_setperm(key, 0x1234) }, 0);
        unsafe { (*ptr::addr_of_mut!(TASK)).euid = 2000; }
        assert_eq!(unsafe { a20_rust_keyring_setperm(key, 0) }, -EPERM);
        unsafe { (*ptr::addr_of_mut!(TASK)).admin = 1; }
        assert_eq!(unsafe { a20_rust_keyring_chown(key, 2000, -1) }, 0);
        unsafe { (*ptr::addr_of_mut!(TASK)).admin = 0; (*ptr::addr_of_mut!(TASK)).euid = 2000; }
        // Timeout arithmetic saturates instead of wrapping into an already-
        // expired timestamp. Exercise multiplication and addition overflow,
        // and preserve the Linux zero-means-clear behavior.
        NOW.store(0, Ordering::Relaxed);
        assert_eq!(unsafe { a20_rust_keyring_set_timeout(key, usize::MAX) }, 0);
        assert_eq!(unsafe { (*lookup(key)).expiry_ns }, u64::MAX);
        assert_eq!(unsafe { a20_rust_keyring_read(key, read.as_mut_ptr().cast(), read.len()) }, 3);
        assert_eq!(unsafe { a20_rust_keyring_search(ring, b"user\0".as_ptr().cast(), b"alpha\0".as_ptr().cast()) }, key);
        NOW.store(u64::MAX - 500_000_000, Ordering::Relaxed);
        assert_eq!(unsafe { a20_rust_keyring_set_timeout(key, 1) }, 0);
        assert_eq!(unsafe { (*lookup(key)).expiry_ns }, u64::MAX);
        assert_eq!(unsafe { a20_rust_keyring_read(key, read.as_mut_ptr().cast(), read.len()) }, 3);
        assert_eq!(unsafe { a20_rust_keyring_search(ring, b"user\0".as_ptr().cast(), b"alpha\0".as_ptr().cast()) }, key);
        assert_eq!(unsafe { a20_rust_keyring_set_timeout(key, 0) }, 0);
        assert_eq!(unsafe { (*lookup(key)).expiry_ns }, 0);
        NOW.store(1_000_000_000, Ordering::Relaxed);
        let dest = unsafe { a20_rust_keyring_get_keyring_id(KEY_SPEC_USER_KEYRING, 1) };
        assert!(dest > 0);
        let key_obj = unsafe { lookup(key) };
        let old_refs = unsafe { (*key_obj).refs };
        FAIL_ALLOC_AT.store(ALLOC_COUNT.load(Ordering::Relaxed) + 1, Ordering::Relaxed);
        assert_eq!(unsafe { a20_rust_keyring_link(key, dest) }, -ENOMEM);
        FAIL_ALLOC_AT.store(0, Ordering::Relaxed);
        assert_eq!(unsafe { (*key_obj).refs }, old_refs);
        assert_eq!(unsafe { (*lookup(dest)).item_count }, 0);
        assert_eq!(unsafe { a20_rust_keyring_link(key, dest) }, 0);
        let mut serials = [0i32; 2];
        assert_eq!(unsafe { a20_rust_keyring_read(dest, serials.as_mut_ptr().cast(), 8) }, 4);
        assert_eq!(serials[0], key);
        assert_eq!(unsafe { a20_rust_keyring_unlink(key, dest) }, 0);
        let dest_obj = unsafe { lookup(dest) };
        assert!(unsafe { (*dest_obj).first_item.is_null() && (*dest_obj).last_item.is_null() });
        assert_eq!(unsafe { (*dest_obj).item_count }, 0);
        assert_eq!(unsafe { (*dest_obj).free_item_count }, 1);
        assert_eq!(unsafe { a20_rust_keyring_unlink(key, dest) }, -ENOENT);
        let allocs_after_warmup = ALLOC_COUNT.load(Ordering::Relaxed);
        for _ in 0..100 {
            assert_eq!(unsafe { a20_rust_keyring_link(key, dest) }, 0);
            assert_eq!(unsafe { a20_rust_keyring_unlink(key, dest) }, 0);
        }
        assert_eq!(ALLOC_COUNT.load(Ordering::Relaxed), allocs_after_warmup,
                   "warm link/unlink reuses cached item nodes");
        assert_eq!(unsafe { a20_rust_keyring_set_timeout(key, 1) }, 0);
        NOW.fetch_add(1_000_000_001, Ordering::Relaxed);
        assert_eq!(unsafe { a20_rust_keyring_read(key, read.as_mut_ptr().cast(), read.len()) }, -EKEYEXPIRED as isize);

        // Populate beyond the per-ring cache bound, drain it without
        // allocating, force an allocator failure when empty, and verify the
        // final session release frees all cached nodes.
        let pool_descs: [&[u8]; 9] = [
            b"pool-0\0", b"pool-1\0", b"pool-2\0", b"pool-3\0",
            b"pool-4\0", b"pool-5\0", b"pool-6\0", b"pool-7\0",
            b"pool-8\0",
        ];
        let mut pool_keys = [0i32; 9];
        for i in 0..pool_keys.len() {
            pool_keys[i] = unsafe {
                a20_rust_keyring_add_key(b"user\0".as_ptr().cast(),
                    pool_descs[i].as_ptr().cast(), payload.as_ptr().cast(),
                    payload.len(), ring)
            };
            assert!(pool_keys[i] > 0);
        }
        for serial in pool_keys {
            assert_eq!(unsafe { a20_rust_keyring_link(serial, dest) }, 0);
            assert_eq!(unsafe { a20_rust_keyring_unlink(serial, ring) }, 0);
        }
        let ring_obj = unsafe { lookup(ring) };
        assert_eq!(unsafe { (*ring_obj).free_item_count }, KEYRING_ITEM_CACHE_MAX);
        let allocs_before_reuse = ALLOC_COUNT.load(Ordering::Relaxed);
        for serial in &pool_keys[..KEYRING_ITEM_CACHE_MAX] {
            assert_eq!(unsafe { a20_rust_keyring_link(*serial, ring) }, 0);
        }
        assert_eq!(ALLOC_COUNT.load(Ordering::Relaxed), allocs_before_reuse,
                   "cached nodes satisfy links without allocation");
        FAIL_ALLOC_AT.store(ALLOC_COUNT.load(Ordering::Relaxed) + 1, Ordering::Relaxed);
        assert_eq!(unsafe { a20_rust_keyring_link(pool_keys[8], ring) }, -ENOMEM,
                   "empty cache propagates allocator failure");
        FAIL_ALLOC_AT.store(0, Ordering::Relaxed);
        assert_eq!(unsafe { (*ring_obj).item_count }, KEYRING_ITEM_CACHE_MAX + 1);
        for serial in &pool_keys[..KEYRING_ITEM_CACHE_MAX] {
            assert_eq!(unsafe { a20_rust_keyring_unlink(*serial, ring) }, 0);
        }
        for serial in pool_keys {
            assert_eq!(unsafe { a20_rust_keyring_unlink(serial, dest) }, 0);
        }

        let child = FakeTask { uid: 1000, euid: 1000, gid: 1000, egid: 1000, admin: 0, session: ptr::null_mut() };
        let child_ptr = (&child as *const FakeTask).cast_mut().cast::<c_void>();
        let parent_ptr = ptr::addr_of_mut!(TASK).cast::<c_void>();
        unsafe { a20_rust_keyring_inherit(child_ptr, parent_ptr); }
        assert_eq!(unsafe { a20_keyring_session_get(child_ptr) }, unsafe { a20_keyring_session_get(parent_ptr) });
        let reclaimed_before = POOL_CACHE_RECLAIMED.load(Ordering::Relaxed);
        unsafe { a20_rust_keyring_release_task(child_ptr); }
        unsafe { a20_rust_keyring_release_task(parent_ptr); }
        assert_eq!(POOL_CACHE_RECLAIMED.load(Ordering::Relaxed) - reclaimed_before,
                   KEYRING_ITEM_CACHE_MAX,
                   "final ring destruction reclaims cached item allocations");
        assert_eq!(unsafe { a20_rust_keyring_search(ring, b"user\0".as_ptr().cast(), b"alpha\0".as_ptr().cast()) }, -ENOKEY);

        // A fork child keeps the old session ring indexed after the parent
        // joins a replacement session. The final child release removes both
        // old ring and its unshared key from the serial map.
        unsafe {
            (*ptr::addr_of_mut!(TASK)).euid = 1000;
            (*ptr::addr_of_mut!(TASK)).uid = 1000;
        }
        let old_session = unsafe { a20_rust_keyring_get_keyring_id(KEY_SPEC_SESSION_KEYRING, 1) };
        let old_key = unsafe { a20_rust_keyring_add_key(b"user\0".as_ptr().cast(), b"child-owned\0".as_ptr().cast(), payload.as_ptr().cast(), 3, old_session) };
        assert!(old_key > 0);
        let child = FakeTask { uid: 1000, euid: 1000, gid: 1000, egid: 1000, admin: 0, session: ptr::null_mut() };
        let child_ptr = (&child as *const FakeTask).cast_mut().cast::<c_void>();
        let parent_ptr = ptr::addr_of_mut!(TASK).cast::<c_void>();
        let old_ring = unsafe { a20_keyring_session_get(parent_ptr).cast::<Object>() };
        let saved_refs = unsafe { (*old_ring).refs };
        unsafe { (*old_ring).refs = i32::MAX; }
        let rejected_child = FakeTask { uid: 1000, euid: 1000, gid: 1000, egid: 1000, admin: 0, session: ptr::null_mut() };
        let rejected_child_ptr = (&rejected_child as *const FakeTask).cast_mut().cast::<c_void>();
        unsafe { a20_rust_keyring_inherit(rejected_child_ptr, parent_ptr); }
        assert!(unsafe { a20_keyring_session_get(rejected_child_ptr) }.is_null());
        unsafe { (*old_ring).refs = saved_refs; }
        unsafe { a20_rust_keyring_inherit(child_ptr, parent_ptr); }
        let replacement = unsafe { a20_rust_keyring_join_session(b"replacement\0".as_ptr().cast()) };
        assert!(replacement > 0);
        assert_eq!(unsafe { lookup(old_session) }.is_null(), false);
        CURRENT.store(child_ptr, Ordering::Relaxed);
        assert_eq!(unsafe { a20_rust_keyring_search(KEY_SPEC_SESSION_KEYRING, b"user\0".as_ptr().cast(), b"child-owned\0".as_ptr().cast()) }, old_key);
        CURRENT.store(ptr::null_mut(), Ordering::Relaxed);
        unsafe { a20_rust_keyring_release_task(child_ptr); }
        assert!(unsafe { lookup(old_session) }.is_null());
        assert!(unsafe { lookup(old_key) }.is_null());
        unsafe { a20_rust_keyring_release_task(parent_ptr); }

        // Exercise a two-ring and a three-ring cycle, then build a diamond
        // where B and C both own A. A survives the first unlink and is
        // reclaimed only after its final parent reference disappears.
        let ring_a = unsafe { a20_rust_keyring_get_keyring_id(KEY_SPEC_SESSION_KEYRING, 1) };
        let ring_b = unsafe { a20_rust_keyring_get_keyring_id(KEY_SPEC_USER_KEYRING, 1) };
        let dag_key = unsafe { a20_rust_keyring_add_key(b"user\0".as_ptr().cast(), b"dag-key\0".as_ptr().cast(), payload.as_ptr().cast(), 3, ring_a) };
        assert_eq!(unsafe { a20_rust_keyring_link(ring_a, ring_b) }, 0);
        assert_eq!(unsafe { a20_rust_keyring_link(ring_b, ring_a) }, -EDEADLK);
        let ring_c = unsafe { a20_rust_keyring_join_session(b"ring-c\0".as_ptr().cast()) };
        assert!(ring_c > 0);
        assert_eq!(unsafe { a20_rust_keyring_link(ring_c, ring_b) }, 0); // B contains C
        assert_eq!(unsafe { a20_rust_keyring_link(ring_a, ring_c) }, 0); // C contains A
        assert_eq!(unsafe { a20_rust_keyring_unlink(ring_a, ring_b) }, 0);
        assert_eq!(unsafe { a20_rust_keyring_link(ring_b, ring_a) }, -EDEADLK); // A->B->C->A
        assert_eq!(unsafe { a20_rust_keyring_link(ring_a, ring_b) }, 0); // B also owns A
        let a_object = unsafe { lookup(ring_a) };
        let saved_refs = unsafe { (*a_object).refs };
        unsafe { (*a_object).refs = i32::MAX; }
        let ring_d = unsafe { a20_rust_keyring_join_session(b"ring-d\0".as_ptr().cast()) };
        assert!(ring_d > 0);
        assert_eq!(unsafe { a20_rust_keyring_link(ring_a, ring_d) }, -ENOSPC);
        unsafe { (*a_object).refs = saved_refs; }
        unsafe { a20_rust_keyring_release_task(parent_ptr); }
        let mut dag_read = [0u8; 8];
        assert!(!unsafe { lookup(ring_a) }.is_null());
        assert_eq!(unsafe { a20_rust_keyring_read(dag_key, dag_read.as_mut_ptr().cast(), 8) }, 3);
        assert_eq!(unsafe { a20_rust_keyring_unlink(ring_a, ring_b) }, 0);
        assert!(!unsafe { lookup(ring_a) }.is_null());
        assert_eq!(unsafe { a20_rust_keyring_read(dag_key, dag_read.as_mut_ptr().cast(), 8) }, 3);
        assert_eq!(unsafe { a20_rust_keyring_unlink(ring_a, ring_c) }, 0);
        assert!(unsafe { lookup(ring_a) }.is_null());
        assert!(unsafe { lookup(dag_key) }.is_null());
        assert_eq!(unsafe { a20_rust_keyring_unlink(ring_c, ring_b) }, 0);
        unsafe { a20_rust_keyring_release_task(parent_ptr); }

        // Final-put teardown of a deep acyclic chain is iterative, not one
        // kernel-stack frame per nested ring.
        {
            let _guard = unsafe { LockGuard::acquire() };
            let name = b"deep\0";
            let root = unsafe { object_new(OBJECT_KEYRING, name.as_ptr().cast(), 1000, 1000) };
            assert!(!root.is_null());
            unsafe {
                (*root).serial = serial_alloc();
                (*root).refs = 1;
                map_insert(root);
            }
            assert!(unsafe { (*root).serial } > 0);
            let mut previous = root;
            for _ in 1..2048 {
                let node = unsafe { object_new(OBJECT_KEYRING, name.as_ptr().cast(), 1000, 1000) };
                assert!(!node.is_null());
                let serial = unsafe { serial_alloc() };
                assert!(serial > 0);
                unsafe {
                    (*node).serial = serial;
                    map_insert(node);
                }
                assert_eq!(unsafe { ring_link(previous, node) }, 0);
                previous = node;
            }
            unsafe { object_put(root); }
            assert_eq!(unsafe { OBJECT_COUNT }, 1);
        }

        // The map has a hard object bound. Fill it exactly, prove the next
        // allocation fails with -ENOSPC, then release the session graph.
        let full_ring = unsafe { a20_rust_keyring_get_keyring_id(KEY_SPEC_SESSION_KEYRING, 1) };
        assert!(full_ring > 0);
        let mut index = 0usize;
        while unsafe { OBJECT_COUNT } < KEYRING_MAX_OBJECTS {
            let desc = std::ffi::CString::new(format!("limit-{index}")).unwrap();
            let serial = unsafe { a20_rust_keyring_add_key(b"user\0".as_ptr().cast(), desc.as_ptr(), ptr::null(), 0, full_ring) };
            assert!(serial > 0);
            index += 1;
        }
        assert_eq!(unsafe { OBJECT_COUNT }, KEYRING_MAX_OBJECTS);
        assert_eq!(unsafe { a20_rust_keyring_add_key(b"user\0".as_ptr().cast(), b"over-limit\0".as_ptr().cast(), ptr::null(), 0, full_ring) }, -ENOSPC);
        unsafe { a20_rust_keyring_release_task(parent_ptr); }
        assert_eq!(unsafe { OBJECT_COUNT }, 1); // persistent user ring only
    }
}
