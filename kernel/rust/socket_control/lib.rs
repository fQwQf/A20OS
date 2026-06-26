#![no_std]
#![warn(rust_2018_idioms)]

mod ffi;

use a20rust_support::lock::raw_irqsave_lock;
use core::cmp;
use core::ffi::{c_int, c_void};
use core::mem::size_of;
use core::ptr;

use ffi::*;

const SHUT_RD: c_int = 0;
const SHUT_WR: c_int = 1;
const SHUT_RDWR: c_int = 2;

#[inline]
unsafe fn wake_if_blocked(task: *mut task_t) {
    if !task.is_null() && unsafe { a20_socket_control_task_state(task) } == PROC_BLOCKED {
        unsafe { a20_socket_control_proc_make_ready(task) };
    }
}

#[inline]
unsafe fn timeval_to_ticks(optval: *const c_void, optlen: usize) -> u64 {
    if optval.is_null() || optlen < size_of::<i64>() * 2 {
        return 0;
    }
    let tv = optval as *const i64;
    let sec = unsafe { *tv };
    let usec = unsafe { *tv.add(1) };
    if sec < 0 || usec < 0 {
        return 0;
    }
    let ticks = (sec as u64)
        .saturating_mul(unsafe { a20_socket_control_ticks_per_sec() })
        .saturating_add((usec as u64).saturating_mul(unsafe { a20_socket_control_ticks_per_sec() }) / 1_000_000);
    if ticks == 0 { 1 } else { ticks }
}

#[inline]
unsafe fn copyout_int(optval: *mut c_void, optlen: *mut usize, val: c_int) -> c_int {
    if optval.is_null() || optlen.is_null() || unsafe { *optlen } < size_of::<c_int>() {
        return -EINVAL;
    }
    unsafe {
        ptr::write(optval as *mut c_int, val);
        *optlen = size_of::<c_int>();
    }
    0
}

#[inline]
unsafe fn read_int(optval: *const c_void, optlen: usize) -> Result<c_int, c_int> {
    if optval.is_null() || optlen < size_of::<c_int>() {
        return Err(-EINVAL);
    }
    Ok(unsafe { *(optval as *const c_int) })
}

#[inline]
unsafe fn copy_addr_out(addr: *mut c_void, addrlen: *mut usize, src: *const u8, src_len: usize) {
    let n = cmp::min(src_len, unsafe { *addrlen });
    if n != 0 {
        unsafe { ptr::copy_nonoverlapping(src, addr as *mut u8, n) };
    }
    unsafe { *addrlen = n };
}

#[inline]
unsafe fn is_alg_name(socket: *mut net_socket_t, expected: &'static [u8]) -> bool {
    unsafe { a20_socket_control_alg_is((*socket).alg_name.as_ptr().cast(), expected.as_ptr().cast()) != 0 }
}

#[inline]
unsafe fn is_alg_type(socket: *mut net_socket_t, expected: &'static [u8]) -> bool {
    unsafe { a20_socket_control_alg_is((*socket).alg_type.as_ptr().cast(), expected.as_ptr().cast()) != 0 }
}

