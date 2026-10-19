#![no_std]

//! User-namespace id translation and namespace-scoped capability policy.
//!
//! Rust owns the parts of a user namespace that decide what a task may do:
//! the extent lookup in both directions, the "is this global id mapped here?"
//! test, the capability-scoping rule that every namespace permission check
//! funnels through, the exception that lets an unprivileged writer install a
//! single-id map, and the validation plus append of a new map.
//!
//! C keeps the namespace objects themselves: allocation, the reference count,
//! the registry list, the per-namespace lock, the `/proc/<pid>/{uid,gid}_map`
//! wire parsing, and the task/mount membership changes.
//!
//! The maps live in the C `user_namespace_t`, so Rust reads and writes them
//! through borrowed pointers and the caller's `ns->lock`; no namespace state is
//! duplicated here.

#[cfg(test)]
extern crate std;

use core::ffi::c_void;

const EINVAL: i32 = 22;
const EPERM: i32 = 1;
const MAP_MAX: i32 = 5;

/// One contiguous run of a map, mirroring `uid_gid_extent_t`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Extent {
    pub lower: u32,
    pub parent_lower: u32,
    pub count: u32,
}

unsafe extern "C" {
    fn a20_userns_ns_parent(ns: *const c_void) -> *const c_void;
    fn a20_userns_ns_level(ns: *const c_void) -> i32;
}

#[inline]
unsafe fn extent(map: *const Extent, index: i32) -> Extent {
    unsafe { core::ptr::read(map.add(index as usize)) }
}

#[inline]
fn covers(lower: u32, count: u32, id: u32) -> bool {
    // Subtraction, not addition: a range that reaches the top of the 32-bit
    // space must not wrap into a match.
    id >= lower && id.wrapping_sub(lower) < count
}

fn lookup_lower(map: *const Extent, extents: i32, id: u32) -> i32 {
    for i in 0..extents {
        let e = unsafe { extent(map, i) };
        if covers(e.lower, e.count, id) {
            return i;
        }
    }
    -1
}

fn lookup_parent(map: *const Extent, extents: i32, id: u32) -> i32 {
    for i in 0..extents {
        let e = unsafe { extent(map, i) };
        if covers(e.parent_lower, e.count, id) {
            return i;
        }
    }
    -1
}

/// Translate a namespace-relative id to a global id.  Returns -1 when the id is
/// not representable, and then leaves `out` untouched.
///
/// # Safety
/// `map` must point to `extents` valid `Extent`s (or `extents` may be 0) and
/// `out` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_userns_map_to_global(
    map: *const Extent,
    extents: i32,
    ns_id: i32,
    out: *mut u32,
) -> i32 {
    if ns_id < 0 || extents < 0 || (extents > 0 && map.is_null()) || out.is_null() {
        return -1;
    }
    let id = ns_id as u32;
    let i = lookup_lower(map, extents, id);
    if i < 0 {
        return -1;
    }
    let e = unsafe { extent(map, i) };
    // The lookup matched on the namespace side, so this cannot wrap.
    unsafe { core::ptr::write(out, e.parent_lower.wrapping_add(id.wrapping_sub(e.lower))) };
    0
}

/// Translate a global id to a namespace-relative id.  Returns -1 when the id is
/// not mapped, and then leaves `out` untouched.
///
/// # Safety
/// See [`a20_userns_map_to_global`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_userns_map_from_global(
    map: *const Extent,
    extents: i32,
    global: u32,
    out: *mut u32,
) -> i32 {
    if extents < 0 || (extents > 0 && map.is_null()) || out.is_null() {
        return -1;
    }
    let i = lookup_parent(map, extents, global);
    if i < 0 {
        return -1;
    }
    let e = unsafe { extent(map, i) };
    unsafe { core::ptr::write(out, e.lower.wrapping_add(global.wrapping_sub(e.parent_lower))) };
    0
}

/// Nonzero when `global` is representable in this namespace.
///
/// # Safety
/// See [`a20_userns_map_to_global`] for the map requirements.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_userns_map_resolves(
    map: *const Extent,
    extents: i32,
    global: u32,
) -> i32 {
    if extents < 0 || (extents > 0 && map.is_null()) {
        return 0;
    }
    (lookup_parent(map, extents, global) >= 0) as i32
}

