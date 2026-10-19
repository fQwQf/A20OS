#![no_std]

//! `prctl(2)` and `setns(2)` policy.
//!
//! Rust owns the decisions: the argument bounds, which task fields each
//! `prctl` operation touches, the parent-alive rule for `PR_SET_PDEATHSIG`,
//! the capability and bit bounds for the bounding set, the seccomp mode
//! dispatch, and the namespace-kind to `CLONE_NEW*` mapping together with the
//! per-kind authority rule for `setns`.
//!
//! C keeps the object and memory mechanics: the current-task lookup, the mm
//! dumpable calls, the park-locked pdeathsig and parent-state access, the
//! seccomp installers, the name copy, the usercopy of the returned signal, the
//! procfs namespace getters and their reference counts, and the namespace
//! joins.

#[cfg(test)]
extern crate std;

pub const PR_SET_PDEATHSIG: i32 = 1;
pub const PR_GET_PDEATHSIG: i32 = 2;
pub const PR_GET_DUMPABLE: i32 = 3;
pub const PR_SET_DUMPABLE: i32 = 4;
pub const PR_SET_NAME: i32 = 15;
pub const PR_GET_SECCOMP: i32 = 21;
pub const PR_SET_SECCOMP: i32 = 22;
pub const PR_CAPBSET_READ: i32 = 23;
pub const PR_CAPBSET_DROP: i32 = 24;
pub const PR_SET_NO_NEW_PRIVS: i32 = 38;
pub const PR_GET_NO_NEW_PRIVS: i32 = 39;
pub const PR_SET_THP_DISABLE: i32 = 41;
pub const PR_GET_THP_DISABLE: i32 = 42;

pub const SECCOMP_MODE_STRICT: u64 = 1;
pub const SECCOMP_MODE_FILTER: u64 = 2;

pub const PROCNS_MNT: i32 = 1;
pub const PROCNS_PID: i32 = 2;
pub const PROCNS_UTS: i32 = 3;
pub const PROCNS_USER: i32 = 4;
pub const PROCNS_IPC: i32 = 5;
pub const PROCNS_NET: i32 = 6;
pub const PROCNS_CGROUP: i32 = 7;

pub const CLONE_NEWNS: i32 = 0x0002_0000;
pub const CLONE_NEWCGROUP: i32 = 0x0200_0000;
pub const CLONE_NEWUTS: i32 = 0x0400_0000;
pub const CLONE_NEWIPC: i32 = 0x0800_0000;
pub const CLONE_NEWUSER: i32 = 0x1000_0000;
pub const CLONE_NEWPID: i32 = 0x2000_0000;
pub const CLONE_NEWNET: i32 = 0x4000_0000;

const CAP_SETPCAP: i32 = 8;
const NSIG: u64 = 64;

const EPERM: i64 = -1;
const ESRCH: i64 = -3;
const EFAULT: i64 = -14;
const EINVAL: i64 = -22;

extern "C" {
    fn a20_pp_current() -> *mut core::ffi::c_void;
    fn a20_pp_has_mm(t: *mut core::ffi::c_void) -> i32;
    fn a20_pp_mm_dumpable(t: *mut core::ffi::c_void) -> i32;
    fn a20_pp_mm_set_dumpable(t: *mut core::ffi::c_void, value: i32);
    fn a20_pp_parent_alive(t: *mut core::ffi::c_void) -> i32;
    fn a20_pp_set_pdeathsig(t: *mut core::ffi::c_void, sig: i32);
    fn a20_pp_get_pdeathsig(t: *mut core::ffi::c_void) -> i32;
    fn a20_pp_copy_out_sig(addr: u64, sig: i32) -> i32;
    fn a20_pp_has_cap(t: *mut core::ffi::c_void, cap: i32) -> i32;
    fn a20_pp_cap_bounding(t: *mut core::ffi::c_void) -> u64;
    fn a20_pp_cap_drop(t: *mut core::ffi::c_void, bit: u32);
    fn a20_pp_nnps(t: *mut core::ffi::c_void) -> i32;
    fn a20_pp_set_nnps(t: *mut core::ffi::c_void);
    fn a20_pp_thp(t: *mut core::ffi::c_void) -> i32;
    fn a20_pp_set_thp(t: *mut core::ffi::c_void, value: i32);
    fn a20_pp_seccomp_strict(t: *mut core::ffi::c_void) -> i64;
    fn a20_pp_seccomp_filter(t: *mut core::ffi::c_void, addr: u64) -> i64;
    fn a20_pp_seccomp_mode(t: *mut core::ffi::c_void) -> i64;
    fn a20_pp_set_name(t: *mut core::ffi::c_void, addr: u64);
}

