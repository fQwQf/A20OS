//! UXFS lifetime/close-token negotiation state (Rust-owned).
//!
//! `uxfs.c` embeds `uxfs_lifetime_state_t` (mirrored below), serialises
//! negotiation with `sb->lifetime_lock` and performs the INIT/OPEN/CLOSE RPCs.
//! This module owns every transition over the negotiated state: which wire
//! capability selects v1 vs v2, whether shutdown confirmation and
//! token/confirm retirement are enabled, whether a cached negotiation is still
//! valid for the current server generation, which OPEN token to carry, and
//! whether CLOSE needs an RPC at all.
//!
//! Keeping this decision table in one place removes the scattered capability
//! comparisons from the mount/open/close paths while leaving the mechanical
//! lock + RPC orchestration in C.

#![cfg_attr(not(test), no_std)]

use core::ffi::{c_int, c_void};

pub const UFS_LIFETIME_CAPABILITY: u64 = 0x5546_534c_4946_4532; // 'UFSLIFE2'
pub const UFS_SHUTDOWN_CAPABILITY: u64 = 0x5546_5353_4855_5432; // 'UFSSHUT2'
pub const UFS_CLOSE_TOKEN_CAPABILITY: u64 = 0x5546_5343_544f_4b31;
pub const UFS_CLOSE_CONFIRM_CAPABILITY: u64 = 0x5546_5343_464d_5231; // 'UFSCFMR1'

/// Mirror of `uxfs_lifetime_state_t` (kernel/include/fs/uxfs_lifetime_state.h).
#[repr(C)]
pub struct UxfsLifetimeState {
    pub mode: u32,
    pub shutdown_mode: u32,
    pub close_token_mode: u32,
    pub close_confirm_mode: u32,
    pub generation: u64,
    pub server: *mut c_void,
}

const _: [(); 32] = [(); core::mem::size_of::<UxfsLifetimeState>()];
const _: [(); 16] = [(); core::mem::offset_of!(UxfsLifetimeState, generation)];
const _: [(); 24] = [(); core::mem::offset_of!(UxfsLifetimeState, server)];

unsafe fn st<'a>(p: *mut UxfsLifetimeState) -> &'a mut UxfsLifetimeState {
    // SAFETY: the caller passes the live sb->lifetime field.
    unsafe { &mut *p }
}

#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_lifetime_reset(p: *mut UxfsLifetimeState) {
    if p.is_null() {
        return;
    }
    // SAFETY: live state field.
    unsafe {
        *p = UxfsLifetimeState {
            mode: 0,
            shutdown_mode: 0,
            close_token_mode: 0,
            close_confirm_mode: 0,
            generation: 0,
            server: core::ptr::null_mut(),
        };
    }
}

/// A cached negotiation is valid only for the same server task *and* the same
/// server generation, and only after a mode was selected.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_lifetime_negotiated(
    p: *const UxfsLifetimeState,
    server_generation: u64,
    server: *mut c_void,
) -> c_int {
    if p.is_null() {
        return 0;
    }
    // SAFETY: read-only view of the live state.
    let s = unsafe { &*p };
    (s.generation == server_generation && s.server == server && s.mode != 0) as c_int
}

/// Apply an INIT reply.  v2 is selected by any of the three v2 capability
/// replies; only the CONFIRM capability enables open tokens and tombstone
/// confirmation.  A legacy/unknown reply keeps the explicit v1 contract.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_lifetime_apply_init(
    p: *mut UxfsLifetimeState,
    out1: u64,
    out2: u64,
    server_generation: u64,
    server: *mut c_void,
) {
    if p.is_null() {
        return;
    }
    // SAFETY: live state field; caller holds sb->lifetime_lock.
    let s = unsafe { st(p) };
    let v2 = out1 == UFS_LIFETIME_CAPABILITY
        || out1 == UFS_CLOSE_TOKEN_CAPABILITY
        || out1 == UFS_CLOSE_CONFIRM_CAPABILITY;
    s.mode = if v2 { 2 } else { 1 };
    s.shutdown_mode = (out2 == UFS_SHUTDOWN_CAPABILITY) as u32;
    s.close_token_mode = (out1 == UFS_CLOSE_CONFIRM_CAPABILITY) as u32;
    s.close_confirm_mode = (out1 == UFS_CLOSE_CONFIRM_CAPABILITY) as u32;
    s.generation = server_generation;
    s.server = server;
}

#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_lifetime_is_v1(p: *const UxfsLifetimeState) -> c_int {
    (!p.is_null() && unsafe { (*p).mode } == 1) as c_int
}

#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_lifetime_is_v2(p: *const UxfsLifetimeState) -> c_int {
    (!p.is_null() && unsafe { (*p).mode } == 2) as c_int
}

#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_lifetime_shutdown_supported(
    p: *const UxfsLifetimeState,
) -> c_int {
    (!p.is_null() && unsafe { (*p).shutdown_mode } != 0) as c_int
}

/// OPEN carries the server token only on a v2 peer that negotiated CONFIRM
/// retirement.  A v1 peer never took an OPEN reference, so the token stays 0.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_lifetime_open_token(
    p: *const UxfsLifetimeState,
    server_token: u64,
) -> u64 {
    if p.is_null() {
        return 0;
    }
    // SAFETY: read-only view.
    let s = unsafe { &*p };
    if s.mode == 2 && s.close_token_mode != 0 {
        server_token
    } else {
        0
    }
}

