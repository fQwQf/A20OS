#![no_std]
#![warn(rust_2018_idioms)]

mod ffi;

use core::ffi::{c_int, c_void};
use core::mem::{size_of, zeroed};
use core::ptr;

use ffi::{
    ip_addr_t, net_sockaddr_in6_t, net_sockaddr_in_t, net_socket_t, AF_INET, AF_INET6,
    EAFNOSUPPORT, EINVAL, NET_SOCKADDR_MAX,
};

static mut G_NEXT_EPHEMERAL: u16 = 49152;

const IPV4_LOOPBACK_RAW: u32 = 0x0100_007f;
const IPV4_QEMU_HOST_RAW: u32 = 0x0f02_000a;

#[inline]
const fn net_htons(x: u16) -> u16 {
    x.rotate_left(8)
}

#[inline]
unsafe fn sockaddr_family(addr: *const c_void) -> u16 {
    unsafe { *(addr as *const u16) }
}

#[inline]
unsafe fn ipv6_is_zero(addr: &[u8; 16]) -> bool {
    let mut i = 0usize;
    while i < 16 {
        if addr[i] != 0 {
            return false;
        }
        i += 1;
    }
    true
}

#[inline]
unsafe fn ipv6_is_loopback(addr: &[u8; 16]) -> bool {
    let mut i = 0usize;
    while i < 15 {
        if addr[i] != 0 {
            return false;
        }
        i += 1;
    }
    addr[15] == 1
}

#[no_mangle]
pub extern "C" fn net_ntohs(x: u16) -> u16 {
    net_htons(x)
}

#[no_mangle]
pub unsafe extern "C" fn net_alloc_ephemeral_port_locked() -> u16 {
    let p = unsafe { G_NEXT_EPHEMERAL };
    unsafe {
        G_NEXT_EPHEMERAL = G_NEXT_EPHEMERAL.wrapping_add(1);
        if G_NEXT_EPHEMERAL < 49152 {
            G_NEXT_EPHEMERAL = 49152;
        }
    }
    net_htons(p)
}

#[no_mangle]
pub unsafe extern "C" fn net_sockaddr_loopback(s: *mut net_socket_t, port: u16) {
    if s.is_null() {
        return;
    }

    let mut addr: net_sockaddr_in_t = unsafe { zeroed() };
    addr.sin_family = unsafe { (*s).domain as u16 };
    addr.sin_port = port;
    addr.sin_addr = IPV4_LOOPBACK_RAW;

    unsafe {
        ptr::copy_nonoverlapping(
            (&addr as *const net_sockaddr_in_t).cast::<u8>(),
            (*s).local.as_mut_ptr(),
            size_of::<net_sockaddr_in_t>(),
        );
        (*s).local_len = size_of::<net_sockaddr_in_t>();
        (*s).bound = 1;
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_sockaddr_port(
    addr: *const c_void,
    len: usize,
    port: *mut u16,
) -> c_int {
    if addr.is_null() || len < size_of::<net_sockaddr_in_t>() || port.is_null() {
        return -EINVAL;
    }

    let family = unsafe { sockaddr_family(addr) as c_int };
    if family != AF_INET && family != AF_INET6 {
        return -EAFNOSUPPORT;
    }

    unsafe {
        *port = (*(addr as *const net_sockaddr_in_t)).sin_port;
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn net_sockaddr_set_port(addr: *mut c_void, len: usize, port: u16) {
    if addr.is_null() || len < size_of::<net_sockaddr_in_t>() {
        return;
    }

    let in4 = addr as *mut net_sockaddr_in_t;
    let family = unsafe { (*in4).sin_family as c_int };
    if family == AF_INET || family == AF_INET6 {
        unsafe {
            (*in4).sin_port = port;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_sockaddr_in_local(in4: *const net_sockaddr_in_t) -> c_int {
    if in4.is_null() {
        return 0;
    }

    let addr = unsafe { (*in4).sin_addr };
    if addr == 0 || addr == IPV4_LOOPBACK_RAW || addr == IPV4_QEMU_HOST_RAW {
        1
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_sockaddr_to_lwip_ip(
    addr: *const c_void,
    len: usize,
    ip: *mut ip_addr_t,
    port: *mut u16,
) -> c_int {
    if addr.is_null() || ip.is_null() || len < size_of::<net_sockaddr_in_t>() {
        return -EINVAL;
    }

    let in4 = unsafe { &*(addr as *const net_sockaddr_in_t) };
    if in4.sin_family as c_int != AF_INET {
        return -EAFNOSUPPORT;
    }

    unsafe {
        ffi::a20_ip_addr_set_ip4_u32(ip, in4.sin_addr);
        if !port.is_null() {
            *port = net_ntohs(in4.sin_port);
        }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn net_lwip_ip_to_sockaddr(
    ip: *const ip_addr_t,
    port: u16,
    out: *mut u8,
    outlen: *mut usize,
) -> c_int {
    if out.is_null() || outlen.is_null() || unsafe { ffi::a20_ip_is_v4(ip) } == 0 {
        return -EINVAL;
    }

    let mut addr: net_sockaddr_in_t = unsafe { zeroed() };
    addr.sin_family = AF_INET as u16;
    addr.sin_port = net_htons(port);
    addr.sin_addr = unsafe { ffi::a20_ip_addr_get_ip4_u32(ip) };

    unsafe {
        ptr::copy_nonoverlapping(
            (&addr as *const net_sockaddr_in_t).cast::<u8>(),
            out,
            size_of::<net_sockaddr_in_t>(),
        );
        *outlen = size_of::<net_sockaddr_in_t>();
    }
    0
}

#[allow(dead_code)]
unsafe fn net_inet_domains_overlap(a: c_int, b: c_int) -> c_int {
    if a == b || (a == AF_INET && b == AF_INET6) || (a == AF_INET6 && b == AF_INET) {
        1
    } else {
        0
    }
}

#[allow(dead_code)]
unsafe fn net_sockaddr_port_equal(a: *const c_void, alen: usize, b: *const c_void, blen: usize) -> c_int {
    let mut ap = 0u16;
    let mut bp = 0u16;
    if unsafe { net_sockaddr_port(a, alen, &mut ap) } == 0
        && unsafe { net_sockaddr_port(b, blen, &mut bp) } == 0
        && ap == bp
    {
        1
    } else {
        0
    }
}

#[allow(dead_code)]
unsafe fn net_sockaddr_is_local_target(addr: *const c_void, len: usize) -> c_int {
    if addr.is_null() || len < size_of::<net_sockaddr_in_t>() {
        return 0;
    }

    let family = unsafe { sockaddr_family(addr) as c_int };
    if family == AF_INET {
        return unsafe { net_sockaddr_in_local(addr as *const net_sockaddr_in_t) };
    }

    if family == AF_INET6 && len >= size_of::<net_sockaddr_in6_t>() {
        let in6 = unsafe { &*(addr as *const net_sockaddr_in6_t) };
        if unsafe { ipv6_is_zero(&in6.sin6_addr) || ipv6_is_loopback(&in6.sin6_addr) } {
            return 1;
        }
    }

    0
}

#[allow(dead_code)]
unsafe fn _sockaddr_buffer_fits(len: usize) -> bool {
    len <= NET_SOCKADDR_MAX
}
