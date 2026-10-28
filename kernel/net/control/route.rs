#![no_std]
//! Bounded FIB and ordered lookup rules. All algorithms operate on owned value
//! arrays; only the FFI shell touches shared storage, under an IRQ-safe guard.
//! No reference into the FIB survives a transaction or backend call.
use core::cell::UnsafeCell;
use core::ptr;

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

#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Route {
    destination: u32,
    gateway: u32,
    ifindex: u32,
    table: u32,
    metric: u32,
    prefix: u8,
    pad: [u8; 3],
}
impl Route {
    const EMPTY: Self = Self {
        destination: 0,
        gateway: 0,
        ifindex: 0,
        table: 0,
        metric: 0,
        prefix: 0,
        pad: [0; 3],
    };
}

#[repr(C)]
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Rule {
    source: u32,
    destination: u32,
    priority: u32,
    table: u32,
    mark: u32,
    mark_mask: u32,
    source_prefix: u8,
    destination_prefix: u8,
    pad: [u8; 2],
}
impl Rule {
    const EMPTY: Self = Self {
        source: 0,
        destination: 0,
        priority: 0,
        table: 0,
        mark: 0,
        mark_mask: 0,
        source_prefix: 0,
        destination_prefix: 0,
        pad: [0; 2],
    };
    fn matches(&self, src: u32, dst: u32, mark: u32) -> bool {
        (src & mask(self.source_prefix)) == self.source
            && (dst & mask(self.destination_prefix)) == self.destination
            && (mark & self.mark_mask) == (self.mark & self.mark_mask)
    }
}

/* Addresses are u32 containers for network-order bytes, not host integers.
 * Masking the high native bits corrupted prefixes on little-endian targets. */
fn mask(prefix: u8) -> u32 {
    if prefix == 0 {
        0
    } else {
        (u32::MAX << (32 - prefix)).to_be()
    }
}

struct Fib {
    routes: [Route; ROUTES],
    rules: [Rule; RULES],
}
impl Fib {
    const fn new() -> Self {
        Self {
            routes: [Route::EMPTY; ROUTES],
            rules: [Rule::EMPTY; RULES],
        }
    }
    fn route(&mut self, mut r: Route, remove: bool, flags: u32) -> i32 {
        if r.prefix > 32 || r.table == 0 || (!remove && r.ifindex == 0) {
            return -EINVAL;
        }
        r.destination &= mask(r.prefix);
        let existing = self.routes.iter().position(|e| {
            e.table == r.table
                && e.prefix == r.prefix
                && e.destination == r.destination
                && (remove || e.metric == r.metric)
                && (!remove || r.ifindex == 0 || e.ifindex == r.ifindex)
                && (!remove || r.gateway == 0 || e.gateway == r.gateway)
        });
        if remove {
            return match existing {
                Some(i) => {
                    self.routes[i] = Route::EMPTY;
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
    fn rule(&mut self, mut r: Rule, remove: bool) -> i32 {
        if r.table == 0 || r.source_prefix > 32 || r.destination_prefix > 32 {
            return -EINVAL;
        }
        r.source &= mask(r.source_prefix);
        r.destination &= mask(r.destination_prefix);
        if remove {
            return match self.rules.iter().position(|e| *e == r) {
                Some(i) => {
                    self.rules[i] = Rule::EMPTY;
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
    fn lookup_table(&self, dst: u32, table: u32) -> Option<Route> {
        self.routes
            .iter()
            .filter(|r| r.table == table && (dst & mask(r.prefix)) == r.destination)
            .min_by_key(|r| (32 - r.prefix, r.metric))
            .copied()
    }
    fn lookup(&self, src: u32, dst: u32, mark: u32) -> Option<Route> {
        /* Fixed-size selection avoids allocation/sorting on the packet path.
         * A matching rule with an empty table continues to the next rule. */
        let mut visited = 0u32;
        for _ in 0..RULES {
            let selected = self
                .rules
                .iter()
                .enumerate()
                .filter(|(i, r)| visited & (1 << i) == 0 && r.table != 0)
                .min_by_key(|(i, r)| (r.priority, *i));
            let Some((i, r)) = selected else { break };
            visited |= 1 << i;
            if r.matches(src, dst, mark) {
                if let Some(route) = self.lookup_table(dst, r.table) {
                    return Some(route);
                }
            }
        }
        self.lookup_table(dst, MAIN)
    }
    fn flush_if(&mut self, ifindex: u32) {
        for r in &mut self.routes {
            if r.ifindex == ifindex {
                *r = Route::EMPTY;
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
struct SharedFib(UnsafeCell<Fib>);
// SAFETY: every FFI access holds Guard; no reference or pointer is returned.
unsafe impl Sync for SharedFib {}
static FIB: SharedFib = SharedFib(UnsafeCell::new(Fib::new()));

#[no_mangle]
pub unsafe extern "C" fn a20_net_route_update_v4(r: *const Route, remove: i32, flags: u32) -> i32 {
    if r.is_null() {
        return -EINVAL;
    }
    let value = unsafe { ptr::read(r) };
    let _guard = Guard::acquire();
    unsafe { (&mut *FIB.0.get()).route(value, remove != 0, flags) }
}
#[no_mangle]
pub unsafe extern "C" fn a20_net_rule_update_v4(r: *const Rule, remove: i32) -> i32 {
    if r.is_null() {
        return -EINVAL;
    }
    let value = unsafe { ptr::read(r) };
    let _guard = Guard::acquire();
    unsafe { (&mut *FIB.0.get()).rule(value, remove != 0) }
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
#[no_mangle]
pub unsafe extern "C" fn a20_net_route_snapshot_v4(out: *mut Route, cap: u32) -> u32 {
    if out.is_null() {
        return 0;
    }
    let _guard = Guard::acquire();
    let mut n = 0;
    for r in unsafe { &(*FIB.0.get()).routes } {
        if r.table != 0 && n < cap {
            unsafe { *out.add(n as usize) = *r };
            n += 1;
        }
    }
    n
}
#[no_mangle]
pub unsafe extern "C" fn a20_net_rule_snapshot_v4(out: *mut Rule, cap: u32) -> u32 {
    if out.is_null() {
        return 0;
    }
    let _guard = Guard::acquire();
    let mut n = 0;
    for r in unsafe { &(*FIB.0.get()).rules } {
        if r.table != 0 && n < cap {
            unsafe { *out.add(n as usize) = *r };
            n += 1;
        }
    }
    n
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
