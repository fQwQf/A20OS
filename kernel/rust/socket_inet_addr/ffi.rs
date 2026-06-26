use a20rust_support::lock::spinlock_t;
use core::ffi::c_int;

pub const AF_INET: c_int = 2;
pub const AF_INET6: c_int = 10;

pub const EINVAL: c_int = 22;
pub const EAFNOSUPPORT: c_int = 97;

pub const NET_SOCKADDR_MAX: usize = 128;

#[repr(C)]
pub struct net_sockaddr_in_t {
    pub sin_family: u16,
    pub sin_port: u16,
    pub sin_addr: u32,
    pub sin_zero: [u8; 8],
}

#[repr(C)]
pub struct net_sockaddr_in6_t {
    pub sin6_family: u16,
    pub sin6_port: u16,
    pub sin6_flowinfo: u32,
    pub sin6_addr: [u8; 16],
    pub sin6_scope_id: u32,
}

#[repr(C)]
pub struct net_socket_t {
    pub domain: c_int,
    pub type_: c_int,
    pub protocol: c_int,
    pub nonblock: c_int,
    pub closed: c_int,
    pub shut_rd: c_int,
    pub shut_wr: c_int,
    pub peer_closed: c_int,
    pub bound: c_int,
    pub connected: c_int,
    pub listening: c_int,
    pub local: [u8; NET_SOCKADDR_MAX],
    pub local_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ip4_addr_t {
    pub addr: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ip6_addr_t {
    pub addr: [u32; 4],
}

#[repr(C)]
pub union ip_addr_u {
    pub ip6: ip6_addr_t,
    pub ip4: ip4_addr_t,
}

#[repr(C)]
pub struct ip_addr_t {
    pub u_addr: ip_addr_u,
    pub type_: u8,
}

unsafe extern "C" {
    pub static mut g_net_lock: spinlock_t;

    pub fn a20_ip_addr_set_ip4_u32(ip: *mut ip_addr_t, val: u32);
    pub fn a20_ip_addr_get_ip4_u32(ip: *const ip_addr_t) -> u32;
    pub fn a20_ip_is_v4(ip: *const ip_addr_t) -> c_int;
}
