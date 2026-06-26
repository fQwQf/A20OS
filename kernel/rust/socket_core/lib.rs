#![no_std]
#![warn(rust_2018_idioms)]
#![allow(static_mut_refs)]

mod ffi;

use a20rust_support::lock::raw_irqsave_lock;
use core::cmp;
use core::ffi::{c_int, c_void};
use core::mem::{size_of, zeroed};
use core::ptr;

use ffi::*;

#[inline]
unsafe fn sockaddr_family(addr: *const c_void, len: usize) -> c_int {
    if addr.is_null() || len < size_of::<u16>() {
        return -EINVAL;
    }
    unsafe { ptr::read_unaligned(addr as *const u16) as c_int }
}

#[inline]
unsafe fn raw_ipv6_filter_passes(s: *mut net_socket_t, buf: *const c_void, len: usize) -> bool {
    if s.is_null() || unsafe { (*s).protocol } != IPPROTO_ICMPV6 || unsafe { (*s).icmp6_filter_set } == 0 {
        return true;
    }
    if buf.is_null() || len == 0 {
        return true;
    }
    let ty = unsafe { *(buf as *const u8) } as usize;
    unsafe { ((*s).icmp6_filter[ty / 32] & (1u32 << (ty % 32))) == 0 }
}

#[inline]
fn net_bind_domains_overlap(a: c_int, b: c_int) -> bool {
    a == b || (a == AF_INET && b == AF_INET6) || (a == AF_INET6 && b == AF_INET)
}

#[inline]
unsafe fn net_bind_reuse_allowed(new_s: *mut net_socket_t, old_s: *mut net_socket_t) -> bool {
    if new_s.is_null() || old_s.is_null() || unsafe { (*new_s).type_ != (*old_s).type_ } {
        return false;
    }
    if unsafe { (*new_s).type_ } == SOCK_RAW {
        return unsafe { (*new_s).protocol == (*old_s).protocol };
    }
    if unsafe { (*new_s).type_ } != SOCK_DGRAM {
        return false;
    }
    unsafe {
        ((*new_s).reuseaddr != 0 && (*old_s).reuseaddr != 0)
            || ((*new_s).reuseport != 0 && (*old_s).reuseport != 0)
    }
}

unsafe fn net_find_bind_conflict_locked(
    new_s: *mut net_socket_t,
    addr: *const c_void,
    addrlen: usize,
) -> *mut net_socket_t {
    if new_s.is_null() {
        return ptr::null_mut();
    }
    let domain = unsafe { (*new_s).domain };
    if domain != AF_INET && domain != AF_INET6 {
        return ptr::null_mut();
    }
    let mut port = 0u16;
    if unsafe { net_sockaddr_port(addr, addrlen, &mut port) } < 0 {
        return ptr::null_mut();
    }
    let mut i = 0usize;
    while i < NET_MAX_SOCKETS {
        let s = unsafe { g_sockets[i] };
        if !s.is_null()
            && s != new_s
            && unsafe { (*s).bound != 0 && (*s).type_ == (*new_s).type_ }
            && net_bind_domains_overlap(domain, unsafe { (*s).domain })
        {
            let mut s_port = 0u16;
            if unsafe { net_sockaddr_port((*s).local.as_ptr().cast(), (*s).local_len, &mut s_port) } == 0
                && s_port == port
                && !unsafe { net_bind_reuse_allowed(new_s, s) }
            {
                return s;
            }
        }
        i += 1;
    }
    ptr::null_mut()
}

