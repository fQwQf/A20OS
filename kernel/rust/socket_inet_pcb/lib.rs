#![no_std]
#![warn(rust_2018_idioms)]

mod ffi;

use a20rust_support::lock::raw_irqsave_lock;
use core::ffi::{c_int, c_void};
use core::mem::{size_of, zeroed};
use core::ptr;

use ffi::*;

#[inline]
unsafe fn wake_if_blocked(task: *mut task_t) {
    if !task.is_null() && unsafe { a20_socket_inet_pcb_task_state(task) } == PROC_BLOCKED {
        unsafe { proc_make_ready(task) };
    }
}

#[inline]
unsafe fn read_family(addr: *const c_void) -> u16 {
    unsafe { *(addr as *const u16) }
}

#[inline]
unsafe fn net_inet_domains_overlap(a: c_int, b: c_int) -> bool {
    a == b || (a == AF_INET && b == AF_INET6) || (a == AF_INET6 && b == AF_INET)
}

#[inline]
unsafe fn net_sockaddr_port_equal(a: *const c_void, alen: usize, b: *const c_void, blen: usize) -> bool {
    let mut ap = 0u16;
    let mut bp = 0u16;
    let ar = unsafe { net_sockaddr_port(a, alen, &mut ap) };
    let br = unsafe { net_sockaddr_port(b, blen, &mut bp) };
    ar == 0 && br == 0 && ap == bp
}

unsafe fn net_sockaddr_is_local_target(addr: *const c_void, len: usize) -> bool {
    if addr.is_null() || len < size_of::<net_sockaddr_in_t>() {
        return false;
    }
    let family = unsafe { read_family(addr) as c_int };
    if family == AF_INET {
        let in4 = unsafe { &*(addr as *const net_sockaddr_in_t) };
        let ip = in4.sin_addr;
        return ip == 0 || ip == 0x0100_007f || ip == 0x0f02_000a;
    }
    if family == AF_INET6 && len >= size_of::<net_sockaddr_in6_t>() {
        let in6 = unsafe { &*(addr as *const net_sockaddr_in6_t) };
        let mut all_zero = true;
        let mut i = 0usize;
        while i < 16 {
            if in6.sin6_addr[i] != 0 {
                all_zero = false;
                break;
            }
            i += 1;
        }
        if all_zero {
            return true;
        }
        let mut j = 0usize;
        while j < 15 {
            if in6.sin6_addr[j] != 0 {
                return false;
            }
            j += 1;
        }
        return in6.sin6_addr[15] == 1;
    }
    false
}

unsafe fn net_find_stream_listener_locked(s: *mut net_socket_t, port: u16) -> *mut net_socket_t {
    let mut i = 0usize;
    while i < NET_MAX_SOCKETS {
        let cand = unsafe { g_sockets[i] };
        if !cand.is_null()
            && unsafe { (*cand).bound != 0 && (*cand).listening != 0 && (*cand).type_ == SOCK_STREAM }
            && unsafe { net_inet_domains_overlap((*cand).domain, (*s).domain) }
        {
            let mut cand_port = 0u16;
            if unsafe { net_sockaddr_port((*cand).local.as_ptr().cast::<c_void>(), (*cand).local_len, &mut cand_port) } == 0
                && cand_port == port
            {
                return cand;
            }
        }
        i += 1;
    }
    ptr::null_mut()
}

unsafe fn net_find_udp_dst_locked(src: *mut net_socket_t, dst_addr: *const c_void, dst_len: usize) -> *mut net_socket_t {
    if src.is_null() || dst_addr.is_null() {
        return ptr::null_mut();
    }
    let mut dst_port = 0u16;
    if unsafe { net_sockaddr_port(dst_addr, dst_len, &mut dst_port) } < 0 {
        return ptr::null_mut();
    }
    let mut fallback: *mut net_socket_t = ptr::null_mut();
    let mut i = 0usize;
    while i < NET_MAX_SOCKETS {
        let cand = unsafe { g_sockets[i] };
        if !cand.is_null()
            && cand != src
            && unsafe { (*cand).bound != 0 && (*cand).type_ == SOCK_DGRAM }
            && unsafe { net_inet_domains_overlap((*cand).domain, (*src).domain) }
        {
            let mut cand_port = 0u16;
            if unsafe { net_sockaddr_port((*cand).local.as_ptr().cast::<c_void>(), (*cand).local_len, &mut cand_port) } >= 0
                && cand_port == dst_port
            {
                if unsafe { (*cand).connected != 0 } {
                    if unsafe {
                        net_sockaddr_port_equal(
                            (*cand).peer_addr.as_ptr().cast::<c_void>(),
                            (*cand).peer_len,
                            (*src).local.as_ptr().cast::<c_void>(),
                            (*src).local_len,
                        )
                    } {
                        return cand;
                    }
                } else if fallback.is_null() {
                    fallback = cand;
                }
            }
        }
        i += 1;
    }
    fallback
}

