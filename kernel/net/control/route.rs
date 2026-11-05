#![no_std]
//! Bounded FIB and ordered lookup rules. All algorithms operate on owned value
//! arrays; only the FFI shell touches shared storage, under an IRQ-safe guard.
//! No reference into the FIB survives a transaction or backend call.
use core::cell::UnsafeCell;
use core::ptr;
use core::sync::atomic::{AtomicBool, Ordering};

#[cfg(a20_net_embedded)]
const ROUTES: usize = 8;
#[cfg(not(a20_net_embedded))]
const ROUTES: usize = 64;
#[cfg(a20_net_embedded)]
const RULES: usize = 4;
#[cfg(not(a20_net_embedded))]
const RULES: usize = 16;
const MAIN: u32 = 254;
const EINVAL: i32 = 22;
const ENOSPC: i32 = 28;
const ESRCH: i32 = 3;
const EEXIST: i32 = 17;
const REPLACE: u32 = 0x100;
const MATCH_METRIC: u32 = 1 << 16;

pub trait Address: Copy + Eq {
    const ZERO: Self;
    const BITS: u8;
    fn network(self, prefix: u8) -> Self;
}

impl Address for u32 {
    const ZERO: Self = 0;
    const BITS: u8 = 32;
    fn network(self, prefix: u8) -> Self {
        self & mask(prefix)
    }
}

#[cfg(a20_net_policy_v6)]
impl Address for [u8; 16] {
    const ZERO: Self = [0; 16];
    const BITS: u8 = 128;
    fn network(mut self, prefix: u8) -> Self {
        let bytes = usize::from(prefix / 8);
        let bits = prefix % 8;
        if bits == 0 {
            self[bytes..].fill(0);
        } else {
            self[bytes] &= 0xff << (8 - bits);
            self[bytes + 1..].fill(0);
        }
        self
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct RouteEntry<A> {
    destination: A,
    gateway: A,
    ifindex: u32,
    table: u32,
    metric: u32,
    prefix: u8,
    pad: [u8; 3],
}
impl<A: Address> RouteEntry<A> {
    const EMPTY: Self = Self {
        destination: A::ZERO,
        gateway: A::ZERO,
        ifindex: 0,
        table: 0,
        metric: 0,
        prefix: 0,
        pad: [0; 3],
    };
}

#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct RuleEntry<A> {
    source: A,
    destination: A,
    priority: u32,
    table: u32,
    mark: u32,
    mark_mask: u32,
    source_prefix: u8,
    destination_prefix: u8,
    pad: [u8; 2],
}
impl<A: Address> RuleEntry<A> {
    const EMPTY: Self = Self {
        source: A::ZERO,
        destination: A::ZERO,
        priority: 0,
        table: 0,
        mark: 0,
        mark_mask: 0,
        source_prefix: 0,
        destination_prefix: 0,
        pad: [0; 2],
    };
    fn matches(&self, src: A, dst: A, mark: u32) -> bool {
        src.network(self.source_prefix) == self.source
            && dst.network(self.destination_prefix) == self.destination
            && (mark & self.mark_mask) == (self.mark & self.mark_mask)
    }
}

type Route = RouteEntry<u32>;
type Rule = RuleEntry<u32>;

/* Addresses are u32 containers for network-order bytes, not host integers.
 * Masking the high native bits corrupted prefixes on little-endian targets. */
fn mask(prefix: u8) -> u32 {
    if prefix == 0 {
        0
    } else {
        (u32::MAX << (32 - prefix)).to_be()
    }
}

#[derive(Clone, Copy)]
struct Fib<A> {
    routes: [RouteEntry<A>; ROUTES],
    rules: [RuleEntry<A>; RULES],
}
impl<A: Address> Fib<A> {
    const fn new() -> Self {
        Self {
            routes: [RouteEntry::EMPTY; ROUTES],
            rules: [RuleEntry::EMPTY; RULES],
        }
    }
    fn route(&mut self, mut r: RouteEntry<A>, remove: bool, flags: u32) -> i32 {
        if r.prefix > A::BITS || r.table == 0 || (!remove && r.ifindex == 0) {
            return -EINVAL;
        }
        r.destination = r.destination.network(r.prefix);
        let existing = self.routes.iter().position(|e| {
            e.table == r.table
                && e.prefix == r.prefix
                && e.destination == r.destination
                && ((remove && flags & MATCH_METRIC == 0) || e.metric == r.metric)
                && (!remove || r.ifindex == 0 || e.ifindex == r.ifindex)
                && (!remove || r.gateway == A::ZERO || e.gateway == r.gateway)
        });
        if remove {
            return match existing {
                Some(i) => {
                    self.routes[i] = RouteEntry::EMPTY;
                    0
                }
                None => -ESRCH,
            };
        }
        let slot = match existing {
            Some(i) if flags & REPLACE != 0 => i,
            Some(_) => return -EEXIST,
            None => match self.routes.iter().position(|e| e.table == 0) {
                Some(i) => i,
                None => return -ENOSPC,
            },
        };
        self.routes[slot] = r;
        0
    }
    fn rule(&mut self, mut r: RuleEntry<A>, remove: bool) -> i32 {
        if r.table == 0 || r.source_prefix > A::BITS || r.destination_prefix > A::BITS {
            return -EINVAL;
        }
        r.source = r.source.network(r.source_prefix);
        r.destination = r.destination.network(r.destination_prefix);
        if remove {
            return match self.rules.iter().position(|e| *e == r) {
                Some(i) => {
                    self.rules[i] = RuleEntry::EMPTY;
                    0
                }
                None => -ESRCH,
            };
        }
        if self.rules.contains(&r) {
            return -EEXIST;
        }
        match self.rules.iter_mut().find(|e| e.table == 0) {
            Some(e) => {
                *e = r;
                0
            }
            None => -ENOSPC,
        }
    }
    fn lookup_table(&self, dst: A, table: u32) -> Option<RouteEntry<A>> {
        self.routes
            .iter()
            .filter(|r| r.table == table && dst.network(r.prefix) == r.destination)
            .min_by_key(|r| (A::BITS - r.prefix, r.metric))
            .copied()
    }
    fn matching_tables(&self, src: A, dst: A, mark: u32, out: &mut [u32]) -> usize {
        /* Fixed-size selection avoids allocation/sorting on the packet path. */
        let mut visited = 0u32;
        let mut count = 0;
        while count < out.len() {
            let selected = self.rules.iter().enumerate()
                .filter(|(i, r)| visited & (1 << i) == 0 && r.table != 0 && r.matches(src, dst, mark))
                .min_by_key(|(i, r)| (r.priority, *i));
            let Some((i, rule)) = selected else { break };
            visited |= 1 << i;
            if !out[..count].contains(&rule.table) {
                out[count] = rule.table;
                count += 1;
            }
        }
        count
    }