/// Run one `prctl(2)` request.  Returns the syscall result.
///
/// # Safety
/// `op`/`a1..a4` are the raw syscall arguments; only the operations that need
/// a user pointer dereference it, through the C bridges.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_prctl(op: u64, a1: u64, a2: u64, a3: u64, a4: u64) -> i64 {
    let t = unsafe { a20_pp_current() };
    match op as i32 {
        PR_GET_DUMPABLE => {
            if t.is_null() || unsafe { a20_pp_has_mm(t) } == 0 {
                return ESRCH;
            }
            unsafe { a20_pp_mm_dumpable(t) as i64 }
        }
        PR_SET_DUMPABLE => {
            if t.is_null() || unsafe { a20_pp_has_mm(t) } == 0 {
                return ESRCH;
            }
            if a1 > 1 {
                return EINVAL;
            }
            unsafe { a20_pp_mm_set_dumpable(t, a1 as i32) };
            0
        }
        PR_SET_PDEATHSIG => {
            if t.is_null() {
                return ESRCH;
            }
            if a1 >= NSIG {
                return EINVAL;
            }
            // A parent that is already gone is reported with -ESRCH; the
            // parent's state is sampled before the target's field is written,
            // so the two park locks are never nested.
            if unsafe { a20_pp_parent_alive(t) } == 0 {
                return ESRCH;
            }
            unsafe { a20_pp_set_pdeathsig(t, a1 as i32) };
            0
        }
        PR_GET_PDEATHSIG => {
            if t.is_null() {
                return ESRCH;
            }
            let sig = unsafe { a20_pp_get_pdeathsig(t) };
            unsafe { a20_pp_copy_out_sig(a1, sig) as i64 }
        }
        PR_SET_NAME => {
            if !t.is_null() {
                unsafe { a20_pp_set_name(t, a1) };
            }
            0
        }
        PR_CAPBSET_READ => {
            if t.is_null() || a1 >= NSIG {
                return EINVAL;
            }
            let bounding = unsafe { a20_pp_cap_bounding(t) };
            if bounding & (1u64 << a1) != 0 { 1 } else { 0 }
        }
        PR_CAPBSET_DROP => {
            if t.is_null() || a1 >= NSIG {
                return EINVAL;
            }
            if unsafe { a20_pp_has_cap(t, CAP_SETPCAP) } == 0 {
                return EPERM;
            }
            unsafe { a20_pp_cap_drop(t, a1 as u32) };
            0
        }
        PR_SET_NO_NEW_PRIVS => {
            if t.is_null() {
                return ESRCH;
            }
            if a1 != 1 || a2 != 0 || a3 != 0 || a4 != 0 {
                return EINVAL;
            }
            unsafe { a20_pp_set_nnps(t) };
            0
        }
        PR_GET_NO_NEW_PRIVS => {
            if t.is_null() {
                return ESRCH;
            }
            if a1 != 0 || a2 != 0 || a3 != 0 || a4 != 0 {
                return EINVAL;
            }
            unsafe { a20_pp_nnps(t) as i64 }
        }
        PR_SET_THP_DISABLE => {
            if t.is_null() {
                return ESRCH;
            }
            if a1 > 1 || a2 != 0 || a3 != 0 || a4 != 0 {
                return EINVAL;
            }
            unsafe { a20_pp_set_thp(t, a1 as i32) };
            0
        }
        PR_GET_THP_DISABLE => {
            if t.is_null() {
                0
            } else {
                unsafe { a20_pp_thp(t) as i64 }
            }
        }
        PR_SET_SECCOMP => {
            if t.is_null() {
                return ESRCH;
            }
            if a1 == SECCOMP_MODE_STRICT {
                unsafe { a20_pp_seccomp_strict(t) }
            } else if a1 == SECCOMP_MODE_FILTER {
                unsafe { a20_pp_seccomp_filter(t, a2) }
            } else {
                EINVAL
            }
        }
        PR_GET_SECCOMP => unsafe { a20_pp_seccomp_mode(t) },
        _ => EINVAL,
    }
}