unsafe fn ensure_bound_stream_or_udp(s: *mut net_socket_t) {
    if s.is_null() {
        return;
    }
    let mut need_bind = false;
    let mut port = 0u16;
    {
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
        if unsafe { (*s).bound == 0 } {
            port = unsafe { net_alloc_ephemeral_port_locked() };
            if unsafe { (*s).domain == AF_INET6 } {
                let mut in6: net_sockaddr_in6_t = unsafe { zeroed() };
                in6.sin6_family = AF_INET6 as u16;
                in6.sin6_port = port;
                in6.sin6_addr[15] = 1;
                unsafe {
                    ptr::copy_nonoverlapping(
                        (&in6 as *const net_sockaddr_in6_t).cast::<u8>(),
                        (*s).local.as_mut_ptr(),
                        size_of::<net_sockaddr_in6_t>(),
                    );
                    (*s).local_len = size_of::<net_sockaddr_in6_t>();
                    (*s).bound = 1;
                }
            } else {
                unsafe { net_sockaddr_loopback(s, port) };
                need_bind = unsafe { !(*s).udp.is_null() };
            }
        }
    }
    if need_bind {
        unsafe {
            let _ = a20_socket_inet_udp_bind_any((*s).udp, net_ntohs(port));
        }
    }
}

unsafe fn net_inet_connect_stream(
    s: *mut net_socket_t,
    addr: *const c_void,
    addrlen: usize,
    connect_addr: *const c_void,
    peer_len: usize,
) -> c_int {
    unsafe { ensure_bound_stream_or_udp(s) };

    let child = unsafe { net_socket_alloc() };
    if child.is_null() {
        return -ENOMEM;
    }
    unsafe { ptr::write_bytes(child, 0, 1) };

    let mut connect_port = 0u16;
    unsafe { net_sockaddr_port(connect_addr, peer_len, &mut connect_port) };
    let local_target = unsafe { net_sockaddr_is_local_target(connect_addr, peer_len) };
    let mut wait_error = 0;
    let mut listener = ptr::null_mut();
    let wait_deadline = unsafe { timer_get_ticks() }
        .wrapping_add(unsafe { a20_socket_inet_pcb_ms_to_ticks(1000) });

    loop {
        let mut should_break = false;
        {
            let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
            listener = unsafe { net_find_stream_listener_locked(s, connect_port) };
            if !listener.is_null()
                || !local_target
                || unsafe { (*s).nonblock != 0 }
                || (unsafe { timer_get_ticks() } as i64 - wait_deadline as i64) >= 0
            {
                should_break = true;
            } else {
                let cur = unsafe { proc_current() };
                if cur.is_null() {
                    should_break = true;
                } else if unsafe { net_task_has_unblocked_signal(cur) } != 0 {
                    wait_error = -EINTR;
                    should_break = true;
                } else {
                    unsafe { net_block_on_socket_locked(s, cur) };
                    drop(_guard);
                    unsafe { sched() };
                    unsafe { net_clear_socket_waiter(s, cur) };
                    if unsafe { net_task_has_unblocked_signal(cur) } != 0 {
                        wait_error = -EINTR;
                        should_break = true;
                    }
                    continue;
                }
            }
        }
        if should_break {
            break;
        }
    }

    if wait_error != 0 {
        unsafe { net_socket_free(child) };
        return wait_error;
    }

    if !listener.is_null() && unsafe { (*listener).listening != 0 && (*listener).accept_count < NET_MAX_QUEUE } {
        let reg_or_qr = {
            let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
            unsafe {
                (*child).domain = (*listener).domain;
                (*child).type_ = SOCK_STREAM;
                (*child).protocol = (*s).protocol;
                (*child).bpf_prog_fd = -1;
                (*child).bound = 1;
                (*child).connected = 1;
                ptr::copy_nonoverlapping((*listener).local.as_ptr(), (*child).local.as_mut_ptr(), (*listener).local_len);
                (*child).local_len = (*listener).local_len;
                ptr::copy_nonoverlapping((*s).local.as_ptr(), (*child).peer_addr.as_mut_ptr(), (*s).local_len);
                (*child).peer_len = (*s).local_len;
                (*child).peer = s;
                (*s).peer = child;
                (*s).connected = 1;
                (*s).local_tcp = 1;
                (*child).local_tcp = 1;
                let rr = net_register_socket_locked(child);
                if rr < 0 {
                    (*s).connected = 0;
                    (*s).peer = ptr::null_mut();
                    rr
                } else {
                    let qr = net_accept_queue_push_locked(listener, child);
                    if qr < 0 {
                        (*s).connected = 0;
                        (*s).peer = ptr::null_mut();
                        net_unregister_socket_locked(child);
                    }
                    qr
                }
            }
        };
        if reg_or_qr < 0 {
            unsafe { net_socket_free(child) };
            return reg_or_qr;
        }
        unsafe { net_tcp_drop_pcb(s) };
        return 0;
    }

    unsafe { net_socket_free(child) };

    if unsafe { (*s).domain != AF_INET || (*s).tcp.is_null() } {
        unsafe { (*s).connected = 0 };
        return -ECONNREFUSED;
    }

    let mut ip: ip_addr_t = unsafe { zeroed() };
    let mut port = 0u16;
    let r = unsafe { net_sockaddr_to_lwip_ip(addr, addrlen, &mut ip, &mut port) };
    if r < 0 {
        return r;
    }
    if unsafe { net_sockaddr_is_local_target(addr, addrlen) } {
        unsafe { (*s).connected = 0 };
        return -ECONNREFUSED;
    }
    if unsafe { (*s).nonblock != 0 } {
        unsafe { (*s).connected = 0 };
        return -EINPROGRESS;
    }

    unsafe {
        (*s).tcp_connecting = 1;
        (*s).tcp_err = ERR_INPROGRESS;
    }
    let e = unsafe { a20_socket_inet_tcp_connect((*s).tcp, &ip, port) };
    if e != ERR_OK {
        unsafe { (*s).tcp_connecting = 0 };
        return -ENETUNREACH;
    }

    let timeout = unsafe {
        if (*s).send_timeout_ticks != 0 {
            (*s).send_timeout_ticks
        } else {
            a20_socket_inet_pcb_connect_timeout_ticks()
        }
    };
    let deadline = unsafe { timer_get_ticks() }.wrapping_add(timeout);
    while unsafe { (*s).tcp_connecting != 0 } {
        if (unsafe { timer_get_ticks() } as i64 - deadline as i64) >= 0 {
            unsafe {
                net_tcp_drop_pcb(s);
                (*s).tcp_connecting = 0;
                (*s).closed = 1;
            }
            return -ETIMEDOUT;
        }
        let cur = unsafe { proc_current() };
        if cur.is_null() {
            unsafe { a20_lwip_poll() };
            continue;
        }
        if unsafe { net_task_has_unblocked_signal(cur) } != 0 {
            unsafe {
                net_tcp_drop_pcb(s);
                (*s).tcp_connecting = 0;
                (*s).connected = 0;
            }
            return -ERESTARTSYS;
        }
        {
            let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
            unsafe { net_block_on_socket_locked(s, cur) };
        }
        unsafe { sched() };
        unsafe { net_clear_socket_waiter(s, cur) };
        if unsafe { net_task_has_unblocked_signal(cur) } != 0 {
            unsafe {
                net_tcp_drop_pcb(s);
                (*s).tcp_connecting = 0;
                (*s).connected = 0;
            }
            return -ERESTARTSYS;
        }
    }
    if unsafe { (*s).tcp_err } != ERR_OK {
        unsafe { (*s).connected = 0 };
        return -ECONNREFUSED;
    }
    0
}