    fn lookup(&self, src: A, dst: A, mark: u32) -> Option<RouteEntry<A>> {
        let mut tables = [0u32; RULES];
        let count = self.matching_tables(src, dst, mark, &mut tables);
        for table in &tables[..count] {
            if let Some(route) = self.lookup_table(dst, *table) {
                return Some(route);
            }
        }
        self.lookup_table(dst, MAIN)
    }

    fn best_prefix(&self, route: RouteEntry<A>) -> RouteEntry<A> {
        let dst = route.destination.network(route.prefix);
        self.routes.iter()
            .filter(|r| r.table == route.table && r.prefix == route.prefix && r.destination == dst)
            .min_by_key(|r| r.metric).copied().unwrap_or(RouteEntry::EMPTY)
    }

    fn flush_if(&mut self, ifindex: u32) {
        for r in &mut self.routes {
            if r.ifindex == ifindex {
                *r = RouteEntry::EMPTY;
            }
        }
    }
}

unsafe extern "C" {
    fn a20_net_route_lock() -> u64;
    fn a20_net_route_unlock(flags: u64);
}
struct Guard(u64);
impl Guard {
    fn acquire() -> Self {
        Self(unsafe { a20_net_route_lock() })
    }
}
impl Drop for Guard {
    fn drop(&mut self) {
        unsafe { a20_net_route_unlock(self.0) }
    }
}
struct SharedFib<A>(UnsafeCell<Fib<A>>);
// SAFETY: every FFI access holds Guard; no reference or pointer is returned.
unsafe impl<A: Address> Sync for SharedFib<A> {}
static HAVE_RULES: AtomicBool = AtomicBool::new(false);
static FIB: SharedFib<u32> = SharedFib(UnsafeCell::new(Fib::new()));

#[repr(C)]
pub struct RouteChange<A> {
    before: RouteEntry<A>,
    after: RouteEntry<A>,
    table_used: u32,
}

// A backend executor serializes policy route writers. Preflight leaves
// shared state intact while the native RIB operation can still fail.
#[no_mangle]
pub unsafe extern "C" fn a20_net_route_plan_v4(
    route: *const Route, remove: i32, flags: u32, out: *mut RouteChange<u32>,
) -> i32 {
    unsafe { plan(&FIB, route, remove, flags, out) }
}

unsafe fn plan<A: Address>(
    storage: &SharedFib<A>, route: *const RouteEntry<A>, remove: i32, flags: u32,
    out: *mut RouteChange<A>,
) -> i32 {
    if route.is_null() || out.is_null() {
        return -EINVAL;
    }
    let value = unsafe { ptr::read(route) };
    let _guard = Guard::acquire();
    let fib = unsafe { &*storage.0.get() };
    let mut next = *fib;
    let result = next.route(value, remove != 0, flags);
    if result == 0 {
        unsafe {
            *out = RouteChange {
                before: fib.best_prefix(value),
                after: next.best_prefix(value),
                table_used: next.routes.iter().any(|r| r.table == value.table) as u32,
            };
        }
    }
    result
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_route_tables_v4(
    src: u32, dst: u32, mark: u32, out: *mut u32, cap: u32,
) -> u32 {
    if out.is_null() || cap == 0 || !HAVE_RULES.load(Ordering::Acquire) {
        return 0;
    }
    let tables = unsafe { core::slice::from_raw_parts_mut(out, (cap as usize).min(RULES)) };
    let _guard = Guard::acquire();
    unsafe { (&*FIB.0.get()).matching_tables(src, dst, mark, tables) as u32 }
}

unsafe fn update_route<A: Address>(
    storage: &SharedFib<A>, r: *const RouteEntry<A>, remove: i32, flags: u32,
) -> i32 {
    if r.is_null() {
        return -EINVAL;
    }
    let value = unsafe { ptr::read(r) };
    let _guard = Guard::acquire();
    unsafe { (&mut *storage.0.get()).route(value, remove != 0, flags) }
}

unsafe fn update_rule<A: Address>(
    storage: &SharedFib<A>, have_rules: &AtomicBool, r: *const RuleEntry<A>, remove: i32,
) -> i32 {
    if r.is_null() {
        return -EINVAL;
    }
    let value = unsafe { ptr::read(r) };
    let _guard = Guard::acquire();
    let fib = unsafe { &mut *storage.0.get() };
    let rc = fib.rule(value, remove != 0);
    if rc == 0 {
        have_rules.store(remove == 0 || fib.rules.iter().any(|r| r.table != 0), Ordering::Release);
    }
    rc
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_route_update_v4(r: *const Route, remove: i32, flags: u32) -> i32 {
    unsafe { update_route(&FIB, r, remove, flags) }
}
#[no_mangle]
pub unsafe extern "C" fn a20_net_rule_update_v4(r: *const Rule, remove: i32) -> i32 {
    unsafe { update_rule(&FIB, &HAVE_RULES, r, remove) }
}
#[no_mangle]
pub unsafe extern "C" fn a20_net_route_query_v4(
    src: u32,
    dst: u32,
    mark: u32,
    out: *mut Route,
) -> i32 {
    if out.is_null() {
        return -EINVAL;
    }
    let _guard = Guard::acquire();
    match unsafe { (&*FIB.0.get()).lookup(src, dst, mark) } {
        Some(route) => {
            unsafe { *out = route };
            1
        }
        None => 0,
    }
}
unsafe fn snapshot<A: Address, T: Copy>(
    storage: &SharedFib<A>, out: *mut T, cap: u32,
    entries: impl FnOnce(&Fib<A>) -> &[T], table: impl Fn(&T) -> u32,
) -> u32 {
    if out.is_null() {
        return 0;
    }
    let _guard = Guard::acquire();
    let fib = unsafe { &*storage.0.get() };
    let mut n = 0;
    for entry in entries(fib).iter().filter(|e| table(e) != 0).take(cap as usize) {
        unsafe { *out.add(n) = *entry };
        n += 1;
    }
    n as u32
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_route_snapshot_v4(out: *mut Route, cap: u32) -> u32 {
    unsafe { snapshot(&FIB, out, cap, |f| &f.routes, |r| r.table) }
}
#[no_mangle]
pub unsafe extern "C" fn a20_net_rule_snapshot_v4(out: *mut Rule, cap: u32) -> u32 {
    unsafe { snapshot(&FIB, out, cap, |f| &f.rules, |r| r.table) }
}

#[cfg(a20_net_policy_v6)]
mod ipv6 {
    use super::*;

    type Ip6 = [u8; 16];
    type Route6 = RouteEntry<Ip6>;
    type Rule6 = RuleEntry<Ip6>;

    static FIB: SharedFib<Ip6> = SharedFib(UnsafeCell::new(Fib::new()));
    static HAVE_RULES: AtomicBool = AtomicBool::new(false);

    #[no_mangle]
    pub unsafe extern "C" fn a20_net_route_plan_v6(
        route: *const Route6, remove: i32, flags: u32, out: *mut RouteChange<Ip6>,
    ) -> i32 {
        unsafe { plan(&FIB, route, remove, flags, out) }
    }

    #[no_mangle]
    pub unsafe extern "C" fn a20_net_route_update_v6(r: *const Route6, remove: i32, flags: u32) -> i32 {
        unsafe { update_route(&FIB, r, remove, flags) }
    }

    #[no_mangle]
    pub unsafe extern "C" fn a20_net_rule_update_v6(r: *const Rule6, remove: i32) -> i32 {
        unsafe { update_rule(&FIB, &HAVE_RULES, r, remove) }
    }

    #[no_mangle]
    pub unsafe extern "C" fn a20_net_route_tables_v6(
        src: *const Ip6, dst: *const Ip6, out: *mut u32, cap: u32,
    ) -> u32 {
        if src.is_null() || dst.is_null() || out.is_null() || cap == 0 ||
            !HAVE_RULES.load(Ordering::Acquire) {
            return 0;
        }
        let tables = unsafe { core::slice::from_raw_parts_mut(out, (cap as usize).min(RULES)) };
        let _guard = Guard::acquire();
        unsafe { (&*FIB.0.get()).matching_tables(*src, *dst, 0, tables) as u32 }
    }

    #[no_mangle]
    pub unsafe extern "C" fn a20_net_route_snapshot_v6(out: *mut Route6, cap: u32) -> u32 {
        unsafe { snapshot(&FIB, out, cap, |f| &f.routes, |r| r.table) }
    }

    #[no_mangle]
    pub unsafe extern "C" fn a20_net_rule_snapshot_v6(out: *mut Rule6, cap: u32) -> u32 {
        unsafe { snapshot(&FIB, out, cap, |f| &f.rules, |r| r.table) }
    }

    #[no_mangle]
    pub unsafe extern "C" fn a20_net_route_query_table_v6(
        dst: *const Ip6, table: u32, out: *mut Route6,
    ) -> i32 {
        if dst.is_null() || out.is_null() || table == 0 {
            return -EINVAL;
        }
        let _guard = Guard::acquire();
        if let Some(route) = unsafe { (&*FIB.0.get()).lookup_table(*dst, table) } {
            unsafe { *out = route };
            1
        } else {
            0
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_net_route_add(dst: u32, prefix: u8, ifindex: u16) -> i32 {
    let r = Route {
        destination: dst,
        prefix,
        ifindex: ifindex as u32,
        table: MAIN,
        ..Route::EMPTY
    };
    unsafe { a20_net_route_update_v4(&r, 0, REPLACE) }
}
#[no_mangle]
pub unsafe extern "C" fn a20_net_route_del(dst: u32, prefix: u8, ifindex: u16) -> i32 {
    let r = Route {
        destination: dst,
        prefix,
        ifindex: ifindex as u32,
        table: MAIN,
        ..Route::EMPTY
    };
    unsafe { a20_net_route_update_v4(&r, 1, 0) }
}
#[no_mangle]
pub unsafe extern "C" fn a20_net_route_flush_if(ifindex: u16) {
    let _guard = Guard::acquire();
    unsafe { (&mut *FIB.0.get()).flush_if(ifindex as u32) };
}
#[no_mangle]
pub unsafe extern "C" fn a20_net_route_lookup(dst: u32) -> u32 {
    let mut r = Route::EMPTY;
    if unsafe { a20_net_route_query_v4(0, dst, 0, &mut r) } == 1 {
        r.ifindex
    } else {
        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn ip(a: [u8; 4]) -> u32 {
        u32::from_ne_bytes(a)
    }
    fn route(dst: [u8; 4], prefix: u8, oif: u32, table: u32, metric: u32) -> Route {
        Route {
            destination: ip(dst),
            prefix,
            ifindex: oif,
            table,
            metric,
            ..Route::EMPTY
        }
    }
    #[test]
    fn wire_order_prefixes_and_longest_match() {
        let mut f = Fib::new();
        assert_eq!(f.route(route([10, 9, 8, 7], 8, 1, MAIN, 0), false, 0), 0);
        assert_eq!(f.route(route([10, 2, 0, 9], 24, 2, MAIN, 0), false, 0), 0);
        assert_eq!(f.lookup(0, ip([10, 2, 0, 4]), 0).unwrap().ifindex, 2);
        assert_eq!(f.lookup(0, ip([10, 4, 0, 4]), 0).unwrap().ifindex, 1);
        assert_eq!(f.lookup(0, ip([11, 2, 0, 4]), 0), None);
    }
    #[test]
    fn gateway_metric_replace_and_interface_removal() {
        let mut f = Fib::new();
        let mut r = route([0; 4], 0, 1, MAIN, 50);
        r.gateway = ip([192, 168, 1, 1]);
        assert_eq!(f.route(r, false, 0), 0);
        assert_eq!(f.route(r, false, 0), -EEXIST);
        r.ifindex = 3;
        assert_eq!(f.route(r, false, REPLACE), 0);
        assert_eq!(f.lookup(0, ip([8, 8, 8, 8]), 0).unwrap().gateway, r.gateway);
        assert_eq!(f.route(route([0; 4], 0, 2, MAIN, 10), false, 0), 0);
        assert_eq!(f.lookup(0, 0, 0).unwrap().ifindex, 2);
        f.flush_if(2);
        assert_eq!(f.lookup(0, 0, 0).unwrap().ifindex, 3);
    }
    #[test]
    fn source_and_mark_rules_obey_priority_and_empty_table_fallback() {
        let mut f = Fib::new();
        assert_eq!(f.route(route([0; 4], 0, 1, MAIN, 0), false, 0), 0);
        assert_eq!(f.route(route([0; 4], 0, 2, 100, 0), false, 0), 0);
        let r = Rule {
            source: ip([192, 168, 5, 0]),
            source_prefix: 24,
            priority: 100,
            table: 100,
            ..Rule::EMPTY
        };
        assert_eq!(f.rule(r, false), 0);
        assert_eq!(
            f.rule(
                Rule {
                    priority: 1,
                    table: 200,
                    ..Rule::EMPTY
                },
                false
            ),
            0
        );
        assert_eq!(f.lookup(ip([192, 168, 5, 7]), 0, 0).unwrap().ifindex, 2);
        assert_eq!(f.lookup(ip([192, 168, 6, 7]), 0, 0).unwrap().ifindex, 1);
        assert_eq!(f.rule(r, true), 0);
        assert_eq!(
            f.rule(
                Rule {
                    priority: 2,
                    table: 100,
                    mark: 4,
                    mark_mask: 7,
                    ..Rule::EMPTY
                },
                false
            ),
            0
        );
        assert_eq!(f.lookup(0, 0, 4).unwrap().ifindex, 2);
        assert_eq!(f.lookup(0, 0, 0).unwrap().ifindex, 1);
    }
    #[test]
    fn bounded_storage_refuses_overflow_without_losing_routes() {
        let mut f = Fib::new();
        for i in 0..ROUTES {
            assert_eq!(
                f.route(route([10, 0, 0, i as u8], 32, 1, MAIN, 0), false, 0),
                0
            );
        }
        assert_eq!(
            f.route(route([11, 0, 0, 1], 32, 1, MAIN, 0), false, 0),
            -ENOSPC
        );
        assert_eq!(f.lookup(0, ip([10, 0, 0, 0]), 0).unwrap().ifindex, 1);
        assert_eq!(f.route(route([0; 4], 33, 1, MAIN, 0), false, 0), -EINVAL);
    }
}
