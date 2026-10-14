#![no_std]

//! Linux epoll interest-table state transitions.
//!
//! The C adapter owns the epoll lock, vfile references, wait queues and poll
//! calls. This module owns the protected registration table transitions. The
//! lock is held by the caller across every call that reads or mutates an item.

const EPOLL_MAX_FDS: usize = 1024;
const READINESS_MODE_EDGE: u32 = 1;
const READINESS_MODE_ONESHOT: u32 = 2;
const EEXIST: i32 = 17;
const ENOENT: i32 = 2;
const ENOSPC: i32 = 28;

#[repr(C)]
#[derive(Clone, Copy)]
struct Event {
    events: u32,
    data: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ReadinessState {
    mode: u32,
    active: i16,
    generation: u64,
    enabled: bool,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EpollItem {
    fd: i32,
    registered: i32,
    identity: u64,
    ev: Event,
    state: ReadinessState,
}

const _: [(); 16] = [(); core::mem::size_of::<Event>()];
const _: [(); 24] = [(); core::mem::size_of::<ReadinessState>()];
const _: [(); 56] = [(); core::mem::size_of::<EpollItem>()];

fn mode_from_events(events: u32) -> u32 {
    let mut mode = 0;
    if events & (1 << 31) != 0 {
        mode |= READINESS_MODE_EDGE;
    }
    if events & (1 << 30) != 0 {
        mode |= READINESS_MODE_ONESHOT;
    }
    mode
}

/// Apply epoll LT/ET/ONESHOT delivery semantics to one raw readiness sample.
///
/// This callback runs from readiness_scan before max_ready is counted, so a
/// filtered ET duplicate cannot consume the caller's ready-event budget and
/// starve a later registration. The `state` pointer is a private wait-snapshot
/// copy; the C adapter commits it only if the registration identity is still
/// current after the wait.
#[no_mangle]
pub unsafe extern "C" fn a20_eventpoll_filter_ready_rust(
    raw: i16,
    generation: u64,
    state: *mut core::ffi::c_void,
) -> i16 {
    if state.is_null() {
        return raw;
    }
    // SAFETY: the adapter passes the ReadinessState snapshot for this item.
    let state = unsafe { &mut *(state as *mut ReadinessState) };
    if !state.enabled {
        return 0;
    }
    if raw == 0 {
        state.active = 0;
        state.generation = generation;
        return 0;
    }

    let deliver = state.mode & READINESS_MODE_EDGE == 0
        || state.active == 0
        || state.generation != generation;
    state.active = raw;
    state.generation = generation;
    if !deliver {
        return 0;
    }
    if state.mode & READINESS_MODE_ONESHOT != 0 {
        state.enabled = false;
    }
    raw
}

unsafe fn item_at<'a>(items: *mut EpollItem, index: usize) -> Option<&'a mut EpollItem> {
    if items.is_null() || index >= EPOLL_MAX_FDS {
        return None;
    }
    // SAFETY: the caller supplies a live 1024-entry table under its lock.
    unsafe { items.add(index).as_mut() }
}

#[no_mangle]
/// Find a registration by local descriptor number and open-file identity.
/// Caller holds the epoll table lock.
pub unsafe extern "C" fn a20_eventpoll_find_rust(
    items: *const EpollItem,
    fd: i32,
    identity: u64,
) -> i32 {
    if items.is_null() {
        return -1;
    }
    for index in 0..EPOLL_MAX_FDS {
        // SAFETY: the caller supplies a live 1024-entry table under its lock.
        let item = unsafe { &*items.add(index) };
        if item.registered != 0 && item.fd == fd && item.identity == identity {
            return index as i32;
        }
    }
    -1
}

#[no_mangle]
pub unsafe extern "C" fn a20_eventpoll_find_free_rust(items: *const EpollItem) -> i32 {
    if items.is_null() {
        return -1;
    }
    for index in 0..EPOLL_MAX_FDS {
        // SAFETY: the caller supplies a live 1024-entry table under its lock.
        if unsafe { (&*items.add(index)).registered == 0 } {
            return index as i32;
        }
    }
    -1
}

#[no_mangle]
/// Add a registration keyed by descriptor number plus open-file identity.
/// A reused number with a different live open-file description gets a distinct
/// slot; an identical pair reports EEXIST.
pub unsafe extern "C" fn a20_eventpoll_add_rust(
    items: *mut EpollItem,
    count: *mut i32,
    fd: i32,
    identity: u64,
    events: u32,
    data: u64,
) -> i32 {
    if items.is_null() || count.is_null() {
        return -ENOSPC;
    }
    if unsafe { a20_eventpoll_find_rust(items, fd, identity) } >= 0 {
        return -EEXIST;
    }
    let free = unsafe { a20_eventpoll_find_free_rust(items) };
    if free < 0 {
        return -ENOSPC;
    }
    let index = free as usize;
    let Some(item) = (unsafe { item_at(items, index) }) else {
        return -ENOSPC;
    };
    item.fd = fd;
    item.identity = identity;
    item.ev = Event { events, data };
    item.state = ReadinessState {
        mode: mode_from_events(events),
        active: 0,
        generation: 0,
        enabled: true,
    };
    item.registered = 1;
    unsafe { *count += 1 };
    0
}

#[no_mangle]
/// Modify and rearm a registration only when descriptor and file identity
/// still name the same open file description.
pub unsafe extern "C" fn a20_eventpoll_mod_rust(
    items: *mut EpollItem,
    fd: i32,
    identity: u64,
    events: u32,
    data: u64,
) -> i32 {
    let index = unsafe { a20_eventpoll_find_rust(items, fd, identity) };
    if index < 0 {
        return -ENOENT;
    }
    let Some(item) = (unsafe { item_at(items, index as usize) }) else {
        return -ENOENT;
    };
    item.ev = Event { events, data };
    item.state = ReadinessState {
        mode: mode_from_events(events),
        active: 0,
        generation: 0,
        enabled: true,
    };
    0
}

#[no_mangle]
/// Delete a registration only when descriptor and identity still match.
pub unsafe extern "C" fn a20_eventpoll_del_rust(
    items: *mut EpollItem,
    count: *mut i32,
    fd: i32,
    identity: u64,
) -> i32 {
    let index = unsafe { a20_eventpoll_find_rust(items, fd, identity) };
    if index < 0 {
        return -ENOENT;
    }
    unsafe {
        a20_eventpoll_del_index_rust(items, count, index as usize, fd, identity)
    }
}

#[no_mangle]
/// Delete the indexed registration only if it still matches the caller's
/// descriptor and open-file identity. The C adapter may use an index it found
/// while holding the same table lock, avoiding a second linear table search;
/// the Rust check remains the state-transition decision and rejects stale
/// indices without changing the table or count.
pub unsafe extern "C" fn a20_eventpoll_del_index_rust(
    items: *mut EpollItem,
    count: *mut i32,
    index: usize,
    fd: i32,
    identity: u64,
) -> i32 {
    let Some(item) = (unsafe { item_at(items, index) }) else {
        return -ENOENT;
    };
    if item.registered == 0 || item.fd != fd || item.identity != identity {
        return -ENOENT;
    }
    item.registered = 0;
    item.fd = -1;
    item.identity = 0;
    if !count.is_null() {
        unsafe { *count -= 1 };
    }
    0
}

#[no_mangle]
/// Commit readiness state from one wait snapshot if it still describes the
/// same registration. A POLLNVAL result prunes that exact stale registration.
pub unsafe extern "C" fn a20_eventpoll_commit_rust(
    items: *mut EpollItem,
    count: *mut i32,
    index: usize,
    fd: i32,
    identity: u64,
    state: *const ReadinessState,
    invalid: bool,
) -> bool {
    let Some(item) = (unsafe { item_at(items, index) }) else {
        return false;
    };
    if item.registered == 0 || item.fd != fd || item.identity != identity {
        return false;
    }
    if invalid {
        item.registered = 0;
        item.fd = -1;
        item.identity = 0;
        if !count.is_null() {
            unsafe { *count -= 1 };
        }
        return true;
    }
    if !state.is_null() {
        // SAFETY: caller provides a valid readiness-state snapshot.
        item.state = unsafe { *state };
    }
    false
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;

    fn table() -> [EpollItem; EPOLL_MAX_FDS] {
        [EpollItem {
            fd: -1,
            registered: 0,
            identity: 0,
            ev: Event { events: 0, data: 0 },
            state: ReadinessState {
                mode: 0,
                active: 0,
                generation: 0,
                enabled: false,
            },
        }; EPOLL_MAX_FDS]
    }

    #[test]
    fn same_number_reused_for_another_open_description_keeps_both_watches() {
        let mut items = table();
        let mut count = 0;
        unsafe {
            assert_eq!(a20_eventpoll_add_rust(items.as_mut_ptr(), &mut count, 9, 44, 1, 7), 0);
            assert_eq!(count, 1);
            assert_eq!(a20_eventpoll_add_rust(items.as_mut_ptr(), &mut count, 9, 44, 1, 7), -EEXIST);
            assert_eq!(a20_eventpoll_add_rust(items.as_mut_ptr(), &mut count, 9, 45, 1, 8), 0);
            assert_eq!(count, 2);
            assert_eq!(a20_eventpoll_find_rust(items.as_ptr(), 9, 44), 0);
            assert_eq!(a20_eventpoll_find_rust(items.as_ptr(), 9, 45), 1);
            assert_eq!(a20_eventpoll_del_rust(items.as_mut_ptr(), &mut count, 9, 44), 0);
            assert_eq!(count, 1);
            assert_eq!(a20_eventpoll_del_rust(items.as_mut_ptr(), &mut count, 9, 45), 0);
            assert_eq!(count, 0);
        }
    }

    #[test]
    fn indexed_delete_validates_slot_identity_before_mutating() {
        let mut items = table();
        let mut count = 0;
        unsafe {
            assert_eq!(a20_eventpoll_add_rust(items.as_mut_ptr(), &mut count, 6, 70, 1, 10), 0);
            assert_eq!(a20_eventpoll_add_rust(items.as_mut_ptr(), &mut count, 6, 71, 1, 11), 0);
            assert_eq!(count, 2);
            assert_eq!(a20_eventpoll_del_index_rust(items.as_mut_ptr(), &mut count, 0, 6, 71), -ENOENT);
            assert_eq!(count, 2);
            assert_eq!(items[0].registered, 1);
            assert_eq!(a20_eventpoll_del_index_rust(items.as_mut_ptr(), &mut count, EPOLL_MAX_FDS, 6, 70), -ENOENT);
            assert_eq!(count, 2);
            assert_eq!(a20_eventpoll_del_index_rust(items.as_mut_ptr(), &mut count, 0, 6, 70), 0);
            assert_eq!(count, 1);
            assert_eq!(items[0].registered, 0);
            assert_eq!(items[1].identity, 71);
        }
    }

    #[test]
    fn mod_rearms_edge_oneshot_and_rejects_reused_fd() {
        let mut items = table();
        let mut count = 0;
        unsafe {
            assert_eq!(a20_eventpoll_add_rust(items.as_mut_ptr(), &mut count, 3, 100, (1 << 31) | (1 << 30), 4), 0);
            let old_generation = 77;
            items[0].state.active = 1;
            items[0].state.generation = old_generation;
            items[0].state.enabled = false;
            assert_eq!(a20_eventpoll_mod_rust(items.as_mut_ptr(), 3, 99, 1, 5), -ENOENT);
            assert_eq!(a20_eventpoll_mod_rust(items.as_mut_ptr(), 3, 100, 1 << 31, 5), 0);
            assert_eq!(items[0].state.mode, READINESS_MODE_EDGE);
            assert_eq!(items[0].state.active, 0);
            assert!(items[0].state.enabled);
            assert_eq!(items[0].state.generation, 0);
            assert_eq!(items[0].ev.data, 5);
        }
    }

    #[test]
    fn stale_wait_snapshot_prunes_only_the_closed_identity_after_number_reuse() {
        let mut items = table();
        let mut count = 0;
        unsafe {
            assert_eq!(a20_eventpoll_add_rust(items.as_mut_ptr(), &mut count, 4, 20, 1, 0), 0);
            let saved = items[0].state;
            assert_eq!(a20_eventpoll_add_rust(items.as_mut_ptr(), &mut count, 4, 21, 1, 0), 0);
            assert!(a20_eventpoll_commit_rust(items.as_mut_ptr(), &mut count, 0, 4, 20, &saved, true));
            assert_eq!(count, 1);
            assert_eq!(items[0].registered, 0);
            assert_eq!(items[1].identity, 21);
        }
    }

    #[test]
    fn filter_owns_lt_et_oneshot_and_generation_transitions() {
        let mut state = ReadinessState {
            mode: 0,
            active: 0,
            generation: 0,
            enabled: true,
        };
        unsafe {
            assert_eq!(a20_eventpoll_filter_ready_rust(1, 3, &mut state as *mut _ as *mut _), 1);
            assert_eq!(a20_eventpoll_filter_ready_rust(1, 3, &mut state as *mut _ as *mut _), 1);
            assert_eq!(a20_eventpoll_filter_ready_rust(0, 3, &mut state as *mut _ as *mut _), 0);
            assert_eq!(a20_eventpoll_filter_ready_rust(1, 3, &mut state as *mut _ as *mut _), 1);

            state.mode = READINESS_MODE_EDGE;
            state.active = 0;
            assert_eq!(a20_eventpoll_filter_ready_rust(1, 4, &mut state as *mut _ as *mut _), 1);
            assert_eq!(a20_eventpoll_filter_ready_rust(1, 4, &mut state as *mut _ as *mut _), 0);
            assert_eq!(a20_eventpoll_filter_ready_rust(1, 5, &mut state as *mut _ as *mut _), 1);

            state.mode = READINESS_MODE_ONESHOT;
            state.active = 0;
            state.enabled = true;
            assert_eq!(a20_eventpoll_filter_ready_rust(1, 6, &mut state as *mut _ as *mut _), 1);
            assert!(!state.enabled);
            assert_eq!(a20_eventpoll_filter_ready_rust(1, 7, &mut state as *mut _ as *mut _), 0);
        }
    }

    #[test]
    fn filtered_edge_duplicate_does_not_change_generation() {
        let mut state = ReadinessState {
            mode: READINESS_MODE_EDGE,
            active: 1,
            generation: 11,
            enabled: true,
        };
        unsafe {
            assert_eq!(a20_eventpoll_filter_ready_rust(1, 11, &mut state as *mut _ as *mut _), 0);
            assert_eq!(state.generation, 11);
            assert_eq!(a20_eventpoll_filter_ready_rust(1, 12, &mut state as *mut _ as *mut _), 1);
            assert_eq!(state.generation, 12);
        }
    }
}