unsafe fn net_inet_send_udp(
    s: *mut net_socket_t,
    buf: *const c_void,
    len: usize,
    flags: c_int,
    addr: *const c_void,
    addrlen: usize,
) -> c_int {
    unsafe { ensure_bound_stream_or_udp(s) };

    let mut local_dst = ptr::null_mut();
    let mut dst_addr = addr;
    let mut dst_len = addrlen;
    {
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
        if dst_addr.is_null() && unsafe { (*s).connected != 0 } {
            dst_addr = unsafe { (*s).peer_addr.as_ptr().cast::<c_void>() };
            dst_len = unsafe { (*s).peer_len };
        }
        if !unsafe { (*s).peer }.is_null() && unsafe { net_socket_is_valid_locked((*s).peer) } != 0 {
            local_dst = unsafe { (*s).peer };
        } else {
            if !unsafe { (*s).peer }.is_null() {
                unsafe { (*s).peer = ptr::null_mut() };
            }
            if !dst_addr.is_null() {
                local_dst = unsafe { net_find_udp_dst_locked(s, dst_addr, dst_len) };
            }
        }
        if !local_dst.is_null() {
            let dontwait = unsafe { (*s).nonblock != 0 || (flags & MSG_DONTWAIT) != 0 } as c_int;
            if unsafe { (*local_dst).rx_count >= NET_MAX_QUEUE } && dontwait == 0 {
                drop(_guard);
                return unsafe {
                    net_enqueue_msg_blocking(
                        s,
                        local_dst,
                        buf,
                        len,
                        (*s).local.as_ptr().cast::<c_void>(),
                        (*s).local_len,
                        dontwait,
                        (*s).send_timeout_ticks,
                    )
                };
            }
            return unsafe {
                net_enqueue_msg_locked(
                    local_dst,
                    buf,
                    len,
                    (*s).local.as_ptr().cast::<c_void>(),
                    (*s).local_len,
                )
            };
        }
    }

    if unsafe { (*s).domain == AF_INET6 } {
        return if dst_addr.is_null() { -EDESTADDRREQ } else { -ECONNREFUSED };
    }
    if !dst_addr.is_null() {
        let mut ip: ip_addr_t = unsafe { zeroed() };
        let mut port = 0u16;
        let r = unsafe { net_sockaddr_to_lwip_ip(dst_addr, dst_len, &mut ip, &mut port) };
        if r < 0 {
            return r;
        }
        let e = unsafe { a20_socket_inet_udp_sendto((*s).udp, buf, len, &ip, port, 0) };
        return if e == ERR_OK { len as c_int } else { -EIO };
    }
    if unsafe { (*s).connected != 0 } {
        let e = unsafe { a20_socket_inet_udp_sendto((*s).udp, buf, len, ptr::null(), 0, 1) };
        return if e == ERR_OK { len as c_int } else { -EIO };
    }
    -EDESTADDRREQ
}