unsafe fn detach_child_after_install_failure(child: *mut net_socket_t) {
    let mut peer_waiter = ptr::null_mut();
    let mut peer_send_waiter = ptr::null_mut();
    {
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
        unsafe {
            (*child).closed = 1;
            let peer = (*child).peer;
            if !peer.is_null() && (*peer).peer == child {
                (*peer).peer = ptr::null_mut();
                (*peer).peer_closed = 1;
                peer_waiter = (*peer).waiter;
                peer_send_waiter = (*peer).send_waiter;
            }
            net_unregister_socket_locked(child);
        }
    }
    unsafe {
        wake_if_blocked(peer_waiter);
        wake_if_blocked(peer_send_waiter);
        net_inet_socket_destroy(child);
        net_socket_free(child);
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_listen(gfd: c_int, mut backlog: c_int) -> c_int {
    let s = unsafe { net_socket_from_file(gfd) };
    if s.is_null() {
        return -ENOTSOCK;
    }
    if unsafe { (*s).domain } == AF_ALG {
        return 0;
    }
    if (unsafe { (*s).type_ } != SOCK_STREAM
        && !(unsafe { (*s).domain } == AF_UNIX && unsafe { (*s).type_ } == SOCK_SEQPACKET))
        || (unsafe { (*s).domain } != AF_INET
            && unsafe { (*s).domain } != AF_INET6
            && unsafe { (*s).domain } != AF_UNIX)
    {
        return -EOPNOTSUPP;
    }
    if unsafe { (*s).domain } == AF_UNIX && unsafe { (*s).bound } == 0 {
        return -EINVAL;
    }
    if unsafe { (*s).listening } != 0 {
        return 0;
    }
    if backlog <= 0 {
        backlog = 1;
    }
    if backlog > NET_MAX_QUEUE {
        backlog = NET_MAX_QUEUE;
    }
    unsafe {
        (*s).listening = 1;
        if (*s).domain == AF_INET || (*s).domain == AF_INET6 {
            (*s).local_tcp = 1;
            if (*s).domain == AF_INET {
                net_tcp_drop_pcb(s);
            }
        }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn net_accept(
    gfd: c_int,
    addr: *mut c_void,
    addrlen: *mut usize,
    flags: c_int,
) -> c_int {
    if flags & !(SOCK_CLOEXEC | SOCK_NONBLOCK) != 0 {
        return -EINVAL;
    }
    let s = unsafe { net_socket_from_file(gfd) };
    if s.is_null() {
        return -ENOTSOCK;
    }
    if unsafe { (*s).domain } == AF_ALG {
        return unsafe { net_alg_socket_accept(s, addrlen, flags) };
    }
    if (unsafe { (*s).type_ } != SOCK_STREAM
        && !(unsafe { (*s).domain } == AF_UNIX && unsafe { (*s).type_ } == SOCK_SEQPACKET))
        || (unsafe { (*s).domain } != AF_INET
            && unsafe { (*s).domain } != AF_INET6
            && unsafe { (*s).domain } != AF_UNIX)
    {
        return -EOPNOTSUPP;
    }
    if unsafe { (*s).listening } == 0 {
        return -EINVAL;
    }

    let start = unsafe { timer_get_ticks() };
    let child = loop {
        let maybe_child = {
            let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
            if unsafe { (*s).closed } != 0 {
                return -EINVAL;
            }
            let child = unsafe { net_accept_queue_pop_locked(s) };
            if !child.is_null() {
                Some(child)
            } else if unsafe { (*s).nonblock } != 0 {
                return -EAGAIN;
            } else {
                let cur = unsafe { proc_current() };
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
                None
            }
        };
        if let Some(child) = maybe_child {
            break child;
        }
        unsafe { sched() };
        let cur = unsafe { proc_current() };
        if !cur.is_null() {
            unsafe { net_clear_socket_waiter(s, cur) };
            if unsafe { net_task_has_unblocked_signal(cur) } != 0 {
                return -ERESTARTSYS;
            }
        }
    };

    unsafe { net_inet_accept_child_ready(child) };
    if !addr.is_null() && !addrlen.is_null() && unsafe { *addrlen } > 0 {
        unsafe { copy_addr_out(addr, addrlen, (*child).peer_addr.as_ptr(), (*child).peer_len) };
    }

    unsafe {
        (*child).nonblock = if (flags & SOCK_NONBLOCK) != 0 { 1 } else { (*s).nonblock };
    }
    let newfd = unsafe {
        net_socket_install_file(child, O_RDWR | if (*child).nonblock != 0 { O_NONBLOCK } else { 0 })
    };
    if newfd < 0 {
        unsafe { detach_child_after_install_failure(child) };
    }
    newfd
}

#[no_mangle]
pub unsafe extern "C" fn net_getsockname(gfd: c_int, addr: *mut c_void, addrlen: *mut usize) -> c_int {
    let s = unsafe { net_socket_from_file(gfd) };
    if s.is_null() {
        return -ENOTSOCK;
    }
    if addr.is_null() || addrlen.is_null() {
        return -EFAULT;
    }
    let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
    unsafe {
        if (*s).bound == 0 && ((*s).domain == AF_INET || (*s).domain == AF_INET6) {
            net_sockaddr_loopback(s, net_alloc_ephemeral_port_locked());
        }
        copy_addr_out(addr, addrlen, (*s).local.as_ptr(), (*s).local_len);
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn net_getpeername(gfd: c_int, addr: *mut c_void, addrlen: *mut usize) -> c_int {
    let s = unsafe { net_socket_from_file(gfd) };
    if s.is_null() {
        return -ENOTSOCK;
    }
    if unsafe { (*s).connected } == 0 {
        return -ENOTCONN;
    }
    unsafe { copy_addr_out(addr, addrlen, (*s).peer_addr.as_ptr(), (*s).peer_len) };
    0
}

#[no_mangle]
pub unsafe extern "C" fn net_setsockopt(
    gfd: c_int,
    level: c_int,
    optname: c_int,
    optval: *const c_void,
    optlen: usize,
) -> c_int {
    let s = unsafe { net_socket_from_file(gfd) };
    if s.is_null() {
        return -ENOTSOCK;
    }
    if unsafe { (*s).domain } == AF_ALG && level == SOL_ALG && optname == ALG_SET_KEY {
        if unsafe { is_alg_type(s, b"aead\0") }
            && unsafe { is_alg_name(s, b"authenc(hmac(sha256),cbc(aes))\0") }
            && optlen < 16
        {
            return -EINVAL;
        }
        if (unsafe { is_alg_type(s, b"skcipher\0") } || unsafe { is_alg_type(s, b"aead\0") })
            && optlen != 0
            && optlen < 16
        {
            return -EINVAL;
        }
        return 0;
    }
    if level == IPPROTO_IP {
        if optname == MCAST_JOIN_GROUP {
            return if optlen != 0 { 0 } else { -EINVAL };
        }
        if optname == MCAST_LEAVE_GROUP {
            return -EADDRNOTAVAIL;
        }
        return 0;
    }
    if unsafe { (*s).domain } == AF_INET6 && level == IPPROTO_IPV6 && optname == IPV6_CHECKSUM {
        let offset = match unsafe { read_int(optval, optlen) } {
            Ok(v) => v,
            Err(e) => return e,
        };
        if offset >= 0 && (offset & 1) != 0 {
            return -EINVAL;
        }
        unsafe { (*s).ipv6_checksum_offset = offset };
        return 0;
    }
    if unsafe { (*s).domain } == AF_INET6 && level == IPPROTO_IPV6 && optname == IPV6_V6ONLY {
        return if optlen >= size_of::<c_int>() { 0 } else { -EINVAL };
    }
    if unsafe { (*s).domain } == AF_INET6
        && level == IPPROTO_IPV6
        && matches!(
            optname,
            IPV6_RECVPKTINFO
                | IPV6_RECVTCLASS
                | IPV6_RECVHOPLIMIT
                | IPV6_RECVRTHDR
                | IPV6_RECVHOPOPTS
                | IPV6_RECVDSTOPTS
                | IPV6_RECVERR
                | IPV6_2292PKTINFO
                | IPV6_2292HOPLIMIT
                | IPV6_2292RTHDR
                | IPV6_2292HOPOPTS
                | IPV6_2292DSTOPTS
        )
    {
        let val = match unsafe { read_int(optval, optlen) } {
            Ok(v) => v != 0,
            Err(e) => return e,
        };
        unsafe {
            match optname {
                IPV6_RECVPKTINFO => (*s).ipv6_recv_pktinfo = val as c_int,
                IPV6_RECVTCLASS => (*s).ipv6_recv_tclass = val as c_int,
                IPV6_RECVHOPLIMIT => (*s).ipv6_recv_hoplimit = val as c_int,
                IPV6_RECVRTHDR => (*s).ipv6_recv_rthdr = val as c_int,
                IPV6_RECVHOPOPTS => (*s).ipv6_recv_hopopts = val as c_int,
                IPV6_RECVDSTOPTS => (*s).ipv6_recv_dstopts = val as c_int,
                IPV6_RECVERR => (*s).ipv6_recv_err = val as c_int,
                IPV6_2292PKTINFO => (*s).ipv6_recv_2292_pktinfo = val as c_int,
                IPV6_2292HOPLIMIT => (*s).ipv6_recv_2292_hoplimit = val as c_int,
                IPV6_2292RTHDR => (*s).ipv6_recv_2292_rthdr = val as c_int,
                IPV6_2292HOPOPTS => (*s).ipv6_recv_2292_hopopts = val as c_int,
                IPV6_2292DSTOPTS => (*s).ipv6_recv_2292_dstopts = val as c_int,
                _ => {}
            }
        }
        return 0;
    }
    if unsafe { (*s).domain } == AF_INET6
        && level == IPPROTO_IPV6
        && matches!(
            optname,
            IPV6_UNICAST_IF
                | IPV6_MULTICAST_IF
                | IPV6_MULTICAST_HOPS
                | IPV6_MULTICAST_LOOP
                | IPV6_TCLASS
                | IPV6_HOPLIMIT
                | IPV6_FLOWINFO
                | IPV6_ROUTER_ALERT
        )
    {
        return if optlen >= size_of::<c_int>() { 0 } else { -EINVAL };
    }
    if unsafe { (*s).domain } == AF_INET6
        && level == IPPROTO_IPV6
        && (optname == IPV6_JOIN_GROUP || optname == IPV6_LEAVE_GROUP)
    {
        return 0;
    }
    if unsafe { (*s).domain } == AF_INET6 && level == IPPROTO_IPV6 && optname == IPV6_ADDRFORM {
        let val = match unsafe { read_int(optval, optlen) } {
            Ok(v) => v,
            Err(e) => return e,
        };
        if val != AF_INET {
            return -EINVAL;
        }
        if unsafe { (*s).type_ != SOCK_STREAM || (*s).connected != 0 || (*s).bound != 0 } {
            return -EOPNOTSUPP;
        }
        unsafe { (*s).domain = AF_INET };
        return 0;
    }
    if unsafe { (*s).domain } == AF_INET6
        && unsafe { (*s).type_ } == SOCK_RAW
        && level == IPPROTO_ICMPV6
        && optname == ICMP6_FILTER
    {
        if optval.is_null() || optlen < size_of::<[u32; 8]>() {
            return -EINVAL;
        }
        unsafe {
            ptr::copy_nonoverlapping(optval as *const u32, (*s).icmp6_filter.as_mut_ptr(), 8);
            (*s).icmp6_filter_set = 1;
        }
        return 0;
    }
    if level == IPPROTO_TCP {
        if unsafe { (*s).type_ } != SOCK_STREAM {
            return -ENOPROTOOPT;
        }
        if optname == TCP_CONGESTION {
            return if !optval.is_null() && optlen != 0 { 0 } else { -EINVAL };
        }
        let val = match unsafe { read_int(optval, optlen) } {
            Ok(v) => v,
            Err(e) => return e,
        };
        match optname {
            TCP_NODELAY => {
                unsafe {
                    (*s).tcp_nodelay = (val != 0) as c_int;
                    if !(*s).tcp.is_null() {
                        let flags = a20_socket_control_lwip_lock();
                        if (*s).tcp_nodelay != 0 {
                            a20_socket_control_tcp_nagle_disable((*s).tcp);
                        } else {
                            a20_socket_control_tcp_nagle_enable((*s).tcp);
                        }
                        a20_socket_control_lwip_unlock(flags);
                    }
                }
                return 0;
            }
            TCP_CORK | TCP_MAXSEG | TCP_SYNCNT | TCP_LINGER2 | TCP_DEFER_ACCEPT | TCP_WINDOW_CLAMP
            | TCP_QUICKACK | TCP_USER_TIMEOUT => return 0,
            TCP_KEEPIDLE => {
                if val <= 0 {
                    return -EINVAL;
                }
                unsafe {
                    (*s).keep_idle = val;
                    if !(*s).tcp.is_null() {
                        let flags = a20_socket_control_lwip_lock();
                        a20_socket_control_tcp_set_keep_idle_ms((*s).tcp, (val as u32).saturating_mul(1000));
                        a20_socket_control_lwip_unlock(flags);
                    }
                }
                return 0;
            }
            TCP_KEEPINTVL => {
                if val <= 0 {
                    return -EINVAL;
                }
                unsafe {
                    (*s).keep_intvl = val;
                    if !(*s).tcp.is_null() {
                        let flags = a20_socket_control_lwip_lock();
                        a20_socket_control_tcp_set_keep_intvl_ms((*s).tcp, (val as u32).saturating_mul(1000));
                        a20_socket_control_lwip_unlock(flags);
                    }
                }
                return 0;
            }
            TCP_KEEPCNT => {
                if val <= 0 {
                    return -EINVAL;
                }
                unsafe {
                    (*s).keep_cnt = val;
                    if !(*s).tcp.is_null() {
                        let flags = a20_socket_control_lwip_lock();
                        a20_socket_control_tcp_set_keep_cnt((*s).tcp, val as u32);
                        a20_socket_control_lwip_unlock(flags);
                    }
                }
                return 0;
            }
            _ => return -ENOPROTOOPT,
        }
    }
    if level == SOL_SOCKET && optname == SO_ATTACH_BPF {
        let prog_fd = match unsafe { read_int(optval, optlen) } {
            Ok(v) => v,
            Err(e) => return e,
        };
        if unsafe { bpf_prog_is_loaded(prog_fd) } == 0 {
            return -EBADF;
        }
        unsafe { (*s).bpf_prog_fd = prog_fd };
        return 0;
    }
    if level == SOL_SOCKET && optname == SO_RCVTIMEO {
        if optval.is_null() || optlen < size_of::<i64>() * 2 {
            return -EINVAL;
        }
        unsafe { (*s).recv_timeout_ticks = timeval_to_ticks(optval, optlen) };
        return 0;
    }
    if level == SOL_SOCKET && optname == SO_SNDTIMEO {
        if optval.is_null() || optlen < size_of::<i64>() * 2 {
            return -EINVAL;
        }
        unsafe { (*s).send_timeout_ticks = timeval_to_ticks(optval, optlen) };
        return 0;
    }
    if level == SOL_SOCKET && optname == SO_REUSEADDR {
        let val = match unsafe { read_int(optval, optlen) } {
            Ok(v) => v,
            Err(e) => return e,
        };
        unsafe { (*s).reuseaddr = (val != 0) as c_int };
        return 0;
    }
    if level == SOL_SOCKET && optname == SO_REUSEPORT {
        let val = match unsafe { read_int(optval, optlen) } {
            Ok(v) => v,
            Err(e) => return e,
        };
        unsafe { (*s).reuseport = (val != 0) as c_int };
        return 0;
    }
    if level == SOL_SOCKET && optname == SO_KEEPALIVE {
        let val = match unsafe { read_int(optval, optlen) } {
            Ok(v) => v,
            Err(e) => return e,
        };
        unsafe {
            (*s).keepalive = (val != 0) as c_int;
            if !(*s).tcp.is_null() {
                let flags = a20_socket_control_lwip_lock();
                a20_socket_control_tcp_set_so_keepalive((*s).tcp, (*s).keepalive);
                a20_socket_control_lwip_unlock(flags);
            }
        }
        return 0;
    }
    if level == SOL_SOCKET {
        return 0;
    }
    -EOPNOTSUPP
}

#[no_mangle]
pub unsafe extern "C" fn net_getsockopt(
    gfd: c_int,
    level: c_int,
    optname: c_int,
    optval: *mut c_void,
    optlen: *mut usize,
) -> c_int {
    let s = unsafe { net_socket_from_file(gfd) };
    if s.is_null() {
        return -ENOTSOCK;
    }
    if optval.is_null() || optlen.is_null() {
        return -EINVAL;
    }
    let val: c_int;
    if level == SOL_SOCKET && optname == SO_TYPE {
        val = unsafe { (*s).type_ };
    } else if level == SOL_SOCKET && optname == SO_ERROR {
        val = 0;
    } else if level == SOL_SOCKET && optname == SO_ACCEPTCONN {
        val = unsafe { (*s).listening };
    } else if level == SOL_SOCKET && optname == SO_DOMAIN {
        val = unsafe { (*s).domain };
    } else if level == SOL_SOCKET && optname == SO_PROTOCOL {
        val = unsafe { (*s).protocol };
    } else if level == SOL_SOCKET && (optname == SO_SNDBUF || optname == SO_RCVBUF) {
        val = NET_MAX_QUEUE * NET_MAX_PAYLOAD as c_int;
    } else if level == SOL_SOCKET && optname == SO_REUSEADDR {
        val = unsafe { (*s).reuseaddr };
    } else if level == SOL_SOCKET && optname == SO_REUSEPORT {
        val = unsafe { (*s).reuseport };
    } else if level == SOL_SOCKET && optname == SO_KEEPALIVE {
        val = unsafe { (*s).keepalive };
    } else if level == SOL_SOCKET {
        val = 0;
    } else if level == IPPROTO_TCP {
        if unsafe { (*s).type_ } != SOCK_STREAM {
            return -ENOPROTOOPT;
        }
        if optname == TCP_CONGESTION {
            let congestion = b"cubic\0";
            let n = cmp::min(unsafe { *optlen }, congestion.len());
            if n != 0 {
                unsafe { ptr::copy_nonoverlapping(congestion.as_ptr(), optval as *mut u8, n) };
            }
            unsafe { *optlen = n };
            return 0;
        }
        if optname == TCP_INFO {
            let n = unsafe { *optlen };
            if n != 0 {
                unsafe { ptr::write_bytes(optval as *mut u8, 0, n) };
                unsafe {
                    *(optval as *mut u8) = if (*s).listening != 0 {
                        10
                    } else if (*s).connected != 0 {
                        1
                    } else {
                        7
                    };
                }
            }
            return 0;
        }
        val = match optname {
            TCP_NODELAY => unsafe { (*s).tcp_nodelay },
            TCP_MAXSEG => 1460,
            TCP_CORK | TCP_SYNCNT | TCP_LINGER2 | TCP_DEFER_ACCEPT | TCP_WINDOW_CLAMP | TCP_QUICKACK
            | TCP_USER_TIMEOUT => 0,
            TCP_KEEPIDLE => unsafe {
                if (*s).keep_idle > 0 {
                    (*s).keep_idle
                } else if !(*s).tcp.is_null() {
                    (a20_socket_control_tcp_get_keep_idle_ms((*s).tcp) / 1000) as c_int
                } else {
                    7200
                }
            },
            TCP_KEEPINTVL => unsafe {
                if (*s).keep_intvl > 0 {
                    (*s).keep_intvl
                } else if !(*s).tcp.is_null() {
                    (a20_socket_control_tcp_get_keep_intvl_ms((*s).tcp) / 1000) as c_int
                } else {
                    75
                }
            },
            TCP_KEEPCNT => unsafe {
                if (*s).keep_cnt > 0 {
                    (*s).keep_cnt
                } else if !(*s).tcp.is_null() {
                    a20_socket_control_tcp_get_keep_cnt((*s).tcp) as c_int
                } else {
                    9
                }
            },
            _ => return -ENOPROTOOPT,
        };
    } else if unsafe { (*s).domain } == AF_INET6 && level == IPPROTO_IPV6 {
        val = match optname {
            IPV6_V6ONLY => 0,
            IPV6_RECVPKTINFO => unsafe { (*s).ipv6_recv_pktinfo },
            IPV6_RECVTCLASS => unsafe { (*s).ipv6_recv_tclass },
            IPV6_RECVHOPLIMIT => unsafe { (*s).ipv6_recv_hoplimit },
            IPV6_RECVRTHDR => unsafe { (*s).ipv6_recv_rthdr },
            IPV6_RECVHOPOPTS => unsafe { (*s).ipv6_recv_hopopts },
            IPV6_RECVDSTOPTS => unsafe { (*s).ipv6_recv_dstopts },
            IPV6_RECVERR => unsafe { (*s).ipv6_recv_err },
            IPV6_2292PKTINFO => unsafe { (*s).ipv6_recv_2292_pktinfo },
            IPV6_2292HOPLIMIT => unsafe { (*s).ipv6_recv_2292_hoplimit },
            IPV6_2292RTHDR => unsafe { (*s).ipv6_recv_2292_rthdr },
            IPV6_2292HOPOPTS => unsafe { (*s).ipv6_recv_2292_hopopts },
            IPV6_2292DSTOPTS => unsafe { (*s).ipv6_recv_2292_dstopts },
            IPV6_UNICAST_IF => 0,
            IPV6_MULTICAST_IF => 0,
            IPV6_MULTICAST_HOPS => -1,
            IPV6_MULTICAST_LOOP => 1,
            IPV6_TCLASS => 0,
            IPV6_HOPLIMIT => -1,
            IPV6_FLOWINFO => 0,
            _ => return -ENOPROTOOPT,
        };
    } else {
        return -EOPNOTSUPP;
    }
    unsafe { copyout_int(optval, optlen, val) }
}

#[no_mangle]
pub unsafe extern "C" fn net_shutdown(gfd: c_int, how: c_int) -> c_int {
    let s = unsafe { net_socket_from_file(gfd) };
    if s.is_null() {
        return -ENOTSOCK;
    }

    let mut self_waiter = ptr::null_mut();
    let mut self_send_waiter = ptr::null_mut();
    let mut peer_waiter = ptr::null_mut();
    let mut peer_send_waiter = ptr::null_mut();
    let mut msgs = ptr::null_mut();

    {
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
        unsafe {
            if how == SHUT_RDWR {
                (*s).closed = 1;
                (*s).shut_rd = 1;
                (*s).shut_wr = 1;
            }
            if how == SHUT_WR {
                (*s).shut_wr = 1;
            }
            if how == SHUT_RD {
                (*s).shut_rd = 1;
                msgs = (*s).rx_head;
                (*s).rx_head = ptr::null_mut();
                (*s).rx_tail = ptr::null_mut();
                (*s).rx_count = 0;
            }

            self_waiter = (*s).waiter;
            self_send_waiter = (*s).send_waiter;

            if !(*s).peer.is_null()
                && ((*s).type_ == SOCK_STREAM || (*s).type_ == SOCK_SEQPACKET || net_socket_is_valid_locked((*s).peer) != 0)
                && (*(*s).peer).peer == s
            {
                if how == SHUT_WR || how == SHUT_RDWR {
                    (*(*s).peer).peer_closed = 1;
                    peer_waiter = (*(*s).peer).waiter;
                }
                peer_send_waiter = (*(*s).peer).send_waiter;
            }
        }
    }

    unsafe {
        wake_if_blocked(self_waiter);
        wake_if_blocked(self_send_waiter);
        wake_if_blocked(peer_waiter);
        wake_if_blocked(peer_send_waiter);
    }

    while !msgs.is_null() {
        let next = unsafe { (*msgs).next };
        unsafe { net_msg_free(msgs) };
        msgs = next;
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn net_set_nonblock(gfd: c_int, nonblock: c_int) -> c_int {
    let s = unsafe { net_socket_from_file(gfd) };
    if s.is_null() {
        return -ENOTSOCK;
    }
    let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
    unsafe { (*s).nonblock = if nonblock != 0 { 1 } else { 0 } };
    0
}

#[no_mangle]
pub unsafe extern "C" fn net_poll_events(gfd: c_int, events: i16) -> c_int {
    let s = unsafe { net_socket_from_file(gfd) };
    if s.is_null() {
        return -ENOTSOCK;
    }
    let mut revents: i16 = 0;
    let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
    unsafe {
        if (*s).peer_closed != 0 {
            revents |= POLLHUP;
        } else if (*s).closed != 0 || ((*s).shut_rd != 0 && (*s).shut_wr != 0) {
            revents |= POLLHUP;
        }
        if (events & POLLIN) != 0
            && (!(*s).rx_head.is_null()
                || !(*s).accept_head.is_null()
                || (*s).closed != 0
                || (*s).peer_closed != 0
                || (*s).shut_rd != 0
                || ((*s).domain == AF_ALG
                    && (a20_socket_control_alg_is((*s).alg_type.as_ptr().cast(), b"hash\0".as_ptr().cast()) != 0
                        || (*s).alg_last_len > 0)))
        {
            revents |= POLLIN;
        }
        if (events & POLLOUT) != 0 && (*s).closed == 0 && (*s).shut_wr == 0 {
            if (*s).peer_closed != 0 {
                revents |= POLLERR;
            } else if (*s).type_ == SOCK_STREAM && (*s).connected != 0 && (*s).peer.is_null() {
                revents |= POLLERR;
            } else if !(*s).peer.is_null() && (*s).type_ == SOCK_STREAM && (*(*s).peer).rx_count >= NET_MAX_QUEUE {
            } else {
                revents |= POLLOUT;
            }
        }
    }
    revents as c_int
}