/// May a task with capability word `cap_effective` exercise `cap` with respect
/// to `target`?
///
/// The rule is a single walk from `target` up the parent chain: authority is
/// granted only when the walk lands exactly on `own`, the task's own
/// namespace, where `cap_effective` decides.  Running off the top without
/// landing is the containment property — a task holds nothing in a namespace
/// above its own, whatever it created below it.  The level test stops a
/// malformed chain from turning this into a spin; levels strictly decrease on
/// the way up, so it is also the loop's exit.
///
/// # Safety
/// `target` and `own` must be null or point to live namespaces.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_userns_capable(
    target: *const c_void,
    own: *const c_void,
    cap: i32,
    cap_effective: u64,
) -> i32 {
    if target.is_null() || own.is_null() {
        return 0;
    }
    if cap < 0 || cap >= 64 {
        return 0;
    }
    let own_level = unsafe { a20_userns_ns_level(own) };
    let mut p = target;
    while !p.is_null() {
        if p == own {
            return ((cap_effective >> cap) & 1) as i32;
        }
        if unsafe { a20_userns_ns_level(p) } <= own_level {
            return 0;
        }
        p = unsafe { a20_userns_ns_parent(p) };
    }
    0
}

/// May an unprivileged writer install this flat extent list?
///
/// The narrow case is exactly one extent of length one that maps the writer's
/// own id to namespace id 0.  Anything wider would hand the writer ids it did
/// not already hold.
///
/// # Safety
/// `extents` must point to `count * 3` readable `u32`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_userns_unprivileged_map_allowed(
    extents: *const u32,
    count: i32,
    own_id: u32,
) -> i32 {
    if extents.is_null() {
        return 0;
    }
    if count != 1 {
        return 0;
    }
    let mut flat = [0u32; 3];
    for (i, slot) in flat.iter_mut().enumerate() {
        *slot = unsafe { core::ptr::read(extents.add(i)) };
    }
    if flat[2] != 1 || flat[1] != own_id {
        return 0;
    }
    // `lower` is the id inside the namespace; any value is harmless for a
    // single-id map because it cannot reach another id.
    1
}

/// Validate a flat extent list against the parent namespace's map.
///
/// Returns 0, `-EINVAL` for a representation error, or `-EPERM` when an extent
/// is not representable in the parent.  Checks run extent by extent, so the
/// first failing extent decides the code.
///
/// # Safety
/// `extents` must point to `count * 3` readable `u32`s and `parent_map` to
/// `parent_extents` valid `Extent`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_userns_validate_extents(
    extents: *const u32,
    count: i32,
    parent_map: *const Extent,
    parent_extents: i32,
) -> i32 {
    if extents.is_null() || count <= 0 {
        return -EINVAL;
    }
    for i in 0..count {
        let base = (i * 3) as usize;
        let lower = unsafe { core::ptr::read(extents.add(base)) };
        let plower = unsafe { core::ptr::read(extents.add(base + 1)) };
        let cnt = unsafe { core::ptr::read(extents.add(base + 2)) };
        if cnt == 0 {
            return -EINVAL;
        }
        // A range that runs off the end would let lower + count wrap and alias
        // ids that were never granted.
        if cnt - 1 > u32::MAX - lower {
            return -EINVAL;
        }
        if cnt - 1 > u32::MAX - plower {
            return -EINVAL;
        }
        let mut resolved = 0u32;
        if unsafe { a20_userns_map_to_global(parent_map, parent_extents, plower as i32, &mut resolved) } < 0 {
            return -EPERM;
        }
    }
    0
}

/// Append a validated flat extent list to a namespace map, refusing overlap.
///
/// Extents are appended in increasing `lower` order and must not overlap, which
/// is what keeps lookup a plain scan with no reordering.  `map_extents` is
/// updated in place only when the whole list was appended.
///
/// # Safety
/// The caller must hold the namespace lock.  `map` must point to
/// `max_extents` writable `Extent`s, `map_extents` to its live count, and
/// `extents` to `count * 3` readable `u32`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_userns_append_map(
    map: *mut Extent,
    map_extents: *mut i32,
    max_extents: i32,
    extents: *const u32,
    count: i32,
) -> i32 {
    if map.is_null() || map_extents.is_null() || extents.is_null() || count <= 0 {
        return -EINVAL;
    }
    if count > max_extents {
        return -EINVAL;
    }
    let nextents = unsafe { core::ptr::read(map_extents) };
    if nextents < 0 || nextents + count > max_extents {
        return -EINVAL;
    }

    let mut highest: u32 = 0;
    let mut have = false;
    for i in 0..nextents {
        let e = unsafe { extent(map, i) };
        let top = e.lower.wrapping_add(e.count);
        if !have || top > highest {
            highest = top;
            have = true;
        }
    }

    // C computed `lower + cnt` in 32 bits and then compared it against
    // 0xFFFFFFFF, a comparison no 32-bit value can satisfy.  The range check
    // above leaves `lower + cnt == 0x100000000` as the only interesting case,
    // and that wraps to 0 in C; reproduce the wrapped value so the overlap test
    // behaves the same.
    for i in 0..count {
        let base = (i * 3) as usize;
        let lower = unsafe { core::ptr::read(extents.add(base)) };
        let cnt = unsafe { core::ptr::read(extents.add(base + 2)) };
        if have && lower < highest {
            return -EINVAL;
        }
        highest = lower.wrapping_add(cnt);
        have = true;
    }

    for i in 0..count {
        let base = (i * 3) as usize;
        let e = Extent {
            lower: unsafe { core::ptr::read(extents.add(base)) },
            parent_lower: unsafe { core::ptr::read(extents.add(base + 1)) },
            count: unsafe { core::ptr::read(extents.add(base + 2)) },
        };
        unsafe { core::ptr::write(map.add((nextents + i) as usize), e) };
    }
    unsafe { core::ptr::write(map_extents, nextents + count) };
    0
}