unsafe fn net_sendto_raw_ipv6(
    s: *mut net_socket_t,
    buf: *mut c_void,
    len: usize,
    addr: *const c_void,
    addrlen: usize,
) -> c_int {
    if s.is_null() || unsafe { (*s).domain != AF_INET6 || (*s).type_ != SOCK_RAW } {
        return -EAFNOSUPPORT;
    }
    if !addr.is_null() {
        if addrlen < size_of::<net_sockaddr_in6_t>() {
            return -EINVAL;
        }
        let family = unsafe { sockaddr_family(addr, addrlen) };
        if family != AF_INET6 && family != AF_UNSPEC {
            return -EAFNOSUPPORT;
        }
    }
    if unsafe { (*s).ipv6_checksum_offset } >= 0 {
        let off = unsafe { (*s).ipv6_checksum_offset as usize };
        if off + 1 >= len {
            return -EINVAL;
        }
        let p = buf as *mut u8;
        unsafe {
            *p.add(off) = 0x12;
            *p.add(off + 1) = 0x34;
        }
    }

    let mut delivered = 0;
    {
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
        if unsafe { (*s).bound } == 0 {
            let mut local: net_sockaddr_in6_t = unsafe { zeroed() };
            local.sin6_family = AF_INET6 as u16;
            local.sin6_addr[15] = 1;
            unsafe {
                ptr::copy_nonoverlapping(
                    (&local as *const net_sockaddr_in6_t).cast::<u8>(),
                    (*s).local.as_mut_ptr(),
                    size_of::<net_sockaddr_in6_t>(),
                );
                (*s).local_len = size_of::<net_sockaddr_in6_t>();
                (*s).bound = 1;
            }
        }
        let mut i = 0usize;
        while i < NET_MAX_SOCKETS {
            let dst = unsafe { g_sockets[i] };
            if !dst.is_null()
                && unsafe {
                    (*dst).closed == 0
                        && (*dst).domain == AF_INET6
                        && (*dst).type_ == SOCK_RAW
                        && (*dst).protocol == (*s).protocol
                }
                && unsafe { raw_ipv6_filter_passes(dst, buf.cast_const(), len) }
            {
                if unsafe { net_enqueue_msg_locked(dst, buf.cast_const(), len, (*s).local.as_ptr().cast(), (*s).local_len) } >= 0 {
                    delivered += 1;
                }
            }
            i += 1;
        }
    }
    if delivered != 0 { len as c_int } else { -ECONNREFUSED }
}

#[no_mangle]
pub unsafe extern "C" fn net_socket_alloc() -> *mut net_socket_t {
    unsafe { a20_socket_core_alloc_raw() }
}

#[no_mangle]
pub unsafe extern "C" fn net_socket_free(s: *mut net_socket_t) {
    unsafe { a20_socket_core_free_raw(s) };
}