/// A v1 peer has no lifetime reference to release, so CLOSE is a local no-op
/// (and is reported as acknowledged).  Everything else needs the wire RPC.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_lifetime_close_needs_rpc(
    p: *const UxfsLifetimeState,
) -> c_int {
    (!p.is_null() && unsafe { (*p).mode } != 1) as c_int
}

#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_lifetime_close_v1_acked(
    p: *const UxfsLifetimeState,
) -> c_int {
    (!p.is_null() && unsafe { (*p).mode } == 1) as c_int
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::ptr::null_mut;

    fn state() -> UxfsLifetimeState {
        UxfsLifetimeState {
            mode: 0,
            shutdown_mode: 0,
            close_token_mode: 0,
            close_confirm_mode: 0,
            generation: 0,
            server: null_mut(),
        }
    }

    fn server(n: usize) -> *mut c_void {
        n as *mut c_void
    }

    #[test]
    fn layout_matches_c() {
        assert_eq!(core::mem::size_of::<UxfsLifetimeState>(), 32);
        assert_eq!(core::mem::offset_of!(UxfsLifetimeState, generation), 16);
        assert_eq!(core::mem::offset_of!(UxfsLifetimeState, server), 24);
    }

    #[test]
    fn reset_and_legacy_v1() {
        let mut s = state();
        unsafe {
            a20_uxfs_lifetime_apply_init(&mut s, 0, 0, 7, server(1));
            assert_eq!(s.mode, 1);
            assert_eq!(a20_uxfs_lifetime_is_v1(&s), 1);
            assert_eq!(a20_uxfs_lifetime_is_v2(&s), 0);
            assert_eq!(a20_uxfs_lifetime_open_token(&s, 99), 0);
            assert_eq!(a20_uxfs_lifetime_close_needs_rpc(&s), 0);
            assert_eq!(a20_uxfs_lifetime_close_v1_acked(&s), 1);
            assert_eq!(a20_uxfs_lifetime_negotiated(&s, 7, server(1)), 1);
            a20_uxfs_lifetime_reset(&mut s);
            assert_eq!(s.mode, 0);
            assert_eq!(a20_uxfs_lifetime_negotiated(&s, 7, server(1)), 0);
        }
    }

    #[test]
    fn lifetime_capability_selects_v2_without_tokens() {
        let mut s = state();
        unsafe {
            a20_uxfs_lifetime_apply_init(&mut s, UFS_LIFETIME_CAPABILITY, 0, 3, server(2));
            assert_eq!(s.mode, 2);
            assert_eq!(s.close_token_mode, 0);
            assert_eq!(s.close_confirm_mode, 0);
            assert_eq!(s.shutdown_mode, 0);
            assert_eq!(a20_uxfs_lifetime_open_token(&s, 42), 0);
            assert_eq!(a20_uxfs_lifetime_close_needs_rpc(&s), 1);
            assert_eq!(a20_uxfs_lifetime_close_v1_acked(&s), 0);
        }
    }

    #[test]
    fn token_capability_is_v2_but_not_confirm_safe() {
        let mut s = state();
        unsafe {
            a20_uxfs_lifetime_apply_init(&mut s, UFS_CLOSE_TOKEN_CAPABILITY, 0, 3, server(2));
            assert_eq!(s.mode, 2);
            assert_eq!(s.close_token_mode, 0);
            assert_eq!(s.close_confirm_mode, 0);
        }
    }

    #[test]
    fn confirm_capability_enables_tokens_confirm_and_shutdown() {
        let mut s = state();
        unsafe {
            a20_uxfs_lifetime_apply_init(
                &mut s, UFS_CLOSE_CONFIRM_CAPABILITY, UFS_SHUTDOWN_CAPABILITY, 4, server(3),
            );
            assert_eq!(s.mode, 2);
            assert_eq!(s.close_token_mode, 1);
            assert_eq!(s.close_confirm_mode, 1);
            assert_eq!(s.shutdown_mode, 1);
            assert_eq!(a20_uxfs_lifetime_shutdown_supported(&s), 1);
            assert_eq!(a20_uxfs_lifetime_open_token(&s, 0x1234), 0x1234);
            assert_eq!(a20_uxfs_lifetime_negotiated(&s, 4, server(3)), 1);
            assert_eq!(a20_uxfs_lifetime_negotiated(&s, 5, server(3)), 0);
            assert_eq!(a20_uxfs_lifetime_negotiated(&s, 4, server(4)), 0);
        }
    }

    #[test]
    fn server_generation_change_invalidates() {
        let mut s = state();
        unsafe {
            a20_uxfs_lifetime_apply_init(&mut s, UFS_LIFETIME_CAPABILITY, 0, 9, server(5));
            assert_eq!(a20_uxfs_lifetime_negotiated(&s, 9, server(5)), 1);
            assert_eq!(a20_uxfs_lifetime_negotiated(&s, 10, server(5)), 0);
            assert_eq!(a20_uxfs_lifetime_negotiated(&s, 9, server(6)), 0);
        }
    }
}