/// Compile-time check that the C extent mirror is what Rust thinks it is.
const _: () = assert!(core::mem::size_of::<Extent>() == 12);
const _: () = assert!(MAP_MAX == 5);

#[cfg(test)]
mod tests {
    use super::*;

    /// Nesting depth used by the capability-scoping fixture.
    #[repr(C)]
    struct FakeNs {
        level: i32,
        parent: *const FakeNs,
    }

    fn ns(level: i32, parent: *const FakeNs) -> FakeNs {
        FakeNs { level, parent }
    }

    /// Test stubs for the two C namespace accessors.
    #[unsafe(no_mangle)]
    extern "C" fn a20_userns_ns_parent(ns: *const c_void) -> *const c_void {
        unsafe { (*(ns as *const FakeNs)).parent as *const c_void }
    }

    #[unsafe(no_mangle)]
    extern "C" fn a20_userns_ns_level(ns: *const c_void) -> i32 {
        unsafe { (*(ns as *const FakeNs)).level }
    }

    const IDMAP: [Extent; 2] = [
        Extent { lower: 0, parent_lower: 1000, count: 1 },
        Extent { lower: 10, parent_lower: 2000, count: 5 },
    ];

    #[test]
    fn a_namespace_id_translates_through_its_extent() {
        let mut out = 0u32;
        assert_eq!(unsafe { a20_userns_map_to_global(IDMAP.as_ptr(), 2, 0, &mut out) }, 0);
        assert_eq!(out, 1000);
        assert_eq!(unsafe { a20_userns_map_to_global(IDMAP.as_ptr(), 2, 13, &mut out) }, 0);
        assert_eq!(out, 2003);
    }

    #[test]
    fn an_unmapped_namespace_id_reports_failure_without_writing() {
        let mut out = 0xdead_beefu32;
        assert_eq!(unsafe { a20_userns_map_to_global(IDMAP.as_ptr(), 2, 9, &mut out) }, -1);
        assert_eq!(unsafe { a20_userns_map_to_global(IDMAP.as_ptr(), 2, -1, &mut out) }, -1);
        assert_eq!(unsafe { a20_userns_map_to_global(IDMAP.as_ptr(), 2, 15, &mut out) }, -1);
        assert_eq!(out, 0xdead_beef);
    }

    #[test]
    fn a_global_id_translates_back_through_the_parent_range() {
        let mut out = 0u32;
        assert_eq!(unsafe { a20_userns_map_from_global(IDMAP.as_ptr(), 2, 1000, &mut out) }, 0);
        assert_eq!(out, 0);
        assert_eq!(unsafe { a20_userns_map_from_global(IDMAP.as_ptr(), 2, 2004, &mut out) }, 0);
        assert_eq!(out, 14);
        assert_eq!(unsafe { a20_userns_map_from_global(IDMAP.as_ptr(), 2, 1001, &mut out) }, -1);
        assert_eq!(out, 14, "a failed lookup leaves the output alone");
    }

    #[test]
    fn resolving_distinguishes_mapped_from_unmapped() {
        assert_eq!(unsafe { a20_userns_map_resolves(IDMAP.as_ptr(), 2, 1000) }, 1);
        assert_eq!(unsafe { a20_userns_map_resolves(IDMAP.as_ptr(), 2, 1999) }, 0);
        assert_eq!(unsafe { a20_userns_map_resolves(IDMAP.as_ptr(), 0, 1000) }, 0);
    }

