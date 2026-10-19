#![no_std]

//! NUMA memory-policy validation and state transitions.
//!
//! A20OS has a single NUMA node (node 0).  A mask naming any other node can
//! never be honoured, so it is rejected rather than accepted and reported
//! back: a set/get round trip that promises a placement the kernel cannot
//! perform is worse than an error.  The policy is therefore validated, stored
//! per task and reported -- never applied to physical pages, because there is
//! nowhere to move them.
//!
//! Rust owns the validation order, the stored-state policy (`BIND` and
//! `INTERLEAVE` keep their mask; `DEFAULT` and `LOCAL` do not) and the mbind
//! range contract.  C keeps the `task_t` field access and the two getters,
//! which are pure field reads.

use core::ffi::c_void;

const ESRCH: i32 = -3;
const EINVAL: i32 = -22;

const MPOL_DEFAULT: i32 = 0;
const MPOL_BIND: i32 = 2;
const MPOL_INTERLEAVE: i32 = 3;
const MPOL_LOCAL: i32 = 4;

/// The only node this kernel has.
const VALID_NODES: u64 = 1;

const PAGE_SIZE: u64 = 4096;

unsafe extern "C" {
    fn a20_mempolicy_has_mm(t: *const c_void) -> i32;
    fn a20_mempolicy_user_va_limit() -> usize;
    fn a20_mempolicy_store(t: *mut c_void, mode: i32, nmask: u64) -> i32;
}

/// `MPOL_BIND`/`MPOL_INTERLEAVE` need at least one node; `DEFAULT`/`LOCAL`
/// ignore the mask.  Node 0 is always valid, any other node is not.
#[inline]
fn valid(mode: i32, nmask: u64) -> bool {
    if mode < MPOL_DEFAULT || mode > MPOL_LOCAL {
        return false;
    }
    if (mode == MPOL_BIND || mode == MPOL_INTERLEAVE) && nmask == 0 {
        return false;
    }
    nmask & !VALID_NODES == 0
}

/// Only the modes that actually name nodes keep their mask; the rest store
/// zero so a `get_mempolicy` after `MPOL_DEFAULT` cannot report a stale mask.
#[inline]
fn stored_mask(mode: i32, nmask: u64) -> u64 {
    if mode == MPOL_BIND || mode == MPOL_INTERLEAVE {
        nmask
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn mempolicy_set(t: *mut c_void, mode: i32, nmask: u64) -> i32 {
    if t.is_null() {
        return ESRCH;
    }
    if !valid(mode, nmask) {
        return EINVAL;
    }
    unsafe { a20_mempolicy_store(t, mode, stored_mask(mode, nmask)) }
}

#[no_mangle]
pub unsafe extern "C" fn mempolicy_mbind(
    t: *mut c_void,
    addr: u64,
    len: usize,
    mode: i32,
    nmask: u64,
    flags: u32,
) -> i32 {
    let _ = flags;
    if t.is_null() {
        return ESRCH;
    }
    if !valid(mode, nmask) {
        return EINVAL;
    }
    if addr == 0 || len == 0 {
        return EINVAL;
    }
    if addr & (PAGE_SIZE - 1) != 0 || (len as u64) & (PAGE_SIZE - 1) != 0 {
        return EINVAL;
    }
    // The range must stay inside the user window.  The C reference checked
    // this only when the task had an address space and stored the policy
    // regardless; both are preserved.
    if unsafe { a20_mempolicy_has_mm(t) } != 0 {
        let end = addr.wrapping_add(len as u64);
        if end < addr || end > unsafe { a20_mempolicy_user_va_limit() } as u64 {
            return EINVAL;
        }
    }
    unsafe { a20_mempolicy_store(t, mode, stored_mask(mode, nmask)) }
}