unsafe fn net_inet_send_raw(
    s: *mut net_socket_t,
    buf: *const c_void,
    len: usize,
    addr: *const c_void,
    addrlen: usize,
) -> c_int {
    if !addr.is_null() {
        let mut ip: ip_addr_t = unsafe { zeroed() };
        let r = unsafe { net_sockaddr_to_lwip_ip(addr, addrlen, &mut ip, ptr::null_mut()) };
        if r < 0 {
            return r;
        }
        let e = unsafe { a20_socket_inet_raw_sendto((*s).raw, buf, len, &ip, 0) };
        return if e == ERR_OK { len as c_int } else { -EIO };
    }
    if unsafe { (*s).connected != 0 } {
        let e = unsafe { a20_socket_inet_raw_sendto((*s).raw, buf, len, ptr::null(), 1) };
        return if e == ERR_OK { len as c_int } else { -EIO };
    }
    -EDESTADDRREQ
}

unsafe fn net_inet_send_tcp(s: *mut net_socket_t, buf: *const c_void, len: usize) -> c_int {
    if unsafe { (*s).connected == 0 || (*s).closed != 0 || (*s).shut_wr != 0 } {
        return -ENOTCONN;
    }
    if unsafe { (*s).local_tcp != 0 } {
        let dst = {
            let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
            let dst = unsafe { (*s).peer };
            if dst.is_null() || unsafe { net_socket_is_valid_locked(dst) } == 0 || unsafe { (*dst).closed != 0 } {
                return -ENOTCONN;
            }
            dst
        };
        return unsafe {
            net_enqueue_msg_blocking(
                s,
                dst,
                buf,
                len,
                (*s).local.as_ptr().cast::<c_void>(),
                (*s).local_len,
                (*s).nonblock,
                (*s).send_timeout_ticks,
            )
        };
    }

    let mut sent = 0usize;
    let start = unsafe { timer_get_ticks() };
    while sent < len {
        unsafe { a20_lwip_poll() };
        let room = unsafe {
            if !(*s).tcp.is_null() && (*s).closed == 0 && (*s).connected != 0 {
                a20_socket_inet_tcp_sndbuf((*s).tcp)
            } else {
                0
            }
        };
        let tcp_alive = unsafe { !(*s).tcp.is_null() && (*s).closed == 0 && (*s).connected != 0 };
        if !tcp_alive {
            return if sent != 0 { sent as c_int } else { -EPIPE };
        }
        if room == 0 {
            if sent != 0 || unsafe { (*s).nonblock != 0 } {
                return if sent != 0 { sent as c_int } else { -EAGAIN };
            }
            let cur = unsafe { proc_current() };
            if cur.is_null() {
                return -EAGAIN;
            }
            if unsafe { net_task_has_unblocked_signal(cur) } != 0 {
                return -ERESTARTSYS;
            }
            if unsafe { (*s).send_timeout_ticks != 0 && (timer_get_ticks() as i64 - start.wrapping_add((*s).send_timeout_ticks) as i64) >= 0 } {
                return -EAGAIN;
            }
            {
                let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(g_net_lock)) };
                unsafe { net_block_on_socket_locked(s, cur) };
            }
            unsafe { sched() };
            unsafe { net_clear_socket_waiter(s, cur) };
            continue;
        }
        let mut n = len - sent;
        if n > room as usize {
            n = room as usize;
        }
        if n > 0xffff {
            n = 0xffff;
        }
        let e = unsafe { a20_socket_inet_tcp_write_output((*s).tcp, (buf as *const u8).add(sent).cast::<c_void>(), n as u16) };
        if e != ERR_OK {
            return if sent != 0 { sent as c_int } else { -EIO };
        }
        sent += n;
    }
    unsafe { a20_lwip_poll() };
    sent as c_int
}