#[no_mangle]
pub unsafe extern "C" fn net_block_on_socket_locked(s: *mut net_socket_t, cur: *mut task_t) {
    if s.is_null() || cur.is_null() {
        return;
    }
    unsafe {
        (*s).waiter = cur;
        a20_socket_core_proc_set_wake_time(cur, timer_get_ticks().wrapping_add(a20_socket_core_net_wait_ticks()));
        a20_socket_core_task_set_state(cur, PROC_BLOCKED);
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_clear_socket_waiter(s: *mut net_socket_t, cur: *mut task_t) {
    let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
    unsafe {
        if net_socket_is_valid_locked(s) != 0 && (*s).waiter == cur {
            (*s).waiter = ptr::null_mut();
        }
        a20_socket_core_proc_set_wake_time(cur, 0);
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_task_has_unblocked_signal(t: *mut task_t) -> c_int {
    unsafe { a20_socket_core_task_has_unblocked_signal_impl(t) }
}

#[no_mangle]
pub unsafe extern "C" fn net_socket_wait_expired(
    s: *mut net_socket_t,
    start: u64,
    for_write: c_int,
) -> c_int {
    if s.is_null() {
        return 0;
    }
    let timeout = unsafe {
        if for_write != 0 {
            (*s).send_timeout_ticks
        } else {
            (*s).recv_timeout_ticks
        }
    };
    if timeout != 0 && (unsafe { timer_get_ticks() } as i64 - start.wrapping_add(timeout) as i64) >= 0 {
        1
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_find_bound_socket_locked(
    domain: c_int,
    type_: c_int,
    addr: *const c_void,
    addrlen: usize,
) -> *mut net_socket_t {
    if domain == AF_UNIX {
        let mut i = 0usize;
        while i < NET_MAX_SOCKETS {
            let s = unsafe { g_sockets[i] };
            if !s.is_null()
                && unsafe { (*s).bound != 0 && (*s).domain == domain && (*s).type_ == type_ }
                && unsafe { (*s).local_len == addrlen }
            {
                let lhs = unsafe { core::slice::from_raw_parts((*s).local.as_ptr(), addrlen) };
                let rhs = if addr.is_null() {
                    &[][..]
                } else {
                    unsafe { core::slice::from_raw_parts(addr as *const u8, addrlen) }
                };
                if lhs == rhs {
                    return s;
                }
            }
            i += 1;
        }
        return ptr::null_mut();
    }

    let mut port = 0u16;
    if unsafe { net_sockaddr_port(addr, addrlen, &mut port) } < 0 {
        return ptr::null_mut();
    }
    let mut i = 0usize;
    while i < NET_MAX_SOCKETS {
        let s = unsafe { g_sockets[i] };
        if !s.is_null() && unsafe { (*s).bound != 0 && (*s).domain == domain && (*s).type_ == type_ } {
            let mut s_port = 0u16;
            if unsafe { net_sockaddr_port((*s).local.as_ptr().cast(), (*s).local_len, &mut s_port) } == 0 && s_port == port {
                return s;
            }
        }
        i += 1;
    }
    if domain == AF_INET {
        let mut j = 0usize;
        while j < NET_MAX_SOCKETS {
            let s = unsafe { g_sockets[j] };
            if !s.is_null() && unsafe { (*s).bound != 0 && (*s).domain == AF_INET6 && (*s).type_ == type_ } {
                let mut s_port = 0u16;
                if unsafe { net_sockaddr_port((*s).local.as_ptr().cast(), (*s).local_len, &mut s_port) } == 0 && s_port == port {
                    return s;
                }
            }
            j += 1;
        }
    }
    if domain == AF_INET6 {
        let mut j = 0usize;
        while j < NET_MAX_SOCKETS {
            let s = unsafe { g_sockets[j] };
            if !s.is_null() && unsafe { (*s).bound != 0 && (*s).domain == AF_INET && (*s).type_ == type_ } {
                let mut s_port = 0u16;
                if unsafe { net_sockaddr_port((*s).local.as_ptr().cast(), (*s).local_len, &mut s_port) } == 0 && s_port == port {
                    return s;
                }
            }
            j += 1;
        }
    }
    ptr::null_mut()
}

#[no_mangle]
pub unsafe extern "C" fn net_init() {
    unsafe { net_socket_registry_init() };
    while unsafe { a20_socket_core_virtio_net_init_once() } == 0 {}
    unsafe { a20_socket_core_lwip_init() };
    unsafe { a20_socket_core_log_init() };
}

#[no_mangle]
pub unsafe extern "C" fn net_format_status(buf: *mut i8, bufsz: usize) -> c_int {
    let cbuf = buf.cast();
    let mut n = unsafe { a20_socket_core_lwip_format_status(cbuf, bufsz) };
    if buf.is_null() || bufsz == 0 {
        return 0;
    }
    if n < 0 {
        n = 0;
    }
    if n as usize >= bufsz {
        return bufsz as c_int - 1;
    }

    let (mut used, mut bound, mut queued) = (0, 0, 0);
    {
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
        let mut i = 0usize;
        while i < NET_MAX_SOCKETS {
            let s = unsafe { g_sockets[i] };
            if !s.is_null() {
                used += 1;
                if unsafe { (*s).bound } != 0 {
                    bound += 1;
                }
                queued += unsafe { (*s).rx_count };
            }
            i += 1;
        }
    }
    let m = unsafe {
        a20_socket_core_append_status(cbuf.wrapping_add(n as usize), bufsz - n as usize, used, bound, queued)
    };
    if m > 0 {
        n += m;
    }
    if n as usize >= bufsz {
        bufsz as c_int - 1
    } else {
        n
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_socket_create(domain: c_int, type_: c_int, protocol: c_int) -> c_int {
    let base_type = type_ & 0xf;
    if domain != AF_UNIX && domain != AF_INET && domain != AF_INET6 && domain != AF_ALG {
        return -EAFNOSUPPORT;
    }
    if base_type != SOCK_STREAM && base_type != SOCK_DGRAM && base_type != SOCK_RAW && base_type != SOCK_SEQPACKET {
        return -EPROTOTYPE;
    }
    if domain == AF_ALG && base_type != SOCK_SEQPACKET {
        return -EPROTOTYPE;
    }
    if base_type == SOCK_RAW && ((domain != AF_INET && domain != AF_INET6) || !(0..=255).contains(&protocol)) {
        return -EPROTONOSUPPORT;
    }

    if domain == AF_INET || domain == AF_INET6 {
        if base_type == SOCK_STREAM && protocol != 0 && protocol != IPPROTO_TCP {
            return -EPROTONOSUPPORT;
        }
        if base_type == SOCK_DGRAM && protocol != 0 && protocol != IPPROTO_UDP {
            return -EPROTONOSUPPORT;
        }
        if base_type == SOCK_RAW && protocol == IPPROTO_TCP {
            return -EPROTONOSUPPORT;
        }
        if base_type == SOCK_RAW && unsafe { a20_socket_core_current_has_cap_net_raw() } == 0 {
            return -EACCES;
        }
    }

    let s = unsafe { net_socket_alloc() };
    if s.is_null() {
        return -ENOMEM;
    }
    unsafe {
        (*s).domain = domain;
        (*s).type_ = base_type;
        (*s).protocol = protocol;
        (*s).nonblock = if (type_ & SOCK_NONBLOCK) != 0 { 1 } else { 0 };
        (*s).bpf_prog_fd = -1;
        (*s).ipv6_checksum_offset = -1;
    }
    let init_r = unsafe { net_inet_socket_init(s) };
    if init_r < 0 {
        unsafe { net_socket_free(s) };
        return init_r;
    }

    let reg_r = {
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
        unsafe { net_register_socket_locked(s) }
    };
    if reg_r < 0 {
        unsafe {
            net_inet_socket_destroy(s);
            net_socket_free(s);
        }
        return reg_r;
    }
    let gfd = unsafe { net_socket_install_file(s, type_) };
    if gfd < 0 {
        {
            let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
            unsafe {
                (*s).closed = 1;
                net_unregister_socket_locked(s);
            }
        }
        unsafe {
            net_inet_socket_destroy(s);
            a20_socket_core_free_raw(s);
        }
        return gfd;
    }
    gfd
}

#[no_mangle]
pub unsafe extern "C" fn net_socketpair_create(
    domain: c_int,
    type_: c_int,
    protocol: c_int,
    out_gfd: *mut c_int,
) -> c_int {
    if domain != AF_UNIX {
        return -EOPNOTSUPP;
    }
    if out_gfd.is_null() {
        return -EFAULT;
    }
    let a = unsafe { net_socket_create(domain, type_, protocol) };
    if a < 0 {
        return a;
    }
    let b = unsafe { net_socket_create(domain, type_, protocol) };
    if b < 0 {
        unsafe { vfs_close(a) };
        return b;
    }
    let sa = unsafe { net_socket_from_file(a) };
    let sb = unsafe { net_socket_from_file(b) };
    if sa.is_null() || sb.is_null() {
        unsafe {
            vfs_close(a);
            vfs_close(b);
        }
        return -ENOTSOCK;
    }
    {
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
        unsafe {
            (*sa).peer = sb;
            (*sb).peer = sa;
            (*sa).connected = 1;
            (*sb).connected = 1;
        }
    }
    unsafe {
        *out_gfd.add(0) = a;
        *out_gfd.add(1) = b;
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn net_bind(gfd: c_int, addr: *const c_void, addrlen: usize) -> c_int {
    let s = unsafe { net_socket_from_file(gfd) };
    if s.is_null() {
        return -ENOTSOCK;
    }
    let family = unsafe { sockaddr_family(addr, addrlen) };
    if family < 0 {
        return family;
    }
    let mut bind_family = family;
    if bind_family == AF_UNSPEC && unsafe { (*s).domain == AF_INET || (*s).domain == AF_INET6 } {
        bind_family = unsafe { (*s).domain };
    }
    if bind_family != unsafe { (*s).domain } {
        return -EAFNOSUPPORT;
    }
    if addrlen > NET_SOCKADDR_MAX {
        return -EINVAL;
    }
    if unsafe { (*s).domain == AF_INET } && addrlen < size_of::<net_sockaddr_in_t>() {
        return -EINVAL;
    }
    if unsafe { (*s).domain == AF_INET6 } && addrlen < size_of::<net_sockaddr_in6_t>() {
        return -EINVAL;
    }

    let mut bind_addr = [0u8; NET_SOCKADDR_MAX];
    unsafe {
        if addrlen != 0 {
            ptr::copy_nonoverlapping(addr as *const u8, bind_addr.as_mut_ptr(), addrlen);
        }
    }
    if family == AF_UNSPEC && unsafe { (*s).domain == AF_INET || (*s).domain == AF_INET6 } {
        let fam = (unsafe { (*s).domain } as u16).to_ne_bytes();
        bind_addr[0] = fam[0];
        bind_addr[1] = fam[1];
    }
    let bind_len = addrlen;

    if unsafe { (*s).bound } != 0 {
        if unsafe { (*s).type_ } != SOCK_RAW {
            return -EINVAL;
        }
        unsafe {
            ptr::copy_nonoverlapping(bind_addr.as_ptr(), (*s).local.as_mut_ptr(), bind_len);
            (*s).local_len = bind_len;
        }
        return 0;
    }
    if unsafe { (*s).domain } == AF_ALG {
        return unsafe { net_alg_socket_bind(s, addr, addrlen) };
    }
    if unsafe { (*s).domain } == AF_UNIX {
        return unsafe { net_unix_socket_bind(s, addr, addrlen) };
    }

    if unsafe { (*s).domain == AF_INET || (*s).domain == AF_INET6 } {
        let mut port = 0u16;
        if unsafe { (*s).domain == AF_INET } {
            let in4 = bind_addr.as_ptr() as *const net_sockaddr_in_t;
            if unsafe { net_sockaddr_in_local(in4) } == 0 {
                return -EADDRNOTAVAIL;
            }
            if unsafe { net_sockaddr_port(bind_addr.as_ptr().cast(), addrlen, &mut port) } == 0 {
                let host_port = unsafe { net_ntohs(port) };
                if host_port < 1024 && host_port != 0 && unsafe { a20_socket_core_current_euid() } != 0 {
                    return -EACCES;
                }
            }
        }
        if unsafe { net_sockaddr_port(bind_addr.as_ptr().cast(), addrlen, &mut port) } == 0 && port == 0 {
            unsafe { net_sockaddr_set_port(bind_addr.as_mut_ptr().cast(), addrlen, net_alloc_ephemeral_port_locked()) };
        }
    }

    {
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
        if unsafe { (*s).domain == AF_INET || (*s).domain == AF_INET6 }
            && !unsafe { net_find_bind_conflict_locked(s, bind_addr.as_ptr().cast(), bind_len) }.is_null()
        {
            return -EADDRINUSE;
        }
        unsafe {
            ptr::copy_nonoverlapping(bind_addr.as_ptr(), (*s).local.as_mut_ptr(), bind_len);
            (*s).local_len = bind_len;
            (*s).bound = 1;
        }
    }
    unsafe { net_inet_bind_pcb(s, bind_addr.as_ptr().cast(), addrlen) }
}

#[no_mangle]
pub unsafe extern "C" fn net_connect(gfd: c_int, addr: *const c_void, addrlen: usize) -> c_int {
    let s = unsafe { net_socket_from_file(gfd) };
    if s.is_null() {
        return -ENOTSOCK;
    }
    if addr.is_null() || addrlen > NET_SOCKADDR_MAX {
        return -EINVAL;
    }
    if unsafe { (*s).connected } != 0 {
        return -EISCONN;
    }
    if unsafe { (*s).domain } == AF_UNIX {
        return unsafe { net_unix_socket_connect(s, addr, addrlen) };
    }

    let family = unsafe { sockaddr_family(addr, addrlen) };
    if family != unsafe { (*s).domain } && !(unsafe { (*s).domain == AF_INET6 } && family == AF_INET) {
        return -EAFNOSUPPORT;
    }

    {
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
        if unsafe { (*s).bound == 0 && ((*s).domain == AF_INET || (*s).domain == AF_INET6) } {
            unsafe { net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked()) };
        }
        unsafe {
            ptr::copy_nonoverlapping(addr as *const u8, (*s).peer_addr.as_mut_ptr(), addrlen);
            (*s).peer_len = addrlen;
        }
    }

    let r = unsafe { net_inet_connect(s, addr, addrlen, addr, addrlen) };
    if r < 0 && r != -EINPROGRESS {
        return r;
    }
    if r == 0 && unsafe { (*s).connected } == 0 {
        unsafe { (*s).connected = 1 };
    }
    r
}

#[no_mangle]
pub unsafe extern "C" fn net_sendto(
    gfd: c_int,
    buf: *const c_void,
    len: usize,
    flags: c_int,
    addr: *const c_void,
    addrlen: usize,
) -> c_int {
    let s = if gfd >= 0 { unsafe { net_socket_from_file(gfd) } } else { ptr::null_mut() };
    if s.is_null() {
        return -ENOTSOCK;
    }
    if len > NET_MAX_PAYLOAD && unsafe { (*s).type_ } != SOCK_STREAM {
        return -EMSGSIZE;
    }
    let dontwait = unsafe { (*s).nonblock != 0 || (flags & MSG_DONTWAIT) != 0 };
    if unsafe { (*s).domain } == AF_ALG {
        return unsafe { net_alg_socket_send(s, buf, len) };
    }
    if unsafe { (*s).domain == AF_INET6 && (*s).type_ == SOCK_RAW } {
        return unsafe { net_sendto_raw_ipv6(s, buf as *mut c_void, len, addr, addrlen) };
    }
    if unsafe {
        ((*s).domain == AF_INET || (*s).domain == AF_INET6)
            && (!(*s).udp.is_null() || !(*s).raw.is_null() || !(*s).tcp.is_null())
    } {
        return unsafe { net_inet_sendto(s, buf, len, flags, addr, addrlen) };
    }
    if unsafe { (*s).domain } == AF_UNIX {
        return unsafe { net_unix_socket_sendto(s, buf, len, addr, addrlen) };
    }

    let mut dst: *mut net_socket_t = ptr::null_mut();
    let mut dst_addr = addr;
    let mut dst_len = addrlen;
    {
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
        if unsafe { (*s).closed != 0 || (*s).shut_wr != 0 } {
            return -EPIPE;
        }
        if unsafe { (*s).bound == 0 && ((*s).domain == AF_INET || (*s).domain == AF_INET6) } {
            unsafe { net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked()) };
        }

        if dst_addr.is_null() && unsafe { (*s).connected } != 0 {
            dst_addr = unsafe { (*s).peer_addr.as_ptr().cast() };
            dst_len = unsafe { (*s).peer_len };
        }
        if unsafe { !(*s).peer.is_null() && ((*s).type_ == SOCK_STREAM || (*s).type_ == SOCK_SEQPACKET || net_socket_is_valid_locked((*s).peer) != 0) } {
            dst = unsafe { (*s).peer };
        } else {
            if unsafe { !(*s).peer.is_null() } {
                unsafe { (*s).peer = ptr::null_mut() };
            }
            if !dst_addr.is_null() {
                dst = unsafe { net_find_bound_socket_locked((*s).domain, (*s).type_, dst_addr, dst_len) };
            }
        }
        if dst.is_null() {
            return if dst_addr.is_null() { -EDESTADDRREQ } else { -ECONNREFUSED };
        }
        if len <= NET_MAX_PAYLOAD {
            if unsafe { (*dst).rx_count >= NET_MAX_QUEUE } && !dontwait {
                return unsafe {
                    net_enqueue_msg_blocking(
                        s,
                        dst,
                        buf,
                        len,
                        (*s).local.as_ptr().cast(),
                        (*s).local_len,
                        0,
                        (*s).send_timeout_ticks,
                    )
                };
            }
            return unsafe { net_enqueue_msg_locked(dst, buf, len, (*s).local.as_ptr().cast(), (*s).local_len) };
        }
    }

    let mut total = 0usize;
    while total < len {
        let chunk = cmp::min(len - total, NET_MAX_PAYLOAD);
        let r = unsafe {
            net_enqueue_msg_blocking(
                s,
                dst,
                (buf as *const u8).add(total).cast(),
                chunk,
                (*s).local.as_ptr().cast(),
                (*s).local_len,
                if dontwait { 1 } else { 0 },
                (*s).send_timeout_ticks,
            )
        };
        if r < 0 {
            return if total != 0 { total as c_int } else { r };
        }
        total += r as usize;
    }
    total as c_int
}

#[no_mangle]
pub unsafe extern "C" fn net_recvfrom_meta(
    gfd: c_int,
    buf: *mut c_void,
    len: usize,
    flags: c_int,
    addr: *mut c_void,
    addrlen: *mut usize,
    meta: *mut net_recv_meta_t,
) -> c_int {
    let s = unsafe { net_socket_from_file(gfd) };
    if s.is_null() {
        return -ENOTSOCK;
    }
    if unsafe { (*s).domain } == AF_ALG {
        return unsafe { net_alg_socket_recv(s, buf, len) };
    }
    let dontwait = (flags & MSG_DONTWAIT) != 0;
    let start = unsafe { timer_get_ticks() };

    loop {
        unsafe { a20_socket_core_lwip_poll() };
        let r = {
            let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
            let mut r = unsafe { net_dequeue_msg_locked_meta(s, buf, len, addr, addrlen, meta) };
            if r > 0 && unsafe { (*s).type_ } == SOCK_STREAM {
                let mut total = r as usize;
                while total < len && unsafe { !(*s).rx_head.is_null() } {
                    let nr = unsafe {
                        net_dequeue_msg_locked_meta(
                            s,
                            (buf as *mut u8).add(total).cast(),
                            len - total,
                            ptr::null_mut(),
                            ptr::null_mut(),
                            ptr::null_mut(),
                        )
                    };
                    if nr <= 0 {
                        break;
                    }
                    total += nr as usize;
                }
                r = total as c_int;
            }
            if r != -EAGAIN
                || unsafe { (*s).nonblock != 0 || (*s).closed != 0 || (*s).peer_closed != 0 || (*s).shut_rd != 0 }
                || dontwait
            {
                if r == -EAGAIN && unsafe { (*s).closed != 0 || (*s).peer_closed != 0 || (*s).shut_rd != 0 } {
                    r = 0;
                }
                r
            } else {
                let cur = unsafe { a20_socket_core_proc_current() };
                if cur.is_null() {
                    return -EAGAIN;
                }
                if unsafe { net_task_has_unblocked_signal(cur) } != 0 {
                    return -ERESTARTSYS;
                }
                if unsafe { net_socket_wait_expired(s, start, 0) } != 0 {
                    return -EAGAIN;
                }
                unsafe { net_block_on_socket_locked(s, cur) };
                -EAGAIN
            }
        };

        if r != -EAGAIN || unsafe { (*s).nonblock != 0 || (*s).closed != 0 || (*s).peer_closed != 0 || (*s).shut_rd != 0 } || dontwait {
            if r > 0 && unsafe { (*s).type_ } == SOCK_STREAM {
                unsafe { net_tcp_recved(s, r as usize) };
            }
            return r;
        }

        unsafe { a20_socket_core_sched() };
        let cur = unsafe { a20_socket_core_proc_current() };
        if !cur.is_null() {
            unsafe { net_clear_socket_waiter(s, cur) };
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_recvfrom(
    gfd: c_int,
    buf: *mut c_void,
    len: usize,
    flags: c_int,
    addr: *mut c_void,
    addrlen: *mut usize,
) -> c_int {
    unsafe { net_recvfrom_meta(gfd, buf, len, flags, addr, addrlen, ptr::null_mut()) }
}