/// The `CLONE_NEW*` bit a `/proc/<pid>/ns/<type>` file maps to, or 0 for an
/// unknown kind.
#[unsafe(no_mangle)]
pub extern "C" fn a20_setns_kind_flag(kind: i32) -> i32 {
    match kind {
        PROCNS_MNT => CLONE_NEWNS,
        PROCNS_PID => CLONE_NEWPID,
        PROCNS_UTS => CLONE_NEWUTS,
        PROCNS_USER => CLONE_NEWUSER,
        PROCNS_IPC => CLONE_NEWIPC,
        PROCNS_NET => CLONE_NEWNET,
        PROCNS_CGROUP => CLONE_NEWCGROUP,
        _ => 0,
    }
}

fn kind_is_resource(kind: i32) -> bool {
    matches!(kind, PROCNS_UTS | PROCNS_IPC | PROCNS_NET | PROCNS_CGROUP)
}

/// The kind/`nstype` agreement check: a non-zero `nstype` must name the
/// namespace kind the file descriptor was opened for.
#[unsafe(no_mangle)]
pub extern "C" fn a20_setns_nstype_ok(kind: i32, nstype: i32) -> i32 {
    let flag = a20_setns_kind_flag(kind);
    if flag == 0 || (nstype != 0 && nstype != flag) {
        return EINVAL as i32;
    }
    0
}

/// The per-kind authority rule.  `capable_in_target` is `CAP_SYS_ADMIN` in the
/// user namespace that owns the target; `capable_own` is `CAP_SYS_ADMIN` in the
/// caller's own namespace.
///
/// A resource namespace (UTS/IPC/NET/CGROUP) is a resource of the owning user
/// namespace, so both are required.  A mount, pid or user namespace requires
/// authority over the target only -- joining a user namespace is the one case
/// where being root in your own namespace is deliberately not enough.
#[unsafe(no_mangle)]
pub extern "C" fn a20_setns_authority(kind: i32, capable_in_target: i32,
                                      capable_own: i32) -> i32 {
    if kind_is_resource(kind) {
        if capable_in_target == 0 || capable_own == 0 {
            return EPERM as i32;
        }
    } else if capable_in_target == 0 {
        return EPERM as i32;
    }
    0
}


/// The result of `getpgid(2)`/`getsid(2)`: the caller and the target must both
/// exist (pid 0 already resolved to the caller by the adapter).
#[unsafe(no_mangle)]
pub extern "C" fn a20_getid(self_ok: i32, target_ok: i32, value: i32) -> i64 {
    if self_ok == 0 || target_ok == 0 {
        return ESRCH;
    }
    value as i64
}

/// The `setpgid(2)` policy.  Returns the group id to store, or a negative
/// errno.  A `pgid` of zero means "the target's own process group", a
/// non-positive result after that is `EINVAL`, a task other than the caller
/// must be its child, and the target must share the caller's session.
#[unsafe(no_mangle)]
pub extern "C" fn a20_setpgid(self_ok: i32, target_ok: i32, target_pid: i32,
                              target_ppid: i32, target_sid: i32, self_pid: i32,
                              self_sid: i32, is_self: i32, pgid: i32) -> i64 {
    if self_ok == 0 || target_ok == 0 {
        return ESRCH;
    }
    let pgid = if pgid == 0 { target_pid } else { pgid };
    if pgid <= 0 {
        return EINVAL;
    }
    if is_self == 0 && target_ppid != self_pid {
        return ESRCH;
    }
    if target_sid != self_sid {
        return EPERM;
    }
    pgid as i64
}

