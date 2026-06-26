use a20rust_support::lock::spinlock_t;
use core::ffi::c_int;

pub const NET_MAX_SOCKETS: usize = 1024;
pub const ENFILE: c_int = 23;

#[repr(C)]
pub struct net_socket_t {
    pub _opaque: [u8; 0],
}

unsafe extern "C" {
    pub static mut g_net_lock: spinlock_t;

    pub fn a20_socket_in_registry(s: *mut net_socket_t) -> c_int;
    pub fn a20_socket_set_in_registry(s: *mut net_socket_t, value: c_int);
    pub fn a20_socket_reg_idx(s: *mut net_socket_t) -> c_int;
    pub fn a20_socket_set_reg_idx(s: *mut net_socket_t, value: c_int);
}
