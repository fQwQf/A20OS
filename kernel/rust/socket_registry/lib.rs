#![no_std]
#![warn(rust_2018_idioms)]
#![allow(static_mut_refs)]

mod ffi;

use a20rust_support::lock::raw_irqsave_lock;
use core::ffi::c_int;
use core::ptr;

use ffi::{net_socket_t, ENFILE, NET_MAX_SOCKETS};

const WORD_BITS: usize = 32;
const FREE_WORDS: usize = NET_MAX_SOCKETS / WORD_BITS;

#[no_mangle]
pub static mut g_sockets: [*mut net_socket_t; NET_MAX_SOCKETS] = [ptr::null_mut(); NET_MAX_SOCKETS];

#[no_mangle]
pub static mut g_net_bh_pending: [c_int; NET_MAX_SOCKETS] = [0; NET_MAX_SOCKETS];

static mut G_SOCK_FREE: [u32; FREE_WORDS] = [0; FREE_WORDS];

#[inline]
unsafe fn clear_socket_state(s: *mut net_socket_t) {
    ffi::a20_socket_set_in_registry(s, 0);
    ffi::a20_socket_set_reg_idx(s, -1);
}

#[no_mangle]
pub unsafe extern "C" fn net_socket_registry_init() {
    let _guard = unsafe { raw_irqsave_lock(core::ptr::addr_of_mut!(ffi::g_net_lock)) };
    unsafe {
        g_sockets = [ptr::null_mut(); NET_MAX_SOCKETS];
        g_net_bh_pending = [0; NET_MAX_SOCKETS];
        G_SOCK_FREE = [0; FREE_WORDS];
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_register_socket_locked(s: *mut net_socket_t) -> c_int {
    if s.is_null() {
        return -ENFILE;
    }

    for w in 0..FREE_WORDS {
        let free_bits = unsafe { !G_SOCK_FREE[w] };
        if free_bits == 0 {
            continue;
        }

        let bit = free_bits.trailing_zeros() as usize;
        let idx = w * WORD_BITS + bit;
        if idx >= NET_MAX_SOCKETS {
            break;
        }

        unsafe {
            g_sockets[idx] = s;
            G_SOCK_FREE[w] |= 1u32 << bit;
            ffi::a20_socket_set_in_registry(s, 1);
            ffi::a20_socket_set_reg_idx(s, idx as c_int);
            g_net_bh_pending[idx] = 0;
        }
        return 0;
    }

    -ENFILE
}

#[no_mangle]
pub unsafe extern "C" fn net_unregister_socket_locked(s: *mut net_socket_t) {
    if s.is_null() || unsafe { ffi::a20_socket_in_registry(s) } == 0 {
        return;
    }

    let reg_idx = unsafe { ffi::a20_socket_reg_idx(s) };
    if reg_idx >= 0 {
        let idx = reg_idx as usize;
        if idx < NET_MAX_SOCKETS && unsafe { g_sockets[idx] } == s {
            unsafe {
                g_sockets[idx] = ptr::null_mut();
                g_net_bh_pending[idx] = 0;
                G_SOCK_FREE[idx / WORD_BITS] &= !(1u32 << (idx % WORD_BITS));
                clear_socket_state(s);
            }
            return;
        }
    }

    for idx in 0..NET_MAX_SOCKETS {
        if unsafe { g_sockets[idx] } == s {
            unsafe {
                g_sockets[idx] = ptr::null_mut();
                g_net_bh_pending[idx] = 0;
                G_SOCK_FREE[idx / WORD_BITS] &= !(1u32 << (idx % WORD_BITS));
                clear_socket_state(s);
            }
            return;
        }
    }

    unsafe { clear_socket_state(s) };
}

#[no_mangle]
pub unsafe extern "C" fn net_socket_is_valid_locked(s: *mut net_socket_t) -> c_int {
    if s.is_null() {
        return 0;
    }
    unsafe { ffi::a20_socket_in_registry(s) }
}