#[no_mangle]
pub unsafe extern "C" fn net_tcp_close_pcb(s: *mut net_socket_t) {
    if s.is_null() || unsafe { (*s).tcp.is_null() } {
        return;
    }
    unsafe { a20_socket_inet_tcp_close_socket(s) };
}

#[no_mangle]
pub unsafe extern "C" fn net_tcp_drop_pcb(s: *mut net_socket_t) {
    if s.is_null() || unsafe { (*s).tcp.is_null() } {
        return;
    }
    unsafe { a20_socket_inet_tcp_drop_socket(s) };
}

#[no_mangle]
pub unsafe extern "C" fn net_inet_socket_init(s: *mut net_socket_t) -> c_int {
    if s.is_null() || (unsafe { (*s).domain } != AF_INET && unsafe { (*s).domain } != AF_INET6) {
        return 0;
    }
    if unsafe { (*s).type_ == SOCK_DGRAM } {
        let pcb = unsafe { a20_socket_inet_udp_new((*s).domain, s) };
        if pcb.is_null() {
            return -ENOMEM;
        }
        unsafe { (*s).udp = pcb };
        return 0;
    }
    if unsafe { (*s).type_ == SOCK_RAW } {
        let pcb = unsafe { a20_socket_inet_raw_new((*s).domain, (*s).protocol, s) };
        if pcb.is_null() {
            return -ENOMEM;
        }
        unsafe { (*s).raw = pcb };
        return 0;
    }
    if unsafe { (*s).domain == AF_INET && (*s).type_ == SOCK_STREAM } {
        let pcb = unsafe { a20_socket_inet_tcp_new_v4(s) };
        if pcb.is_null() {
            return -ENOMEM;
        }
        unsafe { (*s).tcp = pcb };
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn net_inet_socket_destroy(s: *mut net_socket_t) {
    if s.is_null() {
        return;
    }
    if unsafe { !(*s).udp.is_null() } {
        unsafe {
            a20_socket_inet_udp_remove((*s).udp);
            (*s).udp = ptr::null_mut();
        }
    }
    if unsafe { !(*s).raw.is_null() } {
        unsafe {
            a20_socket_inet_raw_remove((*s).raw);
            (*s).raw = ptr::null_mut();
        }
    }
    if unsafe { !(*s).tcp.is_null() } {
        let pcb = unsafe { (*s).tcp };
        unsafe {
            (*s).tcp = ptr::null_mut();
            a20_socket_inet_tcp_destroy_abort(pcb);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_inet_bind_pcb(
    s: *mut net_socket_t,
    addr: *const c_void,
    addrlen: usize,
) -> c_int {
    if s.is_null() || (unsafe { (*s).domain } != AF_INET && unsafe { (*s).domain } != AF_INET6) {
        return 0;
    }
    if unsafe { (*s).domain == AF_INET6 } {
        return 0;
    }
    if unsafe { !(*s).udp.is_null() } {
        let mut ip: ip_addr_t = unsafe { zeroed() };
        let mut port = 0u16;
        let r = unsafe { net_sockaddr_to_lwip_ip(addr, addrlen, &mut ip, &mut port) };
        if r < 0 {
            return r;
        }
        let e = unsafe { a20_socket_inet_udp_bind((*s).udp, &ip, port) };
        return if e == ERR_OK { 0 } else { -EADDRINUSE };
    }
    if unsafe { !(*s).raw.is_null() } {
        let mut ip: ip_addr_t = unsafe { zeroed() };
        let r = unsafe { net_sockaddr_to_lwip_ip(addr, addrlen, &mut ip, ptr::null_mut()) };
        if r < 0 {
            return r;
        }
        let e = unsafe { a20_socket_inet_raw_bind((*s).raw, &ip) };
        return if e == ERR_OK { 0 } else { -EADDRINUSE };
    }
    if unsafe { !(*s).tcp.is_null() } {
        let mut ip: ip_addr_t = unsafe { zeroed() };
        let mut port = 0u16;
        let r = unsafe { net_sockaddr_to_lwip_ip(addr, addrlen, &mut ip, &mut port) };
        if r < 0 {
            return r;
        }
        let e = unsafe { a20_socket_inet_tcp_bind((*s).tcp, &ip, port) };
        return if e == ERR_OK { 0 } else { -EADDRINUSE };
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn net_inet_connect(
    s: *mut net_socket_t,
    addr: *const c_void,
    addrlen: usize,
    connect_addr: *const c_void,
    peer_len: usize,
) -> c_int {
    if s.is_null() || (unsafe { (*s).domain } != AF_INET && unsafe { (*s).domain } != AF_INET6) {
        return 0;
    }
    if unsafe { !(*s).udp.is_null() && (*s).domain == AF_INET6 } {
        return 0;
    }
    if unsafe { !(*s).udp.is_null() && (*s).domain == AF_INET } {
        let mut ip: ip_addr_t = unsafe { zeroed() };
        let mut port = 0u16;
        let r = unsafe { net_sockaddr_to_lwip_ip(addr, addrlen, &mut ip, &mut port) };
        if r < 0 {
            return r;
        }
        let e = unsafe { a20_socket_inet_udp_connect((*s).udp, &ip, port) };
        return if e == ERR_OK { 0 } else { -ENETUNREACH };
    }
    if unsafe { !(*s).raw.is_null() } {
        let mut ip: ip_addr_t = unsafe { zeroed() };
        let r = unsafe { net_sockaddr_to_lwip_ip(addr, addrlen, &mut ip, ptr::null_mut()) };
        if r < 0 {
            return r;
        }
        let e = unsafe { a20_socket_inet_raw_connect((*s).raw, &ip) };
        return if e == ERR_OK { 0 } else { -ENETUNREACH };
    }
    if unsafe { (*s).type_ == SOCK_STREAM } {
        return unsafe { net_inet_connect_stream(s, addr, addrlen, connect_addr, peer_len) };
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn net_inet_sendto(
    s: *mut net_socket_t,
    buf: *const c_void,
    len: usize,
    flags: c_int,
    addr: *const c_void,
    addrlen: usize,
) -> c_int {
    if s.is_null() || (unsafe { (*s).domain } != AF_INET && unsafe { (*s).domain } != AF_INET6) {
        return -EAFNOSUPPORT;
    }
    if unsafe { !(*s).udp.is_null() } {
        return unsafe { net_inet_send_udp(s, buf, len, flags, addr, addrlen) };
    }
    if unsafe { !(*s).raw.is_null() } {
        return unsafe { net_inet_send_raw(s, buf, len, addr, addrlen) };
    }
    if unsafe { !(*s).tcp.is_null() } {
        return unsafe { net_inet_send_tcp(s, buf, len) };
    }
    -EOPNOTSUPP
}

#[no_mangle]
pub unsafe extern "C" fn net_inet_accept_child_ready(s: *mut net_socket_t) {
    if !s.is_null() && unsafe { !(*s).tcp.is_null() } {
        unsafe { a20_socket_inet_tcp_backlog_accepted((*s).tcp) };
    }
}

#[no_mangle]
pub unsafe extern "C" fn net_tcp_recved(s: *mut net_socket_t, len: usize) {
    if !s.is_null() && unsafe { !(*s).tcp.is_null() } && len > 0 {
        unsafe { a20_socket_inet_tcp_recved((*s).tcp, len) };
    }
}