/// The `setsid(2)` policy: a process already leading its group cannot start a
/// session.  Returns the new session id (the pid), or a negative errno.
#[unsafe(no_mangle)]
pub extern "C" fn a20_setsid(self_ok: i32, pid: i32, pgid: i32) -> i64 {
    if self_ok == 0 {
        return ESRCH;
    }
    if pgid == pid {
        return EPERM;
    }
    pid as i64
}

/// The UTS name policy shared by `sethostname`/`setdomainname`: the length
/// bound is checked before the authority, as the kernel always did.
#[unsafe(no_mangle)]
pub extern "C" fn a20_uts_set_check(len: usize, capable_in_owner: i32) -> i32 {
    if len > 64 {
        return EINVAL as i32;
    }
    if capable_in_owner == 0 {
        return EPERM as i32;
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    static TEST_LOCK: Mutex<()> = Mutex::new(());

    struct Fake {
        has_mm: i32,
        dumpable: i32,
        parent_alive: i32,
        pdeathsig: i32,
        copy_rc: i32,
        last_sig: i32,
        has_cap: i32,
        bounding: u64,
        nnps: i32,
        thp: i32,
        seccomp_mode: i64,
        seccomp_strict_calls: i32,
        seccomp_filter_addr: u64,
        name_addr: u64,
        name_calls: i32,
    }

    static mut F: Fake = Fake {
        has_mm: 1, dumpable: 0, parent_alive: 1, pdeathsig: 0, copy_rc: 0,
        last_sig: 0, has_cap: 0, bounding: 0, nnps: 0, thp: 0, seccomp_mode: 0,
        seccomp_strict_calls: 0, seccomp_filter_addr: 0, name_addr: 0, name_calls: 0,
    };

    fn fix() -> &'static mut Fake { unsafe { &mut *core::ptr::addr_of_mut!(F) } }

    fn reset() -> std::sync::MutexGuard<'static, ()> {
        let guard = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        *fix() = Fake {
            has_mm: 1, dumpable: 0, parent_alive: 1, pdeathsig: 0, copy_rc: 0,
            last_sig: 0, has_cap: 0, bounding: 0, nnps: 0, thp: 0, seccomp_mode: 0,
            seccomp_strict_calls: 0, seccomp_filter_addr: 0, name_addr: 0, name_calls: 0,
        };
        guard
    }

    fn t() -> *mut core::ffi::c_void { 1 as *mut core::ffi::c_void }

    unsafe fn call(op: i32, a1: u64, a2: u64, a3: u64, a4: u64) -> i64 {
        unsafe { a20_prctl(op as u64, a1, a2, a3, a4) }
    }

    #[no_mangle] unsafe extern "C" fn a20_pp_current() -> *mut core::ffi::c_void { t() }
    #[no_mangle] unsafe extern "C" fn a20_pp_has_mm(_t: *mut core::ffi::c_void) -> i32 { fix().has_mm }
    #[no_mangle] unsafe extern "C" fn a20_pp_mm_dumpable(_t: *mut core::ffi::c_void) -> i32 { fix().dumpable }
    #[no_mangle] unsafe extern "C" fn a20_pp_mm_set_dumpable(_t: *mut core::ffi::c_void, v: i32) { fix().dumpable = v }
    #[no_mangle] unsafe extern "C" fn a20_pp_parent_alive(_t: *mut core::ffi::c_void) -> i32 { fix().parent_alive }
    #[no_mangle] unsafe extern "C" fn a20_pp_set_pdeathsig(_t: *mut core::ffi::c_void, s: i32) { fix().pdeathsig = s }
    #[no_mangle] unsafe extern "C" fn a20_pp_get_pdeathsig(_t: *mut core::ffi::c_void) -> i32 { fix().pdeathsig }
    #[no_mangle] unsafe extern "C" fn a20_pp_copy_out_sig(_addr: u64, s: i32) -> i32 {
        fix().last_sig = s;
        fix().copy_rc
    }
    #[no_mangle] unsafe extern "C" fn a20_pp_has_cap(_t: *mut core::ffi::c_void, _c: i32) -> i32 { fix().has_cap }
    #[no_mangle] unsafe extern "C" fn a20_pp_cap_bounding(_t: *mut core::ffi::c_void) -> u64 { fix().bounding }
    #[no_mangle] unsafe extern "C" fn a20_pp_cap_drop(_t: *mut core::ffi::c_void, bit: u32) {
        fix().bounding &= !(1u64 << bit);
    }
    #[no_mangle] unsafe extern "C" fn a20_pp_nnps(_t: *mut core::ffi::c_void) -> i32 { fix().nnps }
    #[no_mangle] unsafe extern "C" fn a20_pp_set_nnps(_t: *mut core::ffi::c_void) { fix().nnps = 1 }
    #[no_mangle] unsafe extern "C" fn a20_pp_thp(_t: *mut core::ffi::c_void) -> i32 { fix().thp }
    #[no_mangle] unsafe extern "C" fn a20_pp_set_thp(_t: *mut core::ffi::c_void, v: i32) { fix().thp = v }
    #[no_mangle] unsafe extern "C" fn a20_pp_seccomp_strict(_t: *mut core::ffi::c_void) -> i64 {
        fix().seccomp_strict_calls += 1;
        0
    }
    #[no_mangle] unsafe extern "C" fn a20_pp_seccomp_filter(_t: *mut core::ffi::c_void, a: u64) -> i64 {
        fix().seccomp_filter_addr = a;
        0
    }
    #[no_mangle] unsafe extern "C" fn a20_pp_seccomp_mode(_t: *mut core::ffi::c_void) -> i64 { fix().seccomp_mode }
    #[no_mangle] unsafe extern "C" fn a20_pp_set_name(_t: *mut core::ffi::c_void, a: u64) {
        fix().name_addr = a;
        fix().name_calls += 1;
    }

    #[test]
    fn dumpable_needs_an_address_space_and_is_bounded() {
        let _g = reset();
        unsafe {
            assert_eq!(call(PR_GET_DUMPABLE, 0, 0, 0, 0), 0);
            assert_eq!(call(PR_SET_DUMPABLE, 1, 0, 0, 0), 0);
            assert_eq!(fix().dumpable, 1);
            assert_eq!(call(PR_SET_DUMPABLE, 2, 0, 0, 0), EINVAL);
            fix().has_mm = 0;
            assert_eq!(call(PR_GET_DUMPABLE, 0, 0, 0, 0), ESRCH);
            assert_eq!(call(PR_SET_DUMPABLE, 1, 0, 0, 0), ESRCH);
        }
    }

    #[test]
    fn pdeathsig_is_bounded_and_requires_a_live_parent() {
        let _g = reset();
        unsafe {
            assert_eq!(call(PR_SET_PDEATHSIG, 15, 0, 0, 0), 0);
            assert_eq!(fix().pdeathsig, 15);
            assert_eq!(call(PR_SET_PDEATHSIG, 64, 0, 0, 0), EINVAL);
            fix().parent_alive = 0;
            assert_eq!(call(PR_SET_PDEATHSIG, 15, 0, 0, 0), ESRCH);
            fix().parent_alive = 1;
            fix().pdeathsig = 9;
            assert_eq!(call(PR_GET_PDEATHSIG, 0x1234, 0, 0, 0), 0);
            assert_eq!(fix().last_sig, 9);
            fix().copy_rc = EFAULT as i32;
            assert_eq!(call(PR_GET_PDEATHSIG, 0x1234, 0, 0, 0), EFAULT);
        }
    }

    #[test]
    fn name_and_unknown_operations() {
        let _g = reset();
        unsafe {
            assert_eq!(call(PR_SET_NAME, 0x55, 0, 0, 0), 0);
            assert_eq!(fix().name_addr, 0x55);
            assert_eq!(fix().name_calls, 1);
            assert_eq!(call(PR_CAPBSET_READ, 0, 0, 0, 0), 0, "unknown op");
            assert_eq!(call(9999, 0, 0, 0, 0), EINVAL);
        }
    }

    #[test]
    fn capbset_reads_and_drops_with_the_capability_rule() {
        let _g = reset();
        unsafe {
            fix().bounding = 1 << 5;
            assert_eq!(call(PR_CAPBSET_READ, 5, 0, 0, 0), 1);
            assert_eq!(call(PR_CAPBSET_READ, 6, 0, 0, 0), 0);
            assert_eq!(call(PR_CAPBSET_READ, 64, 0, 0, 0), EINVAL);
            assert_eq!(call(PR_CAPBSET_DROP, 5, 0, 0, 0), EPERM);
            fix().has_cap = 1;
            assert_eq!(call(PR_CAPBSET_DROP, 5, 0, 0, 0), 0);
            assert_eq!(fix().bounding, 0);
            assert_eq!(call(PR_CAPBSET_DROP, 64, 0, 0, 0), EINVAL);
        }
    }

    #[test]
    fn no_new_privs_is_one_way_and_argument_strict() {
        let _g = reset();
        unsafe {
            assert_eq!(call(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0), 0);
            assert_eq!(fix().nnps, 1);
            assert_eq!(call(PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0), EINVAL);
            assert_eq!(call(PR_SET_NO_NEW_PRIVS, 1, 1, 0, 0), EINVAL);
            assert_eq!(call(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0), 1);
            assert_eq!(call(PR_GET_NO_NEW_PRIVS, 1, 0, 0, 0), EINVAL);
        }
    }

    #[test]
    fn thp_disable_is_bounded_and_argument_strict() {
        let _g = reset();
        unsafe {
            assert_eq!(call(PR_SET_THP_DISABLE, 1, 0, 0, 0), 0);
            assert_eq!(fix().thp, 1);
            assert_eq!(call(PR_GET_THP_DISABLE, 0, 0, 0, 0), 1);
            assert_eq!(call(PR_SET_THP_DISABLE, 2, 0, 0, 0), EINVAL);
            assert_eq!(call(PR_SET_THP_DISABLE, 0, 0, 1, 0), EINVAL);
        }
    }

    #[test]
    fn seccomp_modes_dispatch_to_the_installers() {
        let _g = reset();
        unsafe {
            assert_eq!(call(PR_SET_SECCOMP, SECCOMP_MODE_STRICT, 0, 0, 0), 0);
            assert_eq!(fix().seccomp_strict_calls, 1);
            assert_eq!(call(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, 0xbeef, 0, 0), 0);
            assert_eq!(fix().seccomp_filter_addr, 0xbeef);
            assert_eq!(call(PR_SET_SECCOMP, 3, 0, 0, 0), EINVAL);
            fix().seccomp_mode = 2;
            assert_eq!(call(PR_GET_SECCOMP, 0, 0, 0, 0), 2);
        }
    }

    #[test]
    fn getid_needs_both_tasks() {
        assert_eq!(a20_getid(1, 1, 4321), 4321);
        assert_eq!(a20_getid(0, 1, 4321), ESRCH);
        assert_eq!(a20_getid(1, 0, 4321), ESRCH);
    }

    #[test]
    fn setpgid_normalizes_and_enforces_the_child_and_session_rules() {
        let self_pid = 100;
        let self_sid = 100;
        // pgid 0 means the target's own group.
        assert_eq!(a20_setpgid(1, 1, 200, 100, 100, self_pid, self_sid, 0, 0), 200);
        // An explicit positive group is taken as given.
        assert_eq!(a20_setpgid(1, 1, 200, 100, 100, self_pid, self_sid, 0, 150), 150);
        // A negative target group is EINVAL.
        assert_eq!(a20_setpgid(1, 1, 200, 100, 100, self_pid, self_sid, 0, -1), EINVAL);
        // A non-child cannot be moved by the caller.
        assert_eq!(a20_setpgid(1, 1, 200, 999, 100, self_pid, self_sid, 0, 150), ESRCH);
        // A task in another session is EPERM.
        assert_eq!(a20_setpgid(1, 1, 200, 100, 555, self_pid, self_sid, 0, 150), EPERM);
        // The caller itself may always change its own group.
        assert_eq!(a20_setpgid(1, 1, 100, 1, 100, self_pid, self_sid, 1, 0), 100);
        // Missing tasks.
        assert_eq!(a20_setpgid(0, 1, 200, 100, 100, self_pid, self_sid, 0, 150), ESRCH);
        assert_eq!(a20_setpgid(1, 0, 0, 0, 0, self_pid, self_sid, 0, 150), ESRCH);
    }

    #[test]
    fn setsid_refuses_a_group_leader() {
        assert_eq!(a20_setsid(1, 300, 300), EPERM);
        assert_eq!(a20_setsid(1, 300, 200), 300);
        assert_eq!(a20_setsid(0, 300, 200), ESRCH);
    }

    #[test]
    fn uts_name_checks_length_before_authority() {
        assert_eq!(a20_uts_set_check(4, 1), 0);
        assert_eq!(a20_uts_set_check(64, 1), 0);
        assert_eq!(a20_uts_set_check(65, 1), EINVAL as i32);
        assert_eq!(a20_uts_set_check(65, 0), EINVAL as i32);
        assert_eq!(a20_uts_set_check(4, 0), EPERM as i32);
    }

    #[test]
    fn setns_kind_mapping_and_type_agreement() {
        for (kind, flag) in [
            (PROCNS_MNT, CLONE_NEWNS), (PROCNS_PID, CLONE_NEWPID),
            (PROCNS_UTS, CLONE_NEWUTS), (PROCNS_USER, CLONE_NEWUSER),
            (PROCNS_IPC, CLONE_NEWIPC), (PROCNS_NET, CLONE_NEWNET),
            (PROCNS_CGROUP, CLONE_NEWCGROUP),
        ] {
            assert_eq!(a20_setns_kind_flag(kind), flag);
            assert_eq!(a20_setns_nstype_ok(kind, 0), 0);
            assert_eq!(a20_setns_nstype_ok(kind, flag), 0);
            assert_eq!(a20_setns_nstype_ok(kind, flag ^ 1 << 24), EINVAL as i32);
        }
        assert_eq!(a20_setns_kind_flag(0), 0);
        assert_eq!(a20_setns_kind_flag(99), 0);
        assert_eq!(a20_setns_nstype_ok(0, 0), EINVAL as i32);
    }

    #[test]
    fn setns_authority_differs_for_resource_and_hierarchical_namespaces() {
        // A resource namespace needs authority in the target and in the caller.
        assert_eq!(a20_setns_authority(PROCNS_UTS, 1, 1), 0);
        assert_eq!(a20_setns_authority(PROCNS_UTS, 1, 0), EPERM as i32);
        assert_eq!(a20_setns_authority(PROCNS_UTS, 0, 1), EPERM as i32);
        assert_eq!(a20_setns_authority(PROCNS_NET, 0, 0), EPERM as i32);
        // Mount, pid and user namespaces need authority over the target only.
        for kind in [PROCNS_MNT, PROCNS_PID, PROCNS_USER] {
            assert_eq!(a20_setns_authority(kind, 1, 0), 0);
            assert_eq!(a20_setns_authority(kind, 0, 1), EPERM as i32);
        }
    }
}