    #[test]
    fn a_full_range_extent_covers_the_top_of_the_space() {
        let full = [Extent { lower: 0, parent_lower: 0, count: u32::MAX }];
        let mut out = 0u32;
        assert_eq!(unsafe { a20_userns_map_to_global(full.as_ptr(), 1, 0, &mut out) }, 0);
        assert_eq!(out, 0);
        assert_eq!(unsafe { a20_userns_map_to_global(full.as_ptr(), 1, 0x7fffffff, &mut out) }, 0);
        assert_eq!(out, 0x7fffffff);
    }

    #[test]
    fn capability_scoping_lands_on_the_tasks_own_namespace() {
        let own = ns(2, core::ptr::null());
        let child = ns(3, &own as *const FakeNs);
        let grandchild = ns(4, &child as *const FakeNs);
        // Constant 16 is one bit; using it directly keeps the test readable.
        const CAP: i32 = 16;
        assert_eq!(unsafe { a20_userns_capable(&child as *const FakeNs as *const c_void,
                                               &child as *const FakeNs as *const c_void,
                                               CAP, 1u64 << 16) }, 1);
        assert_eq!(unsafe { a20_userns_capable(&grandchild as *const FakeNs as *const c_void,
                                               &child as *const FakeNs as *const c_void,
                                               CAP, 0) }, 0);
    }

    #[test]
    fn capability_scoping_refuses_namespaces_above_the_tasks_own() {
        let parent = ns(1, core::ptr::null());
        let own = ns(2, &parent as *const FakeNs);
        let child = ns(3, &own as *const FakeNs);
        // The task lives in `own`; it must hold nothing in `parent`, even
        // though `own` descends from it.
        assert_eq!(unsafe { a20_userns_capable(&parent as *const FakeNs as *const c_void,
                                               &own as *const FakeNs as *const c_void,
                                               7, u64::MAX) }, 0);
        // Deliberately choosing the effective set to contain every bit: the
        // refusal above is structural, not a missing capability.
        assert_eq!(unsafe { a20_userns_capable(&child as *const FakeNs as *const c_void,
                                               &own as *const FakeNs as *const c_void,
                                               7, u64::MAX) }, 1);
    }

    #[test]
    fn capability_scoping_rejects_bad_arguments() {
        let own = ns(1, core::ptr::null());
        let p = &own as *const FakeNs as *const c_void;
        assert_eq!(unsafe { a20_userns_capable(core::ptr::null(), p, 1, u64::MAX) }, 0);
        assert_eq!(unsafe { a20_userns_capable(p, core::ptr::null(), 1, u64::MAX) }, 0);
        assert_eq!(unsafe { a20_userns_capable(p, p, -1, u64::MAX) }, 0);
        assert_eq!(unsafe { a20_userns_capable(p, p, 64, u64::MAX) }, 0);
    }

    #[test]
    fn unprivileged_rule_accepts_only_the_single_own_id_map() {
        let ok = [0u32, 1000, 1];
        assert_eq!(unsafe { a20_userns_unprivileged_map_allowed(ok.as_ptr(), 1, 1000) }, 1);
        let other = [0u32, 1001, 1];
        assert_eq!(unsafe { a20_userns_unprivileged_map_allowed(other.as_ptr(), 1, 1000) }, 0);
        let wide = [0u32, 1000, 2];
        assert_eq!(unsafe { a20_userns_unprivileged_map_allowed(wide.as_ptr(), 1, 1000) }, 0);
        let two = [0u32, 1000, 1, 5, 1001, 1];
        assert_eq!(unsafe { a20_userns_unprivileged_map_allowed(two.as_ptr(), 2, 1000) }, 0);
    }

    #[test]
    fn validation_accepts_extents_inside_the_parent_lower_range() {
        // The initial namespace maps the whole space, which is the parent a
        // container's map is normally validated against.
        let parent = [Extent { lower: 0, parent_lower: 0, count: u32::MAX }];
        let ok = [0u32, 1000, 4];
        assert_eq!(unsafe { a20_userns_validate_extents(ok.as_ptr(), 1, parent.as_ptr(), 1) }, 0);

        let zero = [0u32, 1000, 0];
        assert_eq!(unsafe { a20_userns_validate_extents(zero.as_ptr(), 1, parent.as_ptr(), 1) }, -EINVAL);
        // `count - 1 > u32::MAX - lower` catches a run that would wrap at the
        // top of the 32-bit space.
        let wraps = [u32::MAX, 1000, 2];
        assert_eq!(unsafe { a20_userns_validate_extents(wraps.as_ptr(), 1, parent.as_ptr(), 1) }, -EINVAL);
        let wraps_plower = [0u32, u32::MAX, 2];
        assert_eq!(unsafe { a20_userns_validate_extents(wraps_plower.as_ptr(), 1, parent.as_ptr(), 1) }, -EINVAL);
        // The second extent is checked only after the first passes.
        let late = [0u32, 1000, 1, 9, u32::MAX, 2];
        assert_eq!(unsafe { a20_userns_validate_extents(late.as_ptr(), 2, parent.as_ptr(), 1) }, -EINVAL);
    }

    #[test]
    fn validation_measures_the_parent_range_on_the_namespace_side() {
        // The check resolves the parent-side id through the parent's
        // namespace-side `lower` range, so a parent whose lower range does not
        // start at 0 refuses an id it cannot address there.  This mirrors the
        // C body exactly; the initial namespace's full range is what makes the
        // common case permissive.
        let parent = [Extent { lower: 1000, parent_lower: 1000, count: 5 }];
        let inside = [0u32, 1002, 1];
        assert_eq!(unsafe { a20_userns_validate_extents(inside.as_ptr(), 1, parent.as_ptr(), 1) }, 0);
        let below = [0u32, 999, 1];
        assert_eq!(unsafe { a20_userns_validate_extents(below.as_ptr(), 1, parent.as_ptr(), 1) }, -EPERM);
        let above = [0u32, 1005, 1];
        assert_eq!(unsafe { a20_userns_validate_extents(above.as_ptr(), 1, parent.as_ptr(), 1) }, -EPERM);
    }

    #[test]
    fn append_installs_the_list_and_updates_the_count() {
        let mut map = [Extent { lower: 0, parent_lower: 0, count: 0 }; MAP_MAX as usize];
        let mut extents = 0i32;
        let first = [0u32, 1000, 1];
        assert_eq!(unsafe { a20_userns_append_map(map.as_mut_ptr(), &mut extents, MAP_MAX,
                                                  first.as_ptr(), 1) }, 0);
        assert_eq!(extents, 1);
        assert_eq!(map[0].lower, 0);
        assert_eq!(map[0].parent_lower, 1000);

        let second = [10u32, 2000, 5];
        assert_eq!(unsafe { a20_userns_append_map(map.as_mut_ptr(), &mut extents, MAP_MAX,
                                                  second.as_ptr(), 1) }, 0);
        assert_eq!(extents, 2);
        assert_eq!(map[1].count, 5);
    }

    #[test]
    fn append_refuses_overlap_and_leaves_the_map_untouched() {
        let mut map = [Extent { lower: 0, parent_lower: 0, count: 0 }; MAP_MAX as usize];
        let mut extents = 0i32;
        let first = [10u32, 2000, 5];
        assert_eq!(unsafe { a20_userns_append_map(map.as_mut_ptr(), &mut extents, MAP_MAX,
                                                  first.as_ptr(), 1) }, 0);
        // lower 9 is below the existing extent's top, so it would overlap.
        let overlap = [9u32, 3000, 6];
        assert_eq!(unsafe { a20_userns_append_map(map.as_mut_ptr(), &mut extents, MAP_MAX,
                                                  overlap.as_ptr(), 1) }, -EINVAL);
        assert_eq!(extents, 1);
        assert_eq!(map[1].lower, 0, "nothing was written past the live count");

        // A contiguous extent starting exactly at the top is fine.
        let adjacent = [15u32, 4000, 1];
        assert_eq!(unsafe { a20_userns_append_map(map.as_mut_ptr(), &mut extents, MAP_MAX,
                                                  adjacent.as_ptr(), 1) }, 0);
        assert_eq!(extents, 2);
    }

    #[test]
    fn append_stops_at_the_namespace_map_limit() {
        let mut map = [Extent { lower: 0, parent_lower: 0, count: 0 }; MAP_MAX as usize];
        let mut extents = 0i32;
        for i in 0..MAP_MAX {
            let list = [(i * 10) as u32, 0u32, 1u32];
            assert_eq!(unsafe { a20_userns_append_map(map.as_mut_ptr(), &mut extents, MAP_MAX,
                                                      list.as_ptr(), 1) }, 0);
        }
        let extra = [100u32, 0, 1];
        assert_eq!(unsafe { a20_userns_append_map(map.as_mut_ptr(), &mut extents, MAP_MAX,
                                                  extra.as_ptr(), 1) }, -EINVAL);
        assert_eq!(extents, MAP_MAX);
    }
}
